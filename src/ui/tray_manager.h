#ifndef TRAY_MANAGER_H
#define TRAY_MANAGER_H

#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QPoint>

class TrayManager : public QObject {
    Q_OBJECT
public:
    explicit TrayManager(QObject *parent = nullptr);
    void updateTrackInfo(const QString &track, const QString &artist);
    void showNotification(const QString &title, const QString &message);

signals:
    void settingsRequested();

private slots:
    void handleTrayActivation(QSystemTrayIcon::ActivationReason reason);

private:
    QSystemTrayIcon *trayIcon;
    QMenu *trayMenu;
    QAction *trackAction;
    QAction *settingsAction;
    #ifdef Q_OS_MAC
    bool menuVisible = false;
    #endif
};

#endif // TRAY_MANAGER_H
