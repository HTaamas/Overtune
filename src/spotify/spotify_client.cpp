// SpotifyClient: construction, wiring, and the shared helpers every other unit
// leans on. The rest of the class lives alongside this file:
//
//   spotify_client_auth.cpp       OAuth flows, token refresh, session bring-up
//   spotify_client_dealer.cpp     dealer WebSocket, connect-state, cluster push
//   spotify_client_queue.cpp      Up Next queue + extended-metadata lookups
//   spotify_client_remote.cpp     volume / transport commands
//   spotify_client_collection.cpp Liked Songs
//
// Constants and the protobuf include block are shared via spotify_client_internal.h.
#include "spotify_client.h"
#include "spotify_client_internal.h"
#include "settings/app_settings.h"

#include <QSslError>
#include <QWebSocket>
#include <iostream>
#include <zlib.h>

using namespace spotify_client;

SpotifyClient::SpotifyClient(QObject *parent)
    : QObject(parent), network(new QNetworkAccessManager(this)) {
    deviceId = AppSettings::loadOrCreateDeviceId();

    devicePollTimer = new QTimer(this);
    connect(devicePollTimer, &QTimer::timeout, this, &SpotifyClient::pollDeviceToken);

    // Refreshes the access token before it expires (and retries transient
    // refresh failures), so the session never lapses while the app runs.
    tokenRefreshTimer = new QTimer(this);
    tokenRefreshTimer->setSingleShot(true);
    connect(tokenRefreshTimer, &QTimer::timeout, this, &SpotifyClient::refreshAccessToken);

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

