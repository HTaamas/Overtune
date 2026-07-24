#include "spotify_client.h"
#include "settings/app_settings.h"

#include <QWebSocket>
#include <QSslError>
#include <QDateTime>
#include <QUuid>
#include <QTcpServer>
#include <QTcpSocket>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <iostream>
#include <string>
#include <zlib.h>

// The connect-state protos define a PlayerState field named "signals", which
// collides with Qt's `signals` keyword macro. Neutralize the Qt keyword macros
// while the generated protobuf headers are included.
QT_WARNING_PUSH
#pragma push_macro("signals")
#pragma push_macro("slots")
#pragma push_macro("emit")
#undef signals
#undef slots
#undef emit
#include "spotify/connectstate/connect.pb.h"
#include "spotify/connectstate/player.pb.h"
#include "spotify/connectstate/devices/connect_devices.pb.h"
#include "spotify/clienttoken/http/v0/clienttoken.pb.h"
#include "spotify/clienttoken/data/v0/connectivity.pb.h"
#include "spotify/extendedmetadata/extended_metadata.pb.h"
#include "spotify/extendedmetadata/extension_kind.pb.h"
#include "spotify/extendedmetadata/entity_extension_data.pb.h"
#include "spotify/metadata/metadata.pb.h"
#include "spotify/collection/collection.pb.h"
#pragma pop_macro("emit")
#pragma pop_macro("slots")
#pragma pop_macro("signals")
QT_WARNING_POP

namespace {
constexpr int kPendingVolumeGracePeriodMs = 2500;
constexpr int kMaxConnectVolume = 65535;

// Well-known Spotify desktop client id (librespot), required for the device flow.
const char kClientIdHex[] = "65b708073fc0480ea92a077233ca87bd";
const char kUserAgent[] = "Spotify/125700463 Win32_x86_64/0 (PC desktop)";
const char kClientVersion[] = "1.2.52.442.g01d2b6ec";
const char kDeviceName[] = "SpotifyVol";

const char kDeviceAuthorizeUrl[] = "https://accounts.spotify.com/oauth2/device/authorize";
const char kAuthorizeUrl[] = "https://accounts.spotify.com/authorize";
const char kTokenUrl[] = "https://accounts.spotify.com/api/token";

// Whitelisted for Spotify's desktop client id — go-librespot uses the same
// port/path, and the auth-code token response includes the canonical
// username (the device flow's does not).
constexpr quint16 kOAuthRedirectPort = 36842;
const char kOAuthRedirectUri[] = "http://127.0.0.1:36842/login";
const char kClientTokenUrl[] = "https://clienttoken.spotify.com/v1/clienttoken";
const char kApresolveUrl[] = "https://apresolve.spotify.com/?type=dealer&type=spclient";

const char kDeviceFlowScopes[] =
    "app-remote-control,playlist-modify,playlist-modify-private,playlist-modify-public,"
    "playlist-read,playlist-read-collaborative,playlist-read-private,streaming,"
    "ugc-image-upload,user-follow-modify,user-follow-read,user-library-modify,"
    "user-library-read,user-modify,user-modify-playback-state,user-modify-private,"
    "user-personalized,user-read-birthdate,user-read-currently-playing,user-read-email,"
    "user-read-play-history,user-read-playback-position,user-read-playback-state,"
    "user-read-private,user-read-recently-played,user-top-read";
}

SpotifyClient::SpotifyClient(QObject *parent)
    : QObject(parent), network(new QNetworkAccessManager(this)) {
    deviceId = AppSettings::loadOrCreateDeviceId();

    devicePollTimer = new QTimer(this);
    connect(devicePollTimer, &QTimer::timeout, this, &SpotifyClient::pollDeviceToken);

    webSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(webSocket, &QWebSocket::connected, this, &SpotifyClient::onWebSocketConnected);
    connect(webSocket, &QWebSocket::disconnected, this, &SpotifyClient::onWebSocketDisconnected);
    connect(webSocket, &QWebSocket::textMessageReceived, this, &SpotifyClient::onWebSocketTextMessageReceived);
    connect(webSocket, &QWebSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
        QStringList errStrings;
        for (const auto &err : errors) {
            errStrings.append(err.errorString());
        }
        logMessage("[WS] SSL errors: " + errStrings.join(" | "));
    });
    connect(webSocket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        logMessage(QString("[WS] socket error %1: %2").arg(error).arg(webSocket->errorString()));
    });

    pingTimer = new QTimer(this);
    connect(pingTimer, &QTimer::timeout, this, &SpotifyClient::sendWebSocketPing);
}

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

    QByteArray randomBytes(64, 0);
    for (char &b : randomBytes) {
        b = char(QRandomGenerator::system()->bounded(256));
    }
    codeVerifier = QString::fromLatin1(randomBytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    const QByteArray challenge = QCryptographicHash::hash(codeVerifier.toLatin1(), QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

    QUrlQuery query;
    query.addQueryItem("response_type", "code");
    query.addQueryItem("client_id", kClientIdHex);
    query.addQueryItem("redirect_uri", kOAuthRedirectUri);
    query.addQueryItem("scope", QString(kDeviceFlowScopes).replace(',', ' '));
    query.addQueryItem("code_challenge_method", "S256");
    query.addQueryItem("code_challenge", QString::fromLatin1(challenge));

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
                code = QUrlQuery(QUrl("http://localhost" + path).query()).queryItemValue("code");
            }

            const QByteArray page = code.isEmpty()
                ? QByteArray("<html><body style=\"font-family:sans-serif\">Authorization failed - you can close this window.</body></html>")
                : QByteArray("<html><body style=\"font-family:sans-serif\">SpotifyVol is connected - you can close this window.</body></html>");
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " +
                          QByteArray::number(page.size()) + "\r\n\r\n" + page);
            socket->flush();
            socket->disconnectFromHost();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);

            if (!code.isEmpty()) {
                authServer->close();
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

void SpotifyClient::refreshAccessToken() {
    if (refreshToken.isEmpty()) {
        logMessage("Cannot refresh: no refresh token. Please re-authorize (Connect Spotify).");
        return;
    }

    QUrlQuery form;
    form.addQueryItem("grant_type", "refresh_token");
    form.addQueryItem("refresh_token", refreshToken);
    form.addQueryItem("client_id", kClientIdHex);

    QNetworkRequest request{QUrl(QString::fromLatin1(kTokenUrl))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("User-Agent", kUserAgent);

    QNetworkReply *reply = network->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        if (obj.value("access_token").toString().isEmpty()) {
            logMessage("Token refresh failed; the stored session is no longer valid. Please Connect Spotify again.");
            AppSettings::clearRefreshToken();
            refreshToken.clear();
            return;
        }
        applyTokenResponse(obj);
        continueSessionBringUp();
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

// ---------------------------------------------------------------------------
// Dealer WebSocket
// ---------------------------------------------------------------------------

void SpotifyClient::connectWebSocket() {
    if (accessToken.isEmpty()) {
        return;
    }
    if (accessTokenExpired()) {
        logMessage("Access token expired; refreshing before connecting dealer.");
        refreshAccessToken();
        return;
    }
    if (webSocket->state() == QAbstractSocket::ConnectedState ||
        webSocket->state() == QAbstractSocket::ConnectingState) {
        return;
    }

    QUrl url(QString("wss://%1/").arg(dealerHost));
    QUrlQuery query;
    query.addQueryItem("access_token", accessToken);
    url.setQuery(query);

    logMessage(QString("[WS] Connecting to dealer %1 ...").arg(dealerHost));

    QNetworkRequest request(url);
    request.setRawHeader("Origin", "https://open.spotify.com");
    request.setRawHeader("User-Agent", kUserAgent);
    webSocket->open(request);
}

void SpotifyClient::onWebSocketConnected() {
    logMessage("[WS] Dealer connected; waiting for connection id...");
    connectStateRegistered = false;
    sendWebSocketPing();
    pingTimer->start(30000);
}

void SpotifyClient::onWebSocketDisconnected() {
    logMessage(QString("[WS] Dealer disconnected: %1").arg(webSocket->errorString()));
    pingTimer->stop();
    connectStateRegistered = false;
    spotConnId.clear();

    QTimer::singleShot(5000, this, [this]() { connectWebSocket(); });
}

void SpotifyClient::sendWebSocketPing() {
    if (webSocket->state() == QAbstractSocket::ConnectedState) {
        QJsonObject pingObj;
        pingObj["type"] = "ping";
        webSocket->sendTextMessage(QJsonDocument(pingObj).toJson(QJsonDocument::Compact));
    }
}

void SpotifyClient::onWebSocketTextMessageReceived(const QString &message) {
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject obj = doc.object();
    const QString uri = obj.value("uri").toString();

    if (uri.startsWith("hm://pusher/v1/connections/")) {
        const QJsonObject headers = obj.value("headers").toObject();
        spotConnId = headers.value("Spotify-Connection-Id").toString();
        if (!spotConnId.isEmpty()) {
            logMessage(QString("[WS] Received connection id (%1 chars); registering connect-state.").arg(spotConnId.size()));
            registerConnectState();
        }
        return;
    }

    if (uri.startsWith("hm://connect-state/v1/cluster")) {
        const QJsonObject headers = obj.value("headers").toObject();
        const bool gzipped = headers.value("Transfer-Encoding").toString() == "gzip";
        const QJsonArray payloads = obj.value("payloads").toArray();
        if (payloads.isEmpty()) {
            return;
        }
        QByteArray payload = QByteArray::fromBase64(payloads.first().toString().toUtf8());
        if (gzipped) {
            payload = gunzip(payload);
        }
        handleClusterBytes(payload, /*isUpdate=*/true);
    }
}

// ---------------------------------------------------------------------------
// Connect-state
// ---------------------------------------------------------------------------

void SpotifyClient::registerConnectState() {
    if (spotConnId.isEmpty()) {
        return;
    }

    spotify::connectstate::PutStateRequest req;
    req.set_client_side_timestamp(QDateTime::currentMSecsSinceEpoch());
    req.set_member_type(spotify::connectstate::CONNECT_STATE);
    req.set_put_state_reason(spotify::connectstate::NEW_DEVICE);
    req.set_is_active(false);

    auto *info = req.mutable_device()->mutable_device_info();
    info->set_can_play(false);
    info->set_volume(kMaxConnectVolume);
    info->set_name(kDeviceName);
    info->set_device_id(deviceId.toStdString());
    info->set_device_type(spotify::connectstate::devices::COMPUTER);
    info->set_device_software_version("spotifyvol 1.0");
    info->set_client_id(kClientIdHex);
    info->set_spirc_version("3.2.6");

    auto *cap = info->mutable_capabilities();
    cap->set_can_be_player(false);
    cap->set_gaia_eq_connect_id(true);
    cap->set_is_observable(true);
    cap->set_volume_steps(100);
    cap->add_supported_types("audio/track");
    cap->add_supported_types("audio/episode");
    cap->set_command_acks(true);
    cap->set_hidden(true);
    cap->set_disable_volume(true);
    cap->set_is_controllable(false);
    cap->set_supports_command_request(true);
    cap->set_supports_gzip_pushes(true);
    cap->set_supports_playlist_v2(true);
    cap->set_supports_set_backend_metadata(true);
    cap->set_supports_set_options_command(true);
    cap->set_needs_full_player_state(false);

    std::string body;
    if (!req.SerializeToString(&body)) {
        return;
    }

    QUrl url(spclientBaseUrl + "/connect-state/v1/devices/" + deviceId);
    QNetworkRequest request = spclientRequest(url);
    request.setRawHeader("Content-Type", "application/x-protobuf");

    QNetworkReply *reply = network->sendCustomRequest(request, "PUT", QByteArray(body.data(), int(body.size())));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        if (status == 200) {
            connectStateRegistered = true;
            logMessage("[connect-state] Registered as an observer device. Listening for realtime pushes.");
            emit authComplete();
            ensureUserProfile();
            handleClusterBytes(data, /*isUpdate=*/false);
        } else if (status == 401) {
            logMessage("[connect-state] Registration unauthorized; refreshing token.");
            refreshAccessToken();
        } else {
            logMessage(QString("[connect-state] Registration failed (HTTP %1): %2")
                           .arg(status)
                           .arg(QString::fromUtf8(data.left(200))));
        }
    });
}

void SpotifyClient::handleClusterBytes(const QByteArray &protoBytes, bool isUpdate) {
    spotify::connectstate::Cluster cluster;
    if (isUpdate) {
        spotify::connectstate::ClusterUpdate update;
        if (!update.ParseFromArray(protoBytes.constData(), int(protoBytes.size()))) {
            return;
        }
        cluster = update.cluster();
    } else if (!cluster.ParseFromArray(protoBytes.constData(), int(protoBytes.size()))) {
        return;
    }

    activeDeviceId = QString::fromStdString(cluster.active_device_id());
    if (activeDeviceId.isEmpty() || !cluster.has_player_state()) {
        // Nothing is playing anywhere.
        lastIsPlaying = false;
        setVolumeControlSupported(false);
        clearUpcomingQueue();
        emit stateSynced(currentVolume, 0, false, false);
        return;
    }

    const spotify::connectstate::PlayerState &ps = cluster.player_state();
    const bool isPlaying = ps.is_playing() && !ps.is_paused();

    // Liked Songs plays under "spotify:user:<username>:collection", which is
    // always the account's own collection — a reliable username source given
    // that neither the device-flow token response nor spclient provide one.
    if (username.isEmpty()) {
        const QString contextUri = QString::fromStdString(ps.context_uri());
        if (contextUri.startsWith("spotify:user:") && contextUri.section(':', 3) == "collection") {
            username = contextUri.section(':', 2, 2);
            if (!username.isEmpty()) {
                AppSettings::saveUsername(username);
                logMessage(QString("[like] username captured from the Liked Songs context: %1").arg(username));
                ensureUserProfile();
            }
        }
    }

    qint64 positionMs = ps.position_as_of_timestamp();
    if (isPlaying && ps.timestamp() > 0) {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - ps.timestamp();
        if (elapsed > 0 && elapsed < 10 * 60 * 1000) {
            positionMs += elapsed;
        }
    }
    const int durationMs = int(ps.duration());
    if (durationMs > 0) {
        positionMs = qBound<qint64>(qint64(0), positionMs, qint64(durationMs));
    }

    // Volume + volume support from the active device entry.
    int volumePercent = currentVolume;
    bool volumeSupported = false;
    const auto &deviceMap = cluster.device();
    auto devIt = deviceMap.find(activeDeviceId.toStdString());
    if (devIt != deviceMap.end()) {
        const spotify::connectstate::DeviceInfo &info = devIt->second;
        volumePercent = int(qRound(info.volume() * 100.0 / kMaxConnectVolume));
        volumeSupported = !info.capabilities().disable_volume();
    }

    // Honour an in-flight local volume change so the OSD doesn't snap back.
    const bool volumePending = pendingVolume >= 0 && pendingVolumeTimer.isValid() &&
                               pendingVolumeTimer.elapsed() < kPendingVolumeGracePeriodMs;
    if (volumePending) {
        volumePercent = pendingVolume;
    } else {
        pendingVolume = -1;
    }

    currentVolume = qBound(0, volumePercent, 100);
    lastProgressMs = int(positionMs);
    lastDurationMs = durationMs;
    lastIsPlaying = isPlaying;
    setVolumeControlSupported(volumeSupported);

    const QString trackUri = QString::fromStdString(ps.track().uri());
    const QString trackId = trackUri.startsWith("spotify:track:") ? trackUri.section(':', 2, 2) : QString();

    if (!trackId.isEmpty() && trackId != lastTrackId) {
        lastTrackId = trackId;

        // The realtime cluster push carries the display metadata directly, so we
        // render it immediately with no Web-API call. Only when the artist name
        // is absent (some editorial/radio contexts omit it) do we fall back to a
        // single /v1/tracks lookup to enrich it.
        const auto &meta = ps.track().metadata();
        auto metaValue = [&meta](const char *key) -> QString {
            auto it = meta.find(key);
            return it != meta.end() ? QString::fromStdString(it->second) : QString();
        };

        const QString title = metaValue("title");
        const QString artist = metaValue("artist_name");

        QString image = metaValue("image_xlarge_url");
        if (image.isEmpty()) image = metaValue("image_large_url");
        if (image.isEmpty()) image = metaValue("image_url");
        if (image.isEmpty()) image = metaValue("image_small_url");
        if (image.startsWith("spotify:image:")) {
            image = "https://i.scdn.co/image/" + image.mid(int(sizeof("spotify:image:") - 1));
        }

        logMessage(QString("[track] \"%1\" - %2").arg(title, artist.isEmpty() ? "(resolving artist…)" : artist));
        emit trackChanged(currentVolume, title, artist, trackId, image,
                          lastProgressMs, lastDurationMs, lastIsPlaying, volumeSupported);

        if (artist.isEmpty() || title.isEmpty()) {
            fetchTrackDetails(trackId);
        }
    }

    updateUpcomingQueue(ps);
    emit stateSynced(currentVolume, lastProgressMs, lastIsPlaying, volumeSupported);
}

// ---------------------------------------------------------------------------
// Upcoming queue
// ---------------------------------------------------------------------------

namespace {
constexpr int kMaxQueueTracks = 30;

QString queueSignatureFor(const QList<UpcomingTrack> &tracks) {
    QString signature;
    for (const UpcomingTrack &t : tracks) {
        signature += t.trackId + '|' + t.title + '|' + t.artist + '|' + t.artUrl +
                     '|' + (t.liked ? '1' : '0') + (t.smartShuffle ? '1' : '0') + ';';
    }
    return signature;
}
}

void SpotifyClient::clearUpcomingQueue() {
    if (lastQueueSignature.isEmpty() && lastQueue.isEmpty()) {
        return;
    }
    lastQueue.clear();
    lastQueueSignature.clear();
    emit queueChanged({});
}

void SpotifyClient::updateUpcomingQueue(const spotify::connectstate::PlayerState &ps) {
    QList<UpcomingTrack> upcoming;
    QStringList unresolved;

    for (const auto &pt : ps.next_tracks()) {
        if (upcoming.size() >= kMaxQueueTracks) {
            break;
        }
        const QString uri = QString::fromStdString(pt.uri());
        if (!uri.startsWith("spotify:track:")) {
            continue; // skip delimiters, ads, episodes...
        }

        UpcomingTrack track;
        track.trackId = uri.section(':', 2, 2);
        track.uid = QString::fromStdString(pt.uid());

        const auto &meta = pt.metadata();
        auto metaValue = [&meta](const char *key) -> QString {
            auto it = meta.find(key);
            return it != meta.end() ? QString::fromStdString(it->second) : QString();
        };
        track.title = metaValue("title");
        track.artist = metaValue("artist_name");
        track.durationMs = metaValue("duration").toInt();
        track.liked = isTrackLiked(track.trackId);

        // Smart Shuffle interleaves recommendations into your context; the
        // injected ones are tagged provider="enhanced_recommendation" (your
        // own context tracks are not), which is exactly what Spotify's UI
        // marks with the sparkle.
        track.smartShuffle = metaValue("provider") == "enhanced_recommendation";

        // Small art is plenty for the thumbnail rows.
        QString image = metaValue("image_small_url");
        if (image.isEmpty()) image = metaValue("image_url");
        if (image.isEmpty()) image = metaValue("image_large_url");
        if (image.isEmpty()) image = metaValue("image_xlarge_url");
        if (image.startsWith("spotify:image:")) {
            image = "https://i.scdn.co/image/" + image.mid(int(sizeof("spotify:image:") - 1));
        }
        track.artUrl = image;

        const auto cached = queueMetaCache.constFind(track.trackId);
        if (cached != queueMetaCache.constEnd()) {
            if (track.title.isEmpty()) track.title = cached->title;
            if (track.artist.isEmpty()) track.artist = cached->artist;
            if (track.artUrl.isEmpty()) track.artUrl = cached->artUrl;
            if (track.durationMs <= 0) track.durationMs = cached->durationMs;
        }
        // Fetch once per track: a cache entry (even with gaps) means we already
        // asked and the service had nothing more to offer.
        if ((track.title.isEmpty() || track.artist.isEmpty() || track.artUrl.isEmpty()) &&
            cached == queueMetaCache.constEnd()) {
            unresolved.append(track.trackId);
        }
        upcoming.append(track);
    }

    const QString signature = queueSignatureFor(upcoming);
    if (signature != lastQueueSignature) {
        lastQueueSignature = signature;
        lastQueue = upcoming;
        emit queueChanged(upcoming);
    }

    QStringList toFetch;
    for (const QString &id : unresolved) {
        if (!pendingQueueLookups.contains(id)) {
            pendingQueueLookups.insert(id);
            toFetch.append(id);
        }
    }
    if (!toFetch.isEmpty()) {
        fetchUpcomingTrackDetails(toFetch);
    }
}

void SpotifyClient::fetchUpcomingTrackDetails(const QStringList &trackIds) {
    // Same internal extended-metadata service as fetchTrackDetails, but batched:
    // one request resolves every queue entry the cluster push left unnamed.
    spotify::extendedmetadata::BatchedEntityRequest req;
    for (const QString &trackId : trackIds) {
        auto *entity = req.add_entity_request();
        entity->set_entity_uri(("spotify:track:" + trackId).toStdString());
        entity->add_query()->set_extension_kind(spotify::extendedmetadata::TRACK_V4);
    }

    std::string body;
    if (!req.SerializeToString(&body)) {
        for (const QString &trackId : trackIds) {
            pendingQueueLookups.remove(trackId);
        }
        return;
    }

    QUrl url(spclientBaseUrl + "/extended-metadata/v0/extended-metadata");
    QNetworkRequest request = spclientRequest(url);
    request.setRawHeader("Accept", "application/x-protobuf");
    request.setRawHeader("Content-Type", "application/x-protobuf");

    QNetworkReply *reply = network->post(request, QByteArray(body.data(), int(body.size())));
    connect(reply, &QNetworkReply::finished, this, [this, reply, trackIds]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        for (const QString &trackId : trackIds) {
            pendingQueueLookups.remove(trackId);
        }

        if (status != 200) {
            logMessage(QString("[queue] metadata lookup for %1 tracks failed (HTTP %2)").arg(trackIds.size()).arg(status));
            return;
        }

        spotify::extendedmetadata::BatchedExtensionResponse resp;
        if (!resp.ParseFromArray(data.constData(), int(data.size()))) {
            return;
        }

        if (queueMetaCache.size() > 600) {
            queueMetaCache.clear();
        }

        for (const auto &arr : resp.extended_metadata()) {
            if (arr.extension_kind() != spotify::extendedmetadata::TRACK_V4) {
                continue;
            }
            for (const auto &ext : arr.extension_data()) {
                const int code = ext.header().status_code();
                if (code != 0 && code != 200) {
                    continue;
                }

                spotify::metadata::Track track;
                if (!ext.extension_data().UnpackTo(&track)) {
                    continue;
                }

                const QString entityUri = QString::fromStdString(ext.entity_uri());
                if (!entityUri.startsWith("spotify:track:")) {
                    continue;
                }

                UpcomingTrack resolved;
                resolved.trackId = entityUri.section(':', 2, 2);
                resolved.title = QString::fromStdString(track.name());
                QStringList artists;
                for (int i = 0; i < track.artist_size(); ++i) {
                    artists.append(QString::fromStdString(track.artist(i).name()));
                }
                resolved.artist = artists.join(", ");
                resolved.durationMs = int(track.duration());

                // Smallest cover is enough for the queue thumbnails.
                const auto &album = track.album();
                const spotify::metadata::Image *bestImage = nullptr;
                for (int i = 0; i < album.cover_group().image_size(); ++i) {
                    const auto &img = album.cover_group().image(i);
                    if (!bestImage || (img.width() > 0 && img.width() < bestImage->width())) {
                        bestImage = &img;
                    }
                }
                for (int i = 0; !bestImage && i < album.cover_size(); ++i) {
                    bestImage = &album.cover(i);
                }
                if (bestImage && bestImage->has_file_id()) {
                    const QByteArray hex =
                        QByteArray(bestImage->file_id().data(), int(bestImage->file_id().size())).toHex();
                    resolved.artUrl = "https://i.scdn.co/image/" + QString::fromUtf8(hex);
                }

                queueMetaCache.insert(resolved.trackId, resolved);
            }
        }

        // Patch the queue we last emitted with the freshly resolved names.
        bool changed = false;
        for (UpcomingTrack &t : lastQueue) {
            const auto cached = queueMetaCache.constFind(t.trackId);
            if (cached == queueMetaCache.constEnd()) {
                continue;
            }
            if (t.title.isEmpty() && !cached->title.isEmpty()) { t.title = cached->title; changed = true; }
            if (t.artist.isEmpty() && !cached->artist.isEmpty()) { t.artist = cached->artist; changed = true; }
            if (t.artUrl.isEmpty() && !cached->artUrl.isEmpty()) { t.artUrl = cached->artUrl; changed = true; }
            if (t.durationMs <= 0 && cached->durationMs > 0) { t.durationMs = cached->durationMs; changed = true; }
        }
        if (changed) {
            lastQueueSignature = queueSignatureFor(lastQueue);
            emit queueChanged(lastQueue);
        }
    });
}

void SpotifyClient::fetchTrackDetails(const QString &trackId) {
    // Resolve full metadata (artist names, cover art) via Spotify's internal
    // extended-metadata service on spclient — NOT the public Web API — so this is
    // not subject to api.spotify.com rate limiting. This is how go-librespot/Mira
    // does it. Only called when the realtime cluster push lacked the artist name.
    const QString entityUri = "spotify:track:" + trackId;

    spotify::extendedmetadata::BatchedEntityRequest req;
    auto *entity = req.add_entity_request();
    entity->set_entity_uri(entityUri.toStdString());
    entity->add_query()->set_extension_kind(spotify::extendedmetadata::TRACK_V4);

    std::string body;
    if (!req.SerializeToString(&body)) {
        return;
    }

    QUrl url(spclientBaseUrl + "/extended-metadata/v0/extended-metadata");
    QNetworkRequest request = spclientRequest(url);
    request.setRawHeader("Accept", "application/x-protobuf");
    request.setRawHeader("Content-Type", "application/x-protobuf");

    QNetworkReply *reply = network->post(request, QByteArray(body.data(), int(body.size())));
    connect(reply, &QNetworkReply::finished, this, [this, reply, trackId, entityUri]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        if (status == 401) {
            refreshAccessToken();
            return;
        }
        if (status != 200) {
            logMessage(QString("[track] metadata lookup for %1 failed (HTTP %2)").arg(trackId).arg(status));
            return;
        }

        spotify::extendedmetadata::BatchedExtensionResponse resp;
        if (!resp.ParseFromArray(data.constData(), int(data.size()))) {
            return;
        }

        for (const auto &arr : resp.extended_metadata()) {
            if (arr.extension_kind() != spotify::extendedmetadata::TRACK_V4) {
                continue;
            }
            for (const auto &ext : arr.extension_data()) {
                if (ext.entity_uri() != entityUri.toStdString()) {
                    continue;
                }
                const int code = ext.header().status_code();
                if (code != 0 && code != 200) {
                    continue;
                }

                spotify::metadata::Track track;
                if (!ext.extension_data().UnpackTo(&track)) {
                    continue;
                }

                const QString name = QString::fromStdString(track.name());

                QStringList artists;
                for (int i = 0; i < track.artist_size(); ++i) {
                    artists.append(QString::fromStdString(track.artist(i).name()));
                }

                // Cover art: pick the largest image from the album's cover group.
                QString albumArtUrl;
                const auto &album = track.album();
                const spotify::metadata::Image *bestImage = nullptr;
                for (int i = 0; i < album.cover_group().image_size(); ++i) {
                    const auto &img = album.cover_group().image(i);
                    if (!bestImage || int(img.size()) > int(bestImage->size())) {
                        bestImage = &img;
                    }
                }
                for (int i = 0; !bestImage && i < album.cover_size(); ++i) {
                    bestImage = &album.cover(i);
                }
                if (bestImage && bestImage->has_file_id()) {
                    const QByteArray hex =
                        QByteArray(bestImage->file_id().data(), int(bestImage->file_id().size())).toHex();
                    albumArtUrl = "https://i.scdn.co/image/" + QString::fromUtf8(hex);
                }

                logMessage(QString("[track] metadata: \"%1\" - %2").arg(name, artists.join(", ")));
                emit trackChanged(currentVolume, name, artists.join(", "), trackId, albumArtUrl,
                                  lastProgressMs, lastDurationMs, lastIsPlaying, volumeControlSupported);
                return;
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Remote control
// ---------------------------------------------------------------------------

bool SpotifyClient::setVolume(int volume) {
    if (!connectStateRegistered || activeDeviceId.isEmpty() || activeDeviceId == deviceId ||
        spotConnId.isEmpty() || !volumeControlSupported) {
        return false;
    }

    currentVolume = qBound(0, volume, 100);
    pendingVolume = currentVolume;
    pendingVolumeTimer.restart();

    const int connectVolume = qRound(currentVolume * kMaxConnectVolume / 100.0);
    const QByteArray body = QJsonDocument(QJsonObject{{"volume", connectVolume}}).toJson(QJsonDocument::Compact);

    QUrl url(spclientBaseUrl + QString("/connect-state/v1/connect/volume/from/%1/to/%2").arg(deviceId, activeDeviceId));
    QNetworkRequest request = spclientRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply *reply = network->sendCustomRequest(request, "PUT", body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (status == 401) {
            refreshAccessToken();
        }
    });
    return true;
}

void SpotifyClient::togglePlayPause() {
    sendConnectCommand(lastIsPlaying ? "pause" : "resume");
}

void SpotifyClient::nextTrack() {
    sendConnectCommand("skip_next");
}

void SpotifyClient::prevTrack() {
    sendConnectCommand("skip_prev");
}

// ---------------------------------------------------------------------------
// Liked songs (spclient collection service; the Web API 429s this client id)
// ---------------------------------------------------------------------------

bool SpotifyClient::isTrackLiked(const QString &trackId) const {
    return likedTrackIds.contains(trackId);
}

void SpotifyClient::ensureUserProfile() {
    if (username.isEmpty()) {
        username = AppSettings::loadUsername();
    }
    if (username.isEmpty()) {
        logMessage("[like] username unknown - play something from your Liked Songs once and it locks in");
        return;
    }
    if (!likedSetRequested) {
        likedSetRequested = true;
        logMessage(QString("[like] loading Liked Songs for %1...").arg(username));
        fetchLikedTracks(QString(), 0);
    }
}

void SpotifyClient::fetchLikedTracks(const QString &paginationToken, int page) {
    spotify::collection::PageRequest req;
    req.set_username(username.toStdString());
    req.set_set("collection");
    if (!paginationToken.isEmpty()) {
        req.set_pagination_token(paginationToken.toStdString());
    }
    req.set_limit(300);

    std::string body;
    if (!req.SerializeToString(&body)) {
        return;
    }

    QNetworkRequest request = spclientRequest(QUrl(spclientBaseUrl + "/collection/v2/paging"));
    request.setRawHeader("Accept", "application/vnd.collection-v2.spotify.proto");
    request.setRawHeader("Content-Type", "application/vnd.collection-v2.spotify.proto");

    QNetworkReply *reply = network->post(request, QByteArray(body.data(), int(body.size())));
    connect(reply, &QNetworkReply::finished, this, [this, reply, page]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        if (status == 401) {
            likedSetRequested = false;
            refreshAccessToken();
            QTimer::singleShot(5000, this, [this]() { ensureUserProfile(); });
            return;
        }
        if (status != 200) {
            logMessage(QString("[like] loading Liked Songs failed (HTTP %1): %2")
                           .arg(status).arg(QString::fromUtf8(data.left(200))));
            return;
        }

        spotify::collection::PageResponse resp;
        if (!resp.ParseFromArray(data.constData(), int(data.size()))) {
            logMessage("[like] could not parse the Liked Songs page");
            return;
        }

        for (const auto &item : resp.items()) {
            const QString uri = QString::fromStdString(item.uri());
            if (!item.is_removed() && uri.startsWith("spotify:track:")) {
                likedTrackIds.insert(uri.section(':', 2, 2));
            }
        }

        const QString nextToken = QString::fromStdString(resp.next_page_token());
        if (!nextToken.isEmpty() && page < 200) {
            fetchLikedTracks(nextToken, page + 1);
        } else {
            logMessage(QString("[like] Liked Songs loaded (%1 tracks)").arg(likedTrackIds.size()));
            emit likedSongsLoaded();
            // The queue was likely built before the liked set arrived; refresh
            // its heart flags now that we know what's liked.
            bool changed = false;
            for (UpcomingTrack &t : lastQueue) {
                const bool liked = isTrackLiked(t.trackId);
                if (t.liked != liked) {
                    t.liked = liked;
                    changed = true;
                }
            }
            if (changed) {
                lastQueueSignature = queueSignatureFor(lastQueue);
                emit queueChanged(lastQueue);
            }
        }
    });
}

void SpotifyClient::toggleLikeCurrentTrack() {
    if (lastTrackId.isEmpty() || accessToken.isEmpty()) {
        emit trackLikeFinished(false, false);
        return;
    }
    if (username.isEmpty()) {
        logMessage("[like] username unknown - play something from your Liked Songs once and it locks in");
        ensureUserProfile();
        emit trackLikeFinished(false, isTrackLiked(lastTrackId));
        return;
    }

    const QString trackId = lastTrackId;
    const bool makeLiked = !likedTrackIds.contains(trackId);

    spotify::collection::WriteRequest req;
    req.set_username(username.toStdString());
    req.set_set("collection");
    auto *item = req.add_items();
    item->set_uri(("spotify:track:" + trackId).toStdString());
    item->set_added_at(makeLiked ? int(QDateTime::currentSecsSinceEpoch()) : 0);
    item->set_is_removed(!makeLiked);
    req.set_client_update_id(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());

    std::string body;
    if (!req.SerializeToString(&body)) {
        emit trackLikeFinished(false, !makeLiked);
        return;
    }

    QNetworkRequest request = spclientRequest(QUrl(spclientBaseUrl + "/collection/v2/write"));
    request.setRawHeader("Accept", "application/vnd.collection-v2.spotify.proto");
    request.setRawHeader("Content-Type", "application/vnd.collection-v2.spotify.proto");

    QNetworkReply *reply = network->post(request, QByteArray(body.data(), int(body.size())));
    connect(reply, &QNetworkReply::finished, this, [this, reply, trackId, makeLiked]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (status == 401) {
            refreshAccessToken();
        }

        const bool success = status / 100 == 2;
        if (success) {
            if (makeLiked) {
                likedTrackIds.insert(trackId);
            } else {
                likedTrackIds.remove(trackId);
            }
        }
        logMessage(success
            ? QString("[like] %1 %2 Liked Songs").arg(trackId, makeLiked ? "added to" : "removed from")
            : QString("[like] updating %1 failed (HTTP %2)").arg(trackId).arg(status));
        emit trackLikeFinished(success, likedTrackIds.contains(trackId));
    });
}

void SpotifyClient::skipToQueuedTrack(const QString &trackId, const QString &uid) {
    QJsonObject trackObject{{"uri", "spotify:track:" + trackId}};
    if (!uid.isEmpty()) {
        trackObject.insert("uid", uid);
    }
    sendConnectCommand("skip_next", QJsonObject{{"track", trackObject}});
}

void SpotifyClient::sendConnectCommand(const QString &endpoint, const QJsonObject &extraCommandFields) {
    if (!connectStateRegistered || activeDeviceId.isEmpty() || activeDeviceId == deviceId ||
        spotConnId.isEmpty()) {
        return;
    }

    QJsonObject command{{"endpoint", endpoint}};
    for (auto it = extraCommandFields.constBegin(); it != extraCommandFields.constEnd(); ++it) {
        command.insert(it.key(), it.value());
    }
    const QByteArray body =
        QJsonDocument(QJsonObject{{"command", command}}).toJson(QJsonDocument::Compact);

    QUrl url(spclientBaseUrl + QString("/connect-state/v1/player/command/from/%1/to/%2").arg(deviceId, activeDeviceId));
    QNetworkRequest request = spclientRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply *reply = network->post(request, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, endpoint]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (status == 401) {
            refreshAccessToken();
        } else if (status / 100 != 2) {
            logMessage(QString("[connect-state] command '%1' failed (HTTP %2)").arg(endpoint).arg(status));
        }
    });
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QNetworkRequest SpotifyClient::spclientRequest(const QUrl &url) const {
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", ("Bearer " + accessToken).toUtf8());
    if (!clientToken.isEmpty()) {
        request.setRawHeader("Client-Token", clientToken.toUtf8());
    }
    if (!spotConnId.isEmpty()) {
        request.setRawHeader("X-Spotify-Connection-Id", spotConnId.toUtf8());
    }
    request.setRawHeader("User-Agent", kUserAgent);
    return request;
}

void SpotifyClient::setVolumeControlSupported(bool supported) {
    volumeControlSupported = supported;
    if (!supported) {
        pendingVolume = -1;
    }
}

QByteArray SpotifyClient::gunzip(const QByteArray &data) {
    if (data.isEmpty()) {
        return {};
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, 15 + 16) != Z_OK) {
        return {};
    }

    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    strm.avail_in = uInt(data.size());

    QByteArray out;
    char buffer[16384];
    int ret = Z_OK;
    do {
        strm.next_out = reinterpret_cast<Bytef *>(buffer);
        strm.avail_out = sizeof(buffer);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&strm);
            return {};
        }
        out.append(buffer, sizeof(buffer) - strm.avail_out);
    } while (ret != Z_STREAM_END && strm.avail_in > 0);

    inflateEnd(&strm);
    return out;
}

void SpotifyClient::logMessage(const QString &msg) {
    std::cout << msg.toStdString() << std::endl;
    emit debugLog(msg);
}

