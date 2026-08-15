// SpotifyClient: outbound playback control — volume, transport, and jumping to
// a queued track, all issued as connect-state commands to the active device.
//
// Part of the SpotifyClient implementation; see spotify_client_internal.h.
#include "spotify_client.h"
#include "spotify_client_internal.h"

#include <QDateTime>

using namespace spotify_client;

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
    // Match the common player behaviour: past the first few seconds, "previous"
    // restarts the current track rather than jumping to the one before it.
    if (lastProgressMs > kPrevRestartThresholdMs) {
        sendConnectCommand("seek_to", QJsonObject{{"value", 0}});
    } else {
        sendConnectCommand("skip_prev");
    }
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
