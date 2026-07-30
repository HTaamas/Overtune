#ifndef TRAY_MANAGER_H
#define TRAY_MANAGER_H

#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QPoint>
#include <QString>

class QLabel;
class QNetworkAccessManager;
class QNetworkReply;

class TrayManager : public QObject {
    Q_OBJECT
public:
    explicit TrayManager(QObject *parent = nullptr);
    // Now-playing item at the top of the menu (art + title + artist + liked).
    void updateNowPlaying(const QString &track, const QString &artist,
                          const QString &artUrl, bool liked);
    // Keep the two toggle rows' checkmarks in sync when the state changes
    // elsewhere (hotkeys, the settings dialog).
    void setQueueEnabled(bool enabled);
    void setQueueLocked(bool locked);
    void showNotification(const QString &title, const QString &message);

signals:
    void settingsRequested();
    // The user picked "Show Up Next" / "Lock it in place" in the menu; the app
    // flips the setting and calls setQueueEnabled/setQueueLocked back to sync.
    void showUpNextToggled();
    void lockToggled();

private slots:
    void handleTrayActivation(QSystemTrayIcon::ActivationReason reason);

private:
    void onArtDownloaded(QNetworkReply *reply);

    QSystemTrayIcon *trayIcon;
    QMenu *trayMenu;
    QLabel *nowArtLabel;
    QLabel *nowTitleLabel;
    QLabel *nowArtistLabel;
    QLabel *nowLikeLabel;
    QLabel *showUpNextCheck;
    QLabel *lockCheck;
    QNetworkAccessManager *network;
    QString lastArtUrl;
    #ifdef Q_OS_MAC
    bool menuVisible = false;
    #endif
};

#endif // TRAY_MANAGER_H
