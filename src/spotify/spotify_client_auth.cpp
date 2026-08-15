// SpotifyClient: getting and keeping a session — the OAuth2 flows (PKCE with a
// loopback redirect, falling back to the device/pairing-code flow), access-token
// refresh, and the client-token + apresolve bring-up that follows.
//
// Part of the SpotifyClient implementation; see spotify_client_internal.h.
#include "spotify_client.h"
#include "spotify_client_internal.h"
#include "settings/app_settings.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>
#include <QWebSocket> // refreshAccessToken reconnects the dealer socket

using namespace spotify_client;

// ---------------------------------------------------------------------------
// OAuth2 device flow
// ---------------------------------------------------------------------------

void SpotifyClient::startAuthorization() {
    startPkceAuthorization();
}

void SpotifyClient::startPkceAuthorization() {
    if (!authServer) {
        authServer = new QTcpServer(this);
        connect(authServer, &QTcpServer::newConnection, this, &SpotifyClient::onAuthServerConnection);
    }
    if (!authServer->isListening() && !authServer->listen(QHostAddress::LocalHost, kOAuthRedirectPort)) {
        logMessage("[auth] local redirect port 36842 is busy; falling back to the pairing-code flow.");
        requestDeviceCode();
        return;
    }

    auto randomUrlSafe = [](int byteCount) {
        QByteArray bytes(byteCount, 0);
        for (char &b : bytes) {
            b = char(QRandomGenerator::system()->bounded(256));
        }
        return QString::fromLatin1(
            bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    };

    codeVerifier = randomUrlSafe(64);
    // Opaque per-attempt value echoed back on the redirect. PKCE already stops an
    // injected code from being exchangeable, but validating state means we reject
    // a forged /login callback outright instead of round-tripping it to Spotify.
    authState = randomUrlSafe(32);
    const QByteArray challenge = QCryptographicHash::hash(codeVerifier.toLatin1(), QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

    QUrlQuery query;
    query.addQueryItem("response_type", "code");
    query.addQueryItem("client_id", kClientIdHex);
    query.addQueryItem("redirect_uri", kOAuthRedirectUri);
    query.addQueryItem("scope", QString(kDeviceFlowScopes).replace(',', ' '));
    query.addQueryItem("code_challenge_method", "S256");
    query.addQueryItem("code_challenge", QString::fromLatin1(challenge));
    query.addQueryItem("state", authState);

    // Don't leave the loopback listener open forever if the user abandons the
    // browser page — it accepts connections from anything local while it's up.
    if (!authServerTimeout) {
        authServerTimeout = new QTimer(this);
        authServerTimeout->setSingleShot(true);
        connect(authServerTimeout, &QTimer::timeout, this, [this]() {
            if (authServer && authServer->isListening()) {
                authServer->close();
                codeVerifier.clear();
                authState.clear();
                logMessage("[auth] authorization window expired; local redirect listener closed.");
            }
        });
    }
    authServerTimeout->start(kAuthWindowMs);

    QUrl url(kAuthorizeUrl);
    url.setQuery(query);

    logMessage("Opening the Spotify authorization page; approve access in your browser.");
    emit authorizationPending(url.toString(), QString());
    QDesktopServices::openUrl(url);
}

void SpotifyClient::onAuthServerConnection() {
    while (QTcpSocket *socket = authServer->nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            const QByteArray requestData = socket->readAll();
            const QString requestLine = QString::fromUtf8(requestData.left(requestData.indexOf("\r\n")));
            const QStringList parts = requestLine.split(' ');
            const QString path = parts.size() >= 2 ? parts.at(1) : QString();

            QString code;
            if (path.startsWith("/login")) {
                const QUrlQuery params(QUrl("http://localhost" + path).query());
                const QString returnedState = params.queryItemValue("state");
                // Reject anything that didn't originate from the authorize URL we
                // just opened: a local process or a page in the browser can reach
                // this port, but can't know this attempt's state value.
                if (authState.isEmpty() || returnedState != authState) {
                    logMessage("[auth] ignoring a redirect with a missing or mismatched state value.");
                } else {
                    code = params.queryItemValue("code");
                }
            }

            const QByteArray page = code.isEmpty()
                ? QByteArray("<html><body style=\"font-family:sans-serif\">Authorization failed - you can close this window.</body></html>")
                : QByteArray("<html><body style=\"font-family:sans-serif\">Overtune is connected - you can close this window.</body></html>");
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " +
                          QByteArray::number(page.size()) + "\r\n\r\n" + page);
            socket->flush();
            socket->disconnectFromHost();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);

            if (!code.isEmpty()) {
                authServer->close();
                if (authServerTimeout) {
                    authServerTimeout->stop();
                }
                authState.clear(); // single-use: a replayed redirect won't match
                exchangeAuthCode(code);
            }
        });
    }
}

void SpotifyClient::exchangeAuthCode(const QString &code) {
    QUrlQuery form;
    form.addQueryItem("grant_type", "authorization_code");
    form.addQueryItem("code", code);
    form.addQueryItem("redirect_uri", kOAuthRedirectUri);
    form.addQueryItem("client_id", kClientIdHex);
    form.addQueryItem("code_verifier", codeVerifier);

    QNetworkRequest request{QUrl(QString::fromLatin1(kTokenUrl))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("User-Agent", kUserAgent);

    QNetworkReply *reply = network->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        if (obj.value("access_token").toString().isEmpty()) {
            logMessage("Authorization code exchange failed: " + QString::fromUtf8(data.left(300)));
            return;
        }
        applyTokenResponse(obj);
        logMessage("Authorized! Bringing up the Spotify session...");
        continueSessionBringUp();
    });
}

void SpotifyClient::requestDeviceCode() {
    QUrlQuery form;
    form.addQueryItem("client_id", kClientIdHex);
    form.addQueryItem("scope", kDeviceFlowScopes);
    form.addQueryItem("intent", "login");
    form.addQueryItem("creation_point",
                      QString("https://login.app.spotify.com/?client_id=%1"
                              "&utm_source=spotify&utm_medium=desktop-win32&utm_campaign=organic")
                          .arg(kClientIdHex));

    QNetworkRequest request{QUrl(QString::fromLatin1(kDeviceAuthorizeUrl))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("User-Agent", kUserAgent);

    logMessage("Requesting device authorization from Spotify...");

    QNetworkReply *reply = network->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray data = reply->readAll();
        const bool netError = reply->error() != QNetworkReply::NoError;
        reply->deleteLater();

        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        deviceCode = obj.value("device_code").toString();
        const QString userCode = obj.value("user_code").toString();
        const QString verifyUrl = obj.value("verification_uri_complete").toString();
        const int interval = obj.value("interval").toInt(5);
        const int expiresIn = obj.value("expires_in").toInt(600);

        if (netError || deviceCode.isEmpty() || verifyUrl.isEmpty()) {
            logMessage("Failed to start device authorization. Response: " + QString::fromUtf8(data.left(300)));
            return;
        }

        deviceFlowDeadlineMs = QDateTime::currentMSecsSinceEpoch() + qint64(expiresIn) * 1000;
        logMessage(QString("Open %1 and confirm the code %2 to authorize.").arg(verifyUrl, userCode));
        emit authorizationPending(verifyUrl, userCode);
        QDesktopServices::openUrl(QUrl(verifyUrl));

        devicePollTimer->start(qMax(1, interval) * 1000);
    });
}

void SpotifyClient::pollDeviceToken() {
    if (deviceCode.isEmpty()) {
        devicePollTimer->stop();
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() > deviceFlowDeadlineMs) {
        devicePollTimer->stop();
        deviceCode.clear();
        logMessage("Device authorization expired before you approved it. Click Connect to try again.");
        return;
    }

    QUrlQuery form;
    form.addQueryItem("client_id", kClientIdHex);
    form.addQueryItem("device_code", deviceCode);
    form.addQueryItem("grant_type", "urn:ietf:params:oauth:grant-type:device_code");

    QNetworkRequest request{QUrl(QString::fromLatin1(kTokenUrl))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("User-Agent", kUserAgent);

    QNetworkReply *reply = network->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        if (!obj.value("access_token").toString().isEmpty()) {
            devicePollTimer->stop();
            deviceCode.clear();
            applyTokenResponse(obj);
            logMessage("Authorized! Bringing up the Spotify session...");
            continueSessionBringUp();
            return;
        }

        const QString err = obj.value("error").toString();
        if (err == "authorization_pending") {
            return; // keep waiting
        }
        if (err == "slow_down") {
            devicePollTimer->setInterval(devicePollTimer->interval() + 5000);
            return;
        }
        devicePollTimer->stop();
        deviceCode.clear();
        logMessage("Device authorization failed: " +
                   (obj.value("error_description").toString().isEmpty()
                        ? err
                        : obj.value("error_description").toString()));
    });
}

void SpotifyClient::applyTokenResponse(const QJsonObject &obj) {
    accessToken = obj.value("access_token").toString();
    const int expiresIn = obj.value("expires_in").toInt(3600);
    accessTokenExpiryMs = QDateTime::currentMSecsSinceEpoch() + qint64(expiresIn) * 1000;
    reauthPromptSent = false;
    scheduleProactiveRefresh(expiresIn);

    const QString newRefresh = obj.value("refresh_token").toString();
    if (!newRefresh.isEmpty()) {
        refreshToken = newRefresh;
        AppSettings::saveRefreshToken(refreshToken);
    }

    // The authorization-code (PKCE) token response carries the canonical
    // username; the device-flow one does not. Capture it when present.
    const QString tokenUsername = obj.value("username").toString();
    if (!tokenUsername.isEmpty() && tokenUsername != username) {
        username = tokenUsername;
        AppSettings::saveUsername(username);
        logMessage(QString("[like] signed in as %1").arg(username));
    }
}

void SpotifyClient::resumeSession() {
    refreshToken = AppSettings::loadRefreshToken();
    if (refreshToken.isEmpty()) {
        logMessage("No stored Spotify session. Open Settings and click Connect Spotify to authorize.");
        return;
    }
    logMessage("Resuming Spotify session from stored credentials...");
    refreshAccessToken();
}

void SpotifyClient::scheduleProactiveRefresh(int expiresInSeconds) {
    if (!tokenRefreshTimer) {
        return;
    }
    // Refresh two minutes before expiry (but no sooner than 30s out), so the
    // token is renewed while still valid instead of lapsing mid-session.
    const int refreshInSec = qMax(30, expiresInSeconds - 120);
    tokenRefreshTimer->start(refreshInSec * 1000);
}

void SpotifyClient::refreshAccessToken() {
    if (refreshToken.isEmpty()) {
        logMessage("Cannot refresh: no refresh token. Please re-authorize (Connect Spotify).");
        if (!reauthPromptSent) {
            reauthPromptSent = true;
            emit reauthorizationRequired();
        }
        return;
    }
    // A burst of 401s (or a proactive refresh coinciding with one) must not
    // fire multiple refreshes: Spotify rotates the refresh token, so a second
    // concurrent refresh would use an already-invalidated token and fail.
    if (refreshInFlight) {
        return;
    }
    refreshInFlight = true;

    QUrlQuery form;
    form.addQueryItem("grant_type", "refresh_token");
    form.addQueryItem("refresh_token", refreshToken);
    form.addQueryItem("client_id", kClientIdHex);

    QNetworkRequest request{QUrl(QString::fromLatin1(kTokenUrl))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("User-Agent", kUserAgent);

    QNetworkReply *reply = network->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        refreshInFlight = false;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool netError = reply->error() != QNetworkReply::NoError;
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        if (!obj.value("access_token").toString().isEmpty()) {
            applyTokenResponse(obj);
            // Only bring the whole session up when it isn't already live; a
            // proactive/mid-session refresh just needs the new token in hand.
            if (webSocket->state() != QAbstractSocket::ConnectedState || !connectStateRegistered) {
                continueSessionBringUp();
            }
            return;
        }

        // Distinguish a dead refresh token from a transient hiccup. Only
        // "invalid_grant" means the credential is truly gone — anything else
        // (network drop, 5xx, rate limit) is retried without wiping it.
        const QString err = obj.value("error").toString();
        if (err == "invalid_grant") {
            logMessage("Token refresh rejected (invalid_grant); the stored session is gone. Please Connect Spotify again.");
            AppSettings::clearRefreshToken();
            refreshToken.clear();
            if (!reauthPromptSent) {
                reauthPromptSent = true;
                emit reauthorizationRequired();
            }
            return;
        }

        logMessage(QString("Token refresh failed transiently (HTTP %1%2); retrying in 30s.")
                       .arg(status).arg(netError ? ", network error" : ""));
        tokenRefreshTimer->start(30 * 1000);
    });
}

bool SpotifyClient::accessTokenExpired() const {
    return accessTokenExpiryMs > 0 &&
           QDateTime::currentMSecsSinceEpoch() >= accessTokenExpiryMs - 30000;
}

// ---------------------------------------------------------------------------
// Session bring-up: client token -> apresolve -> dealer
// ---------------------------------------------------------------------------

void SpotifyClient::continueSessionBringUp() {
    if (accessToken.isEmpty()) {
        return;
    }
    fetchClientToken();
}

void SpotifyClient::fetchClientToken() {
    const bool valid = !clientToken.isEmpty() &&
                       QDateTime::currentMSecsSinceEpoch() < clientTokenExpiryMs;
    if (valid) {
        resolveEndpoints();
        return;
    }

    spotify::clienttoken::http::v0::ClientTokenRequest req;
    req.set_request_type(spotify::clienttoken::http::v0::REQUEST_CLIENT_DATA_REQUEST);
    auto *clientData = req.mutable_client_data();
    clientData->set_client_version(kClientVersion);
    clientData->set_client_id(kClientIdHex);
    auto *sdk = clientData->mutable_connectivity_sdk_data();
    sdk->set_device_id(deviceId.toStdString());
    auto *win = sdk->mutable_platform_specific_data()->mutable_desktop_windows();
    win->set_os_version(10);
    win->set_os_build(22631);
    win->set_platform_id(2);
    win->set_image_file_machine(34404);
    win->set_pe_machine(34404);

    std::string body;
    if (!req.SerializeToString(&body)) {
        return;
    }

    QNetworkRequest request{QUrl(QString::fromLatin1(kClientTokenUrl))};
    request.setRawHeader("Accept", "application/x-protobuf");
    request.setRawHeader("Content-Type", "application/x-protobuf");
    request.setRawHeader("User-Agent", kUserAgent);

    logMessage("Requesting Spotify client token...");
    QNetworkReply *reply = network->post(request, QByteArray(body.data(), int(body.size())));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray data = reply->readAll();
        const bool netError = reply->error() != QNetworkReply::NoError;
        reply->deleteLater();

        spotify::clienttoken::http::v0::ClientTokenResponse resp;
        if (netError || !resp.ParseFromArray(data.constData(), int(data.size()))) {
            logMessage("Client token request failed; continuing without one.");
            resolveEndpoints();
            return;
        }

        if (resp.response_type() == spotify::clienttoken::http::v0::RESPONSE_GRANTED_TOKEN_RESPONSE) {
            clientToken = QString::fromStdString(resp.granted_token().token());
            const int ttl = resp.granted_token().expires_after_seconds();
            clientTokenExpiryMs = QDateTime::currentMSecsSinceEpoch() + qint64(qMax(60, ttl) - 60) * 1000;
            logMessage(QString("Obtained client token (%1 chars).").arg(clientToken.size()));
        } else {
            logMessage("Client token response carried a challenge (unsupported); continuing without one.");
        }
        resolveEndpoints();
    });
}

void SpotifyClient::resolveEndpoints() {
    if (endpointsResolved) {
        connectWebSocket();
        return;
    }

    QNetworkRequest request{QUrl(QString::fromLatin1(kApresolveUrl))};
    request.setRawHeader("User-Agent", kUserAgent);

    QNetworkReply *reply = network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        const QJsonArray dealers = obj.value("dealer").toArray();
        const QJsonArray spclients = obj.value("spclient").toArray();

        if (!dealers.isEmpty()) {
            QString host = dealers.first().toString();
            dealerHost = host.section(':', 0, 0);
        }
        if (!spclients.isEmpty()) {
            QString host = spclients.first().toString().section(':', 0, 0);
            spclientBaseUrl = "https://" + host;
        }
        endpointsResolved = true;
        logMessage(QString("Resolved endpoints (dealer=%1, spclient=%2).").arg(dealerHost, spclientBaseUrl));
        connectWebSocket();
    });
}
