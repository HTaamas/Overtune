// SpotifyClient: the realtime half — the dealer WebSocket, registering as a
// connect-state observer device, and turning cluster pushes into player state.
//
// Part of the SpotifyClient implementation; see spotify_client_internal.h.
#include "spotify_client.h"
#include "spotify_client_internal.h"
#include "settings/app_settings.h"

#include <QDateTime>
#include <QSslError>
#include <QWebSocket>
#include <string>

using namespace spotify_client;

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
        return;
    }

    // Liked Songs changed on some device: the backend pushes the delta here, so
    // this stays in sync without polling. Two variants fire per change — a proto
    // payload we parse and an opaque "/json" one we ignore.
    if (uri.startsWith("hm://collection/collection/")) {
        // Apply the delta straight from the push proto — no re-page — and only
        // fall back to a full resync if the payload won't parse.
        if (!uri.endsWith("/json")) {
            const QJsonObject headers = obj.value("headers").toObject();
            const QJsonArray payloads = obj.value("payloads").toArray();
            QByteArray payload;
            if (!payloads.isEmpty()) {
                payload = QByteArray::fromBase64(payloads.first().toString().toUtf8());
                if (headers.value("Transfer-Encoding").toString() == "gzip") {
                    payload = gunzip(payload);
                }
            }
            if (!applyCollectionDelta(payload)) {
                logMessage("[like] collection push unparseable; resyncing");
                resyncLikedTracks();
            }
        }
        return;
    }

    // Playlist-domain pushes (algorithmic playlist revisions, the per-artist
    // "Liked Songs" views) ride the same socket and fire on every like, but this
    // app doesn't surface playlists — drop them quietly so they don't drown the
    // log. Known-irrelevant, so no need to keep rediscovering them.
    if (uri.startsWith("hm://playlist/") ||
        uri.startsWith("hm://herodotus/uri/spotify:list:play-history:v1/resume-point-revision/") ||
        uri.startsWith("social-connect/v2/broadcast_status_update")) {
        return;
    }

    // Discovery: log anything genuinely new we don't yet route. Handy for mapping
    // further topics off the wire.
    if (!uri.isEmpty()) {
        const QJsonArray payloads = obj.value("payloads").toArray();
        logMessage(QString("[WS] unhandled push uri=%1 payloads=%2")
                       .arg(uri).arg(payloads.size()));
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
    info->set_device_software_version("overtune 1.0");
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
    // Ask the active device to push its full player state, so next_tracks
    // carries the complete upcoming list (in shuffle order when shuffled)
    // instead of a ~2-track window.
    cap->set_needs_full_player_state(true);

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
    lastProgressTimer.start();
    lastDurationMs = durationMs;
    lastIsPlaying = isPlaying;
    setVolumeControlSupported(volumeSupported);

    const QString trackUri = QString::fromStdString(ps.track().uri());
    const QString trackId = trackUri.startsWith("spotify:track:") ? trackUri.section(':', 2, 2) : QString();

    // Is the currently playing track a Smart Shuffle recommendation?
    {
        const auto &curMeta = ps.track().metadata();
        const auto it = curMeta.find("provider");
        lastTrackSmartShuffle = it != curMeta.end() && it->second == "enhanced_recommendation";
    }

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
