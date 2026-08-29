#ifndef SPOTIFY_CLIENT_H
#define SPOTIFY_CLIENT_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>

class QWebSocket;
class QTcpServer;

namespace spotify::connectstate { class PlayerState; }

// One entry of the upcoming-songs queue shown in the "Up Next" window.
struct UpcomingTrack {
    QString trackId;
    QString uid;    // queue-entry id; lets skip-to target one exact entry
    QString title;
    QString artist;
    QString artUrl;
    int durationMs = 0;
    bool liked = false;
    bool smartShuffle = false; // injected by Smart Shuffle (recommendation)
};

// SpotifyClient talks to Spotify the way the librespot/go-librespot desktop
// clients do, using only the Web backend (no audio streaming):
//
//   OAuth2 device flow  -> access token + refresh token
//   clienttoken.spotify -> client token
//   apresolve           -> dealer + spclient hosts
//   dealer WebSocket     -> realtime Spotify Connect cluster pushes
//   connect-state        -> register as a passive device, read/observe state,
//                           and remote-control volume / transport
//
// Playback state, position and volume arrive as realtime pushes over the dealer
// (no polling, no Web-API rate limiting). Track display metadata is fetched once
// per song from the Web API.
class SpotifyClient : public QObject {
    Q_OBJECT
public:
    explicit SpotifyClient(QObject *parent = nullptr);

    // Volume is expressed 0-100 at this interface; converted to Connect's
    // 0-65535 range internally. Returns false if the command can't be sent.
    bool setVolume(int volume);
    void togglePlayPause();
    void nextTrack();
    void prevTrack();
    // Jump directly to one entry of the upcoming queue.
    void skipToQueuedTrack(const QString &trackId, const QString &uid);
    // Add/remove the currently playing song to/from the user's Liked Songs.
    void toggleLikeCurrentTrack();
    bool isTrackLiked(const QString &trackId) const;
    bool isCurrentTrackSmartShuffle() const { return lastTrackSmartShuffle; }

    // Begin an interactive OAuth2 device-flow authorization (opens a browser).
    void startAuthorization();
    // Re-establish the session silently using a stored refresh token, if any.
    void resumeSession();

signals:
    void trackChanged(int volume, const QString &track, const QString &artist, const QString &trackId, const QString &albumArtUrl, int progressMs, int durationMs, bool isPlaying, bool volumeControlSupported);
    void stateSynced(int volume, int progressMs, bool isPlaying, bool volumeControlSupported);
    // The stored session is no longer usable; the user must reconnect Spotify.
    void reauthorizationRequired();
    void queueChanged(const QList<UpcomingTrack> &upcoming);
    void trackLikeFinished(bool success, bool liked);
    void likedSongsLoaded();
    void debugLog(const QString &logLine);
    void authComplete();
    // Fired when the device flow needs the user to authorize: url is the
    // verification URL (also opened in the browser), code is the user code.
    void authorizationPending(const QString &url, const QString &code);

private slots:
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketTextMessageReceived(const QString &message);
    void sendWebSocketPing();

private:
    // --- OAuth2: PKCE (primary, yields the username) + device-flow fallback ---
    void startPkceAuthorization();
    void onAuthServerConnection();
    void exchangeAuthCode(const QString &code);
    void requestDeviceCode();
    void pollDeviceToken();
    void applyTokenResponse(const QJsonObject &obj);
    void refreshAccessToken();
    void scheduleProactiveRefresh(int expiresInSeconds);
    bool accessTokenExpired() const;

    // --- session bring-up ---
    void continueSessionBringUp();          // client token -> apresolve -> dealer
    void fetchClientToken();
    void resolveEndpoints();
    void connectWebSocket();

    // --- connect-state ---
    void registerConnectState();
    void handleClusterBytes(const QByteArray &protoBytes, bool isUpdate);
    void fetchTrackDetails(const QString &trackId);
    void sendConnectCommand(const QString &endpoint, const QJsonObject &extraCommandFields = {});

    // --- upcoming queue ---
    void updateUpcomingQueue(const spotify::connectstate::PlayerState &ps);
    void fetchUpcomingTrackDetails(const QStringList &trackIds);
    void clearUpcomingQueue();

    // --- liked songs (spclient collection service) ---
    void ensureUserProfile();
    void resyncLikedTracks();
    void fetchLikedTracks(const QString &paginationToken, int page);
    // Apply a dealer collection push directly (no re-page). Returns false if the
    // payload isn't a well-formed delta, so the caller can fall back to a resync.
    bool applyCollectionDelta(const QByteArray &protoBytes);

    // --- helpers ---
    QNetworkRequest spclientRequest(const QUrl &url) const; // Authorization + Client-Token + Connection-Id
    void logMessage(const QString &msg);
    void setVolumeControlSupported(bool supported);
    static QByteArray gunzip(const QByteArray &data);

    QNetworkAccessManager *network = nullptr;

    // identity / tokens
    QString deviceId;
    QString accessToken;
    QString refreshToken;
    qint64 accessTokenExpiryMs = 0;
    bool refreshInFlight = false;      // guards against concurrent refreshes
    bool reauthPromptSent = false;     // so the reconnect notice fires once
    QTimer *tokenRefreshTimer = nullptr; // proactive refresh + transient retry
    QString clientToken;
    qint64 clientTokenExpiryMs = 0;

    // resolved endpoints
    QString dealerHost = QStringLiteral("dealer.spotify.com");
    QString spclientBaseUrl = QStringLiteral("https://gew4-spclient.spotify.com");
    bool endpointsResolved = false;

    // device flow
    QString deviceCode;
    QTimer *devicePollTimer = nullptr;
    qint64 deviceFlowDeadlineMs = 0;

    // PKCE flow
    QTcpServer *authServer = nullptr;
    QString codeVerifier;
    QString authState;                 // CSRF guard echoed back on the redirect
    QTimer *authServerTimeout = nullptr; // stops the local listener if auth is abandoned

    // connect-state / dealer
    QWebSocket *webSocket = nullptr;
    QTimer *pingTimer = nullptr;
    QString spotConnId;
    QString activeDeviceId;
    bool connectStateRegistered = false;

    // cached playback state (volume kept 0-100)
    QString lastTrackId;
    bool lastTrackSmartShuffle = false;
    bool volumeControlSupported = true;
    int currentVolume = 50;
    int lastProgressMs = 0;
    QElapsedTimer lastProgressTimer;
    int lastDurationMs = 0;
    bool lastIsPlaying = false;
    int pendingVolume = -1;
    QElapsedTimer pendingVolumeTimer;

    // liked songs
    QString username;
    QSet<QString> likedTrackIds;
    QSet<QString> likedSyncBuffer;         // accumulates a full page-walk; swapped in atomically on completion
    bool likedSetRequested = false;        // first load has been kicked off
    bool likedSyncInProgress = false;      // a page-walk is currently running
    QTimer *likedResyncTimer = nullptr;    // periodic full resync, catches likes/unlikes from other clients

    // upcoming queue (cluster next_tracks, metadata resolved lazily)
    QList<UpcomingTrack> lastQueue;
    QString lastQueueSignature;
    QHash<QString, UpcomingTrack> queueMetaCache;
    QSet<QString> pendingQueueLookups;
};

#endif // SPOTIFY_CLIENT_H
