#include "tray_manager.h"
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QApplication>
#include <QTimer>

namespace {
QIcon createTrayIcon() {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    QRadialGradient glow(32, 20, 28, 28, 18);
    glow.setColorAt(0.0, QColor(80, 255, 180, 120));
    glow.setColorAt(0.45, QColor(23, 173, 110, 80));
    glow.setColorAt(1.0, QColor(0, 0, 0, 0));
    painter.setBrush(glow);
    painter.drawEllipse(QRectF(6, 4, 52, 52));

    QLinearGradient discGradient(14, 10, 50, 56);
    discGradient.setColorAt(0.0, QColor("#34f5a0"));
    discGradient.setColorAt(0.55, QColor("#1db954"));
    discGradient.setColorAt(1.0, QColor("#0d6f41"));
    painter.setBrush(discGradient);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(10, 10, 44, 44));

    painter.setBrush(QColor(8, 20, 15, 210));
    painter.drawEllipse(QRectF(17, 17, 30, 30));

    QPainterPath waveTop;
    waveTop.moveTo(24, 26);
    waveTop.cubicTo(29, 22, 37, 22, 42, 26);
    waveTop.cubicTo(37, 25, 29, 25, 24, 29);
    waveTop.closeSubpath();

    QPainterPath waveMid;
    waveMid.moveTo(22, 32);
    waveMid.cubicTo(29, 27, 39, 27, 44, 32);
    waveMid.cubicTo(38, 31, 28, 31, 22, 35);
    waveMid.closeSubpath();

    QPainterPath waveLow;
    waveLow.moveTo(24, 38);
    waveLow.cubicTo(29, 35, 37, 35, 41, 38);
    waveLow.cubicTo(36, 38, 30, 38, 24, 41);
    waveLow.closeSubpath();

    painter.setBrush(QColor(232, 255, 243, 235));
    painter.drawPath(waveTop);
    painter.drawPath(waveMid);
    painter.drawPath(waveLow);

    return QIcon(pixmap);
}
}

TrayManager::TrayManager(QObject *parent) : QObject(parent) {
    trayIcon = new QSystemTrayIcon(createTrayIcon(), this);
    trayMenu = new QMenu();

    trackAction = trayMenu->addAction("Current Playing: None");
    trackAction->setEnabled(false);

    settingsAction = trayMenu->addAction("Settings");
    connect(settingsAction, &QAction::triggered, this, &TrayManager::settingsRequested);

    trayMenu->addSeparator();
    QAction *quitAction = trayMenu->addAction("Quit");
    connect(quitAction, &QAction::triggered, QApplication::quit);

    connect(trayIcon, &QSystemTrayIcon::activated, this, &TrayManager::handleTrayActivation);
    trayIcon->show();
}

void TrayManager::updateTrackInfo(const QString &track, const QString &artist) {
    trackAction->setText("Current Playing: " + track + " - " + artist);
}

void TrayManager::showNotification(const QString &title, const QString &message) {
    if (trayIcon && QSystemTrayIcon::supportsMessages()) {
        trayIcon->showMessage(title, message, QSystemTrayIcon::Warning, 10000);
    }
}

void TrayManager::handleTrayActivation(QSystemTrayIcon::ActivationReason reason) {
    #if defined(_WIN32) || defined(__linux__)
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        QTimer::singleShot(0, this, [this]() {
            emit settingsRequested();
        });
    } else if (reason == QSystemTrayIcon::Context) {
        trayMenu->popup(QCursor::pos());
    }
    #endif
    #ifdef __APPLE__
    if (reason == QSystemTrayIcon::Trigger) {
        if (!menuVisible) {
            trayMenu->popup(QCursor::pos());
            menuVisible = true;
        } else {
            trayMenu->hide();
            menuVisible = false;
        }
    }
    #endif
}
