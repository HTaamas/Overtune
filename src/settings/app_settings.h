#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <QSettings>
#include <QString>
#include <climits>

struct OverlaySettings {
    QString backgroundColor = "#1c1c1c";
    QString borderColor = "#333333";
    QString accentColor = "#1DB954";
    QString primaryTextColor = "#ffffff";
    QString secondaryTextColor = "#aaaaaa";
    QString mutedTextColor = "#888888";
    QString progressBarColor = "#ffffff";
    int overlayWidth = 440;
    int hideDurationMs = 3500;
};

struct KeybindSettings {
    int coarseStep = 5;
    int fineStep = 1;
    bool useShiftForFineAdjust = true;
    QString mainKey = "0x14"; // VK_CAPITAL (Caps Lock)
    QString likeKey = "0x53"; // 'S'; pressed together with Alt to like the song
    QString lockKey = "0x4C"; // 'L'; Alt+L toggles the Up Next window lock
    QString showKey = "0x55"; // 'U'; Alt+U shows/hides the Up Next window
};

struct QueueSettings {
    bool enabled = false;
    bool showNowPlaying = true;
    // Locked = click-through: the window ignores the mouse entirely
    // (no drag, no resize) until unlocked again. Toggled with Alt+U.
    bool locked = false;
    bool showLockIcon = true;
    int maxSongs = 5;
    int opacityPercent = 100;
    QString hoverColor = "#888888";
    // Last dragged position of the queue window; INT_MIN means "never placed"
    // and the window picks a default spot on the primary screen.
    int windowX = INT_MIN;
    int windowY = INT_MIN;
    // Last dragged width; -1 means "follow the OSD overlay width".
    int windowWidth = -1;
};

namespace AppSettings {
OverlaySettings loadOverlaySettings();
void saveOverlaySettings(const OverlaySettings &settings);

KeybindSettings loadKeybindSettings();
void saveKeybindSettings(const KeybindSettings &settings);

QueueSettings loadQueueSettings();
void saveQueueSettings(const QueueSettings &settings);

// Persisted refresh token from the OAuth2 device flow (used for silent re-auth).
QString loadRefreshToken();
void saveRefreshToken(const QString &token);
void clearRefreshToken();

// Stable per-install Spotify Connect device id (40 hex chars). Generated once.
QString loadOrCreateDeviceId();

// Cached Spotify user id (fetched once; needed for collection writes).
QString loadUsername();
void saveUsername(const QString &username);

// Opt-in: relaunch elevated at startup so hotkeys work over admin windows.
// Off by default, so the app needs no administrator rights.
bool loadRunAsAdmin();
void saveRunAsAdmin(bool enabled);
}

#endif // APP_SETTINGS_H
