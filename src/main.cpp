#include <QApplication>
#include "spotify/spotify_client.h"
#include "ui/osd_window.h"
#include "ui/tray_manager.h"
#include "input/volume_handler.h"
#include "settings/app_settings.h"
#include "ui/settings_dialog.h"
#include "ui/queue_window.h"
#include <QMessageBox>
#include <QMetaObject>
#include <QIcon>
#include <QDir>
#include <atomic>
#include <exception>
#include <QDebug>
#include <QElapsedTimer>
#include <QSignalBlocker>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#ifdef __APPLE__
void configureMacApplicationBehavior();
#endif

namespace {
QString bundledAppIconPath() {
#ifdef __APPLE__
    return QDir(QCoreApplication::applicationDirPath()).filePath("../Resources/app_icon.png");
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath("app_icon.png");
#endif
}

void appMessageHandler(QtMsgType type, const QMessageLogContext &, const QString &msg) {
    static std::atomic_bool dialogQueued{false};

    if (type == QtCriticalMsg || type == QtFatalMsg) {
        if (QApplication::instance() && !dialogQueued.exchange(true)) {
            const QString errorText = msg;
            QMetaObject::invokeMethod(QApplication::instance(), [errorText]() {
                QMessageBox::critical(nullptr, "SpotifyVol Error", errorText);
                dialogQueued.store(false);
            }, Qt::QueuedConnection);
        }
    }

    const QByteArray local = msg.toLocal8Bit();
    fprintf(stderr, "%s\n", local.constData());

    if (type == QtFatalMsg) {
        abort();
    }
}

#ifdef _WIN32
bool isProcessElevated() {
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID adminGroup = nullptr;
    BOOL isMember = FALSE;

    if (!AllocateAndInitializeSid(
            &ntAuthority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &adminGroup)) {
        return false;
    }

    const BOOL checkResult = CheckTokenMembership(nullptr, adminGroup, &isMember);
    FreeSid(adminGroup);

    return checkResult && isMember;
}

bool ensureAdminPrivileges() {
    if (isProcessElevated()) {
        return true;
    }

    wchar_t exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        const HINSTANCE elevateResult = ShellExecuteW(
            nullptr,
            L"runas",
            exePath,
            nullptr,
            nullptr,
            SW_SHOWNORMAL
        );
        if (reinterpret_cast<INT_PTR>(elevateResult) > 32) {
            // Relaunch succeeded; current non-elevated process should exit.
            return false;
        }
    }

    MessageBoxW(
        nullptr,
        L"SpotifyVol requires administrator privileges and could not auto-elevate.",
        L"SpotifyVol",
        MB_OK | MB_ICONERROR
    );

    return false;
}
#endif

}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    if (!ensureAdminPrivileges()) {
        return 0;
    }
#endif

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    qInstallMessageHandler(appMessageHandler);
    app.setWindowIcon(QIcon(bundledAppIconPath()));
#ifdef __APPLE__
    configureMacApplicationBehavior();
#endif

    SpotifyClient spotify;
    OSDWindow osd;
    TrayManager tray;
    VolumeHandler volHandler;
    SettingsDialog settingsDialog;
    QueueWindow queueWindow;
    settingsDialog.setOverlaySettings(AppSettings::loadOverlaySettings());
    settingsDialog.setKeybindSettings(AppSettings::loadKeybindSettings());
    settingsDialog.setQueueSettings(AppSettings::loadQueueSettings());
    osd.applyOverlaySettings(settingsDialog.overlaySettings());
    volHandler.applyKeybindSettings(settingsDialog.keybindSettings());
    queueWindow.applyOverlaySettings(settingsDialog.overlaySettings());
    queueWindow.applyQueueSettings(settingsDialog.queueSettings());

    int currentVolume = 50;
    QString currentTrack = "Loading...";
    QString currentTrackId = "";
    QString currentArtist = "Spotify";
    QString currentArtUrl = "";
    int currentProgress = 0;
    int currentDuration = 0;
    bool currentIsPlaying = false;
    bool currentVolumeControlSupported = true;
    QElapsedTimer progressClock;

    auto updateProgressBaseline = [&](int progressMs, bool isPlaying) {
        currentProgress = progressMs;
        currentIsPlaying = isPlaying;

        if (currentIsPlaying) {
            if (progressClock.isValid()) {
                progressClock.restart();
            } else {
                progressClock.start();
            }
        }
    };

    auto estimatedProgressNow = [&]() {
        if (!progressClock.isValid() || !currentIsPlaying) {
            return currentProgress;
        }
        int estimated = currentProgress + static_cast<int>(progressClock.elapsed());
        if (currentDuration > 0) {
            estimated = qBound(0, estimated, currentDuration);
        }
        return estimated;
    };

    QObject::connect(&spotify, &SpotifyClient::trackChanged, [&](int volume, const QString &track, const QString &artist, const QString &trackId, const QString &albumArtUrl, int progressMs, int durationMs, bool isPlaying, bool volumeControlSupported) {
        currentVolume = volume;
        currentTrack = track;
        currentTrackId = trackId;
        currentArtist = artist;
        currentArtUrl = albumArtUrl;
        currentVolumeControlSupported = volumeControlSupported;
        updateProgressBaseline(progressMs, isPlaying);
        currentDuration = durationMs;
        osd.setLikedState(spotify.isTrackLiked(trackId), spotify.isCurrentTrackSmartShuffle());
        osd.showVolume(currentVolume, currentTrack, currentArtist, currentArtUrl, estimatedProgressNow(), currentDuration, currentIsPlaying, currentVolumeControlSupported);
        queueWindow.setNowPlaying(trackId, currentTrack, currentArtist, currentArtUrl, estimatedProgressNow(), currentDuration, currentIsPlaying);
        tray.updateTrackInfo(currentTrack, currentArtist);
    });


    QObject::connect(&spotify, &SpotifyClient::debugLog, &settingsDialog, &SettingsDialog::appendLog);

    QObject::connect(&tray, &TrayManager::settingsRequested, [&]() {
        settingsDialog.show();
        settingsDialog.raise();
        settingsDialog.activateWindow();
    });

    QObject::connect(&settingsDialog, &SettingsDialog::connectSpotifyRequested, [&]() {
        spotify.startAuthorization();
    });

    QObject::connect(&spotify, &SpotifyClient::authorizationPending, [&](const QString &url, const QString &code) {
        settingsDialog.showAuthorizationPrompt(url, code);
    });

    QObject::connect(&spotify, &SpotifyClient::authComplete, [&]() {
        settingsDialog.setAuthenticated(true);
    });

    QObject::connect(&settingsDialog, &SettingsDialog::overlaySettingsChanged, [&]() {
        const OverlaySettings settings = settingsDialog.overlaySettings();
        AppSettings::saveOverlaySettings(settings);
        osd.applyOverlaySettings(settings);
        queueWindow.applyOverlaySettings(settings);
    });

    QObject::connect(&settingsDialog, &SettingsDialog::queueSettingsChanged, [&]() {
        const QueueSettings settings = settingsDialog.queueSettings();
        AppSettings::saveQueueSettings(settings);
        queueWindow.applyQueueSettings(settings);
    });

    QObject::connect(&spotify, &SpotifyClient::queueChanged, &queueWindow, &QueueWindow::setQueue);

    QObject::connect(&queueWindow, &QueueWindow::trackActivated, [&](const QString &trackId, const QString &uid) {
        spotify.skipToQueuedTrack(trackId, uid);
    });

    QObject::connect(&queueWindow, &QueueWindow::windowMoved, [&](const QPoint &topLeft) {
        QueueSettings settings = settingsDialog.queueSettings();
        settings.windowX = topLeft.x();
        settings.windowY = topLeft.y();
        AppSettings::saveQueueSettings(settings);
        const QSignalBlocker blocker(&settingsDialog);
        settingsDialog.setQueueSettings(settings);
    });

    QObject::connect(&volHandler, &VolumeHandler::likeSongRequested, [&]() {
        spotify.toggleLikeCurrentTrack();
    });

    QObject::connect(&spotify, &SpotifyClient::likedSongsLoaded, [&]() {
        osd.setLikedState(spotify.isTrackLiked(currentTrackId), spotify.isCurrentTrackSmartShuffle());
    });

    QObject::connect(&spotify, &SpotifyClient::trackLikeFinished, [&](bool success, bool liked) {
        if (success) {
            osd.setLikedState(liked, spotify.isCurrentTrackSmartShuffle());
        }
        osd.showVolume(currentVolume, currentTrack, currentArtist, currentArtUrl, estimatedProgressNow(), currentDuration, currentIsPlaying, currentVolumeControlSupported);
    });

    QObject::connect(&volHandler, &VolumeHandler::toggleQueueLockRequested, [&]() {
        QueueSettings settings = settingsDialog.queueSettings();
        settings.locked = !settings.locked;
        AppSettings::saveQueueSettings(settings);
        queueWindow.applyQueueSettings(settings);
        const QSignalBlocker blocker(&settingsDialog);
        settingsDialog.setQueueSettings(settings);
    });

    QObject::connect(&queueWindow, &QueueWindow::windowResized, [&](int windowWidth) {
        QueueSettings settings = settingsDialog.queueSettings();
        settings.windowWidth = windowWidth;
        AppSettings::saveQueueSettings(settings);
        const QSignalBlocker blocker(&settingsDialog);
        settingsDialog.setQueueSettings(settings);
    });

    QObject::connect(&settingsDialog, &SettingsDialog::keybindSettingsChanged, [&]() {
        const KeybindSettings settings = settingsDialog.keybindSettings();
        AppSettings::saveKeybindSettings(settings);
        volHandler.applyKeybindSettings(settings);
    });

    QObject::connect(&spotify, &SpotifyClient::stateSynced, [&](int volume, int progressMs, bool isPlaying, bool volumeControlSupported) {
        const bool playbackStateChanged = (isPlaying != currentIsPlaying);
        currentVolume = volume;
        currentVolumeControlSupported = volumeControlSupported;
        updateProgressBaseline(progressMs, isPlaying);
        osd.syncProgress(progressMs, isPlaying, volumeControlSupported);
        queueWindow.syncNowPlayingProgress(progressMs, isPlaying);

        if (playbackStateChanged) {
            osd.showVolume(currentVolume, currentTrack, currentArtist, currentArtUrl, progressMs, currentDuration, currentIsPlaying, currentVolumeControlSupported);
        }
    });

    QObject::connect(&volHandler, &VolumeHandler::volumeChanged, [&](int delta) {
        const int requestedVolume = qBound(0, currentVolume + delta, 100);
        if (!spotify.setVolume(requestedVolume)) {
            if (!currentVolumeControlSupported) {
                osd.showVolume(currentVolume, currentTrack, currentArtist, currentArtUrl, estimatedProgressNow(), currentDuration, currentIsPlaying, false);
            }
            return;
        }

        currentVolume = requestedVolume;
        osd.showVolume(currentVolume, currentTrack, currentArtist, currentArtUrl, estimatedProgressNow(), currentDuration, currentIsPlaying, currentVolumeControlSupported);
    });

    QObject::connect(&volHandler, &VolumeHandler::toggleMusic, [&]() {
        spotify.togglePlayPause();

        osd.showVolume(currentVolume, currentTrack, currentArtist, currentArtUrl, estimatedProgressNow(), currentDuration, currentIsPlaying, currentVolumeControlSupported);
    });

    QObject::connect(&volHandler, &VolumeHandler::nextTrack, [&]() {
        spotify.nextTrack();

        osd.showVolume(currentVolume, currentTrack, currentArtist, currentArtUrl, estimatedProgressNow(), currentDuration, currentIsPlaying, currentVolumeControlSupported);
    });

    QObject::connect(&volHandler, &VolumeHandler::prevTrack, [&]() {
        spotify.prevTrack();

        osd.showVolume(currentVolume, currentTrack, currentArtist, currentArtUrl, estimatedProgressNow(), currentDuration, currentIsPlaying, currentVolumeControlSupported);
    });

    QTimer::singleShot(1000, [&spotify]() {
        spotify.resumeSession();
    });

    try {
        return app.exec();
    } catch (const std::exception &ex) {
        QMessageBox::critical(nullptr, "SpotifyVol Fatal Error", QString("Unhandled exception: %1").arg(ex.what()));
    } catch (...) {
        QMessageBox::critical(nullptr, "SpotifyVol Fatal Error", "Unhandled unknown exception.");
    }

    return 1;
}
