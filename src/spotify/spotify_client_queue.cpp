// SpotifyClient: the Up Next queue — building it from cluster pushes and
// filling the gaps with batched extended-metadata lookups.
//
// Part of the SpotifyClient implementation; see spotify_client_internal.h.
#include "spotify_client.h"
#include "spotify_client_internal.h"

#include <QDateTime>
#include <string>

using namespace spotify_client;

// ---------------------------------------------------------------------------
// Upcoming queue
// ---------------------------------------------------------------------------

void SpotifyClient::clearUpcomingQueue() {
    if (lastQueueSignature.isEmpty() && lastQueue.isEmpty()) {
        return;
    }
    lastQueue.clear();
    lastQueueSignature.clear();
    emit queueChanged({});
}

void SpotifyClient::updateUpcomingQueue(const spotify::connectstate::PlayerState &ps) {
    // The active device pushes its full upcoming list (in shuffle order when
    // shuffled) thanks to needs_full_player_state, so next_tracks is all we
    // need — no playlist resolution.
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
        const auto &meta = pt.metadata();
        auto metaValue = [&meta](const char *key) -> QString {
            auto it = meta.find(key);
            return it != meta.end() ? QString::fromStdString(it->second) : QString();
        };

        UpcomingTrack track;
        track.trackId = uri.section(':', 2, 2);
        track.uid = QString::fromStdString(pt.uid());
        track.title = metaValue("title");
        track.artist = metaValue("artist_name");
        track.durationMs = metaValue("duration").toInt();
        track.liked = isTrackLiked(track.trackId);
        // Smart Shuffle recommendations are tagged provider="enhanced_recommendation".
        track.smartShuffle = metaValue("provider") == "enhanced_recommendation";

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
