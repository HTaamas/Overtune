#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <QSettings>
#include <QString>
#include <climits>

// Defaults are the Nocturne design tokens (see src/ui/theme.h, the source of
// truth for these literals). Kept as literals here so this core settings header
// stays free of the UI/Windows includes theme.h pulls in.
struct OverlaySettings {
    QString backgroundColor = "#161826";  // theme::kBg — ground
    QString surfaceColor = "#232532";     // theme::kSurface — chips, sidebars, menu ground
    QString borderColor = "#3f424d";      // theme::kNeutral800 — rail troughs, art placeholder ground
    QString accentColor = "#9184d9";      // theme::kAccent — blurple line/glow/mark
    QString primaryTextColor = "#e9e9ed"; // theme::kText
    QString secondaryTextColor = "#9397ab"; // theme::kNeutral500 — artist / secondary
    QString mutedTextColor = "#75798c";   // theme::kNeutral600 — time, index, muted
    QString progressBarColor = "#cfd3e5"; // theme::kNeutral300 — song-progress fill
    int overlayWidth = 496;
    int hideDurationMs = 3500;
    // Which named theme preset these colours came from ("Nocturne", "Ink",
    // "Lifted", "Indigo"), or "Custom" once any field is edited by hand. Only a
    // UI hint for showing which preset is current; the colours above are
    // authoritative.
    QString presetName = "Nocturne";
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
    QString hoverColor = "#e9e9ed"; // theme::kText — row hover tint (painted at ~15/255 alpha)
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
