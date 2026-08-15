// SpotifyClient: Liked Songs. Kept in sync by the dealer's collection push
// (applied as a delta, no round-trip), with a full re-page as the fallback.
// The Web API equivalent 429s this client id, hence the spclient service.
//
// Part of the SpotifyClient implementation; see spotify_client_internal.h.
#include "spotify_client.h"
#include "spotify_client_internal.h"
#include "settings/app_settings.h"

#include <QDateTime>
#include <QUuid>

using namespace spotify_client;

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
        resyncLikedTracks();
    }

    // Steady-state freshness comes from the dealer collection push (see
    // onWebSocketTextMessageReceived). This slow timer is only a safety net for
    // pushes missed while the socket was down — hence minutes, not seconds.
    if (!likedResyncTimer) {
        likedResyncTimer = new QTimer(this);
        likedResyncTimer->setInterval(5 * 60 * 1000);
        connect(likedResyncTimer, &QTimer::timeout, this, [this]() { resyncLikedTracks(); });
        likedResyncTimer->start();
    }
}

// A track's 16-byte GID (as carried in collection pushes and metadata) maps to
// the 22-char base62 id used in "spotify:track:<id>" and in likedTrackIds. It's
// a straight base conversion of the 128-bit value, zero-padded to 22 digits.
static QString gidToBase62(const std::string &gid) {
    if (gid.size() != 16) {
        return QString();
    }
    static const char kAlphabet[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    unsigned __int128 n = 0;
    for (unsigned char byte : gid) {
        n = (n << 8) | byte;
    }
    char out[22];
    for (int i = 21; i >= 0; --i) {
        out[i] = kAlphabet[int(n % 62)];
        n /= 62;
    }
    return QString::fromLatin1(out, 22);
}

// Apply a dealer collection push (hm://collection/collection/<user>) in place,
// with no network round-trip. The payload is a CollectionUpdate: each item is a
// changed track (raw GID) with is_removed telling like from unlike. Returns
// false if the bytes aren't a well-formed delta so the caller can resync.
bool SpotifyClient::applyCollectionDelta(const QByteArray &protoBytes) {
    spotify::collection::CollectionUpdate update;
    if (!update.ParseFromArray(protoBytes.constData(), int(protoBytes.size()))) {
        return false;
    }

    bool sawValidItem = false;
    bool changed = false;
    for (const auto &item : update.items()) {
        if (item.item_gid().size() != 16) {
            continue; // not a track GID we can map — treat payload as unknown
        }
        sawValidItem = true;
        const QString id = gidToBase62(item.item_gid());
        if (item.is_removed()) {
            if (likedTrackIds.remove(id)) {
                changed = true;
            }
        } else if (!likedTrackIds.contains(id)) {
            likedTrackIds.insert(id);
            changed = true;
        }
    }
    if (!sawValidItem) {
        return false; // parsed to something, but nothing we understand — resync
    }

    if (changed) {
        logMessage(QString("[like] applied collection push (%1 tracks liked)")
                       .arg(likedTrackIds.size()));
        emit likedSongsLoaded();
        // Keep the queue's heart flags in step with the change.
        bool queueChangedFlag = false;
        for (UpcomingTrack &t : lastQueue) {
            const bool liked = isTrackLiked(t.trackId);
            if (t.liked != liked) {
                t.liked = liked;
                queueChangedFlag = true;
            }
        }
        if (queueChangedFlag) {
            lastQueueSignature = queueSignatureFor(lastQueue);
            emit queueChanged(lastQueue);
        }
    }
    return true;
}

// Re-page the whole collection into a fresh buffer and swap it in on completion.
// Rebuilding (rather than merging) is what lets an unlike made elsewhere drop
// out of our set — the old code only ever inserted, so removals never landed.
void SpotifyClient::resyncLikedTracks() {
    if (username.isEmpty() || likedSyncInProgress) {
        return;
    }
    likedSyncInProgress = true;
    likedSyncBuffer.clear();
    fetchLikedTracks(QString(), 0);
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
            likedSyncInProgress = false;
            refreshAccessToken();
            QTimer::singleShot(5000, this, [this]() { ensureUserProfile(); });
            return;
        }
        if (status != 200) {
            likedSyncInProgress = false; // let the next poll retry rather than wedging
            logMessage(QString("[like] loading Liked Songs failed (HTTP %1): %2")
                           .arg(status).arg(QString::fromUtf8(data.left(200))));
            return;
        }

        spotify::collection::PageResponse resp;
        if (!resp.ParseFromArray(data.constData(), int(data.size()))) {
            likedSyncInProgress = false;
            logMessage("[like] could not parse the Liked Songs page");
            return;
        }

        for (const auto &item : resp.items()) {
            const QString uri = QString::fromStdString(item.uri());
            if (!item.is_removed() && uri.startsWith("spotify:track:")) {
                likedSyncBuffer.insert(uri.section(':', 2, 2));
            }
        }

        const QString nextToken = QString::fromStdString(resp.next_page_token());
        if (!nextToken.isEmpty() && page < 200) {
            fetchLikedTracks(nextToken, page + 1);
            return;
        }

        // Page-walk complete: swap the freshly-built set in atomically.
        likedSyncInProgress = false;
        const bool setChanged = (likedSyncBuffer != likedTrackIds);
        likedTrackIds = likedSyncBuffer;
        likedSyncBuffer.clear();
        logMessage(QString("[like] Liked Songs loaded (%1 tracks)").arg(likedTrackIds.size()));

        if (!setChanged) {
            return; // nothing moved since the last poll — don't churn the UI
        }
        emit likedSongsLoaded();
        // Refresh the queue's heart flags against the new set (the queue may have
        // been built before this, or an external like/unlike may have landed).
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
            // If a background resync is mid page-walk, mirror the change into its
            // buffer too, so the imminent swap can't clobber this local toggle
            // with a server view that hasn't indexed the write yet.
            if (likedSyncInProgress) {
                if (makeLiked) {
                    likedSyncBuffer.insert(trackId);
                } else {
                    likedSyncBuffer.remove(trackId);
                }
            }
        }
        logMessage(success
            ? QString("[like] %1 %2 Liked Songs").arg(trackId, makeLiked ? "added to" : "removed from")
            : QString("[like] updating %1 failed (HTTP %2)").arg(trackId).arg(status));
        emit trackLikeFinished(success, likedTrackIds.contains(trackId));
    });
}
