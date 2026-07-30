#include "tray_manager.h"
#include "theme.h"

#include <QApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <functional>

namespace {

// The tray/app mark: a line drawing, not a filled disc — a #161826 circle with
// a thin accent stroke and three sound-wave arcs in the accent ramp. Reads at
// 16px on light and dark wallpapers, which the old green disc did not.
QIcon createTrayIcon() {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Filled disc + thin accent ring.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(theme::kBg));
    p.drawEllipse(QPointF(32, 32), 22, 22);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(theme::kAccent), 1.5));
    p.drawEllipse(QPointF(32, 32), 22, 22);

    // Three sound-wave arcs, top to bottom, lightening through the accent ramp.
    const auto arc = [&](QPointF s, QPointF c1, QPointF c2, QPointF e, const char *color) {
        QPainterPath path;
        path.moveTo(s);
        path.cubicTo(c1, c2, e);
        QPen pen(QColor(color), 2.6);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };
    arc({24, 26}, {29, 22},   {37, 22},   {42, 26}, theme::kAccent);
    arc({22, 33}, {29, 27.5}, {39, 27.5}, {44, 33}, theme::kAccent400);
    arc({24, 40}, {29, 36.5}, {37, 36.5}, {41, 40}, theme::kAccent300);

    return QIcon(pixmap);
}

QPixmap roundedThumbnail(const QPixmap &source, int size, qreal radius) {
    QPixmap scaled = source.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPixmap result(size, size);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, size, size), radius, radius);
    painter.setClipPath(path);
    const int x = (size - scaled.width()) / 2;
    const int y = (size - scaled.height()) / 2;
    painter.drawPixmap(x, y, scaled);
    return result;
}

// A clickable, hover-highlighting row inside the menu. Custom widgets are the
// only way to get the design's accent checkmark, right-aligned shortcut hint
// and album-art now-playing line — a plain QAction can't render those.
class ClickableRow : public QWidget {
public:
    explicit ClickableRow(QWidget *parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_StyledBackground, true);
        setAttribute(Qt::WA_Hover, true);
        setCursor(Qt::PointingHandCursor);
        setObjectName("trayRow");
        setStyleSheet(QString(
            "#trayRow { background: transparent; border-radius: %1px; }"
            "#trayRow:hover { background: %2; }")
            .arg(theme::kRadiusSm)
            .arg(theme::kNavHoverTint));
    }

    std::function<void()> onActivate;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && onActivate) {
            onActivate();
        }
        QWidget::mouseReleaseEvent(event);
    }
};

} // namespace

TrayManager::TrayManager(QObject *parent)
    : QObject(parent), network(new QNetworkAccessManager(this)) {
    trayIcon = new QSystemTrayIcon(createTrayIcon(), this);
    trayMenu = new QMenu();
    // 264px surface panel, 6px padding, a #9397ab hairline standing in for
    // shadow-lg (a frameless popup can't paint the ambient blur through a
    // stylesheet). Item text/hover come from the row widgets below.
    trayMenu->setStyleSheet(QString(
        "QMenu { background: %1; border: 1px solid %2; border-radius: %3px; padding: %4px; }"
        "QMenu::separator { height: 1px; background: %5; margin: %6px %4px; }")
        .arg(theme::kSurface)
        .arg(theme::kNeutral500)
        .arg(theme::kRadiusMd)
        .arg(theme::kSpace2)
        .arg(theme::kDivider)
        .arg(theme::kSpace2));
    trayMenu->setFixedWidth(264);

    const auto closeThen = [this](std::function<void()> fn) {
        return [this, fn]() {
            trayMenu->close();
            if (fn) {
                fn();
            }
        };
    };

    // --- Now playing: art + title + artist + liked heart; opens Settings. ---
    ClickableRow *nowRow = new ClickableRow(trayMenu);
    QHBoxLayout *nowLayout = new QHBoxLayout(nowRow);
    nowLayout->setContentsMargins(theme::kSpace2, theme::kSpace2, theme::kSpace2, theme::kSpace2);
    nowLayout->setSpacing(theme::kSpace3);

    nowArtLabel = new QLabel(nowRow);
    nowArtLabel->setFixedSize(34, 34);
    nowArtLabel->setAlignment(Qt::AlignCenter);
    nowArtLabel->setText("♪");
    nowArtLabel->setStyleSheet(QString(
        "border: none; border-radius: %1px; background-color: %2; color: %3; font-size: 15px;")
        .arg(theme::kRadiusSm).arg(theme::kNeutral800).arg(theme::kAccent700));

    QVBoxLayout *nowTextLayout = new QVBoxLayout();
    nowTextLayout->setContentsMargins(0, 0, 0, 0);
    nowTextLayout->setSpacing(0);
    nowTitleLabel = new QLabel("Nothing playing", nowRow);
    nowTitleLabel->setStyleSheet(QString("color: %1; background: transparent; border: none;").arg(theme::kText));
    nowTitleLabel->setFont(theme::uiFont(13, QFont::Medium));
    nowArtistLabel = new QLabel("Spotify", nowRow);
    nowArtistLabel->setStyleSheet(QString("color: %1; background: transparent; border: none;").arg(theme::kNeutral500));
    nowArtistLabel->setFont(theme::uiFont(11));
    nowTextLayout->addWidget(nowTitleLabel);
    nowTextLayout->addWidget(nowArtistLabel);

    nowLikeLabel = new QLabel(nowRow);
    nowLikeLabel->setStyleSheet(QString("color: %1; background: transparent; border: none;").arg(theme::kAccent));
    nowLikeLabel->setFont(theme::uiFont(12));
    nowLikeLabel->setFixedWidth(16);
    nowLikeLabel->setAlignment(Qt::AlignCenter);

    nowLayout->addWidget(nowArtLabel, 0);
    nowLayout->addLayout(nowTextLayout, 1);
    nowLayout->addWidget(nowLikeLabel, 0);
    nowRow->onActivate = closeThen([this]() { emit settingsRequested(); });

    QWidgetAction *nowAction = new QWidgetAction(trayMenu);
    nowAction->setDefaultWidget(nowRow);
    trayMenu->addAction(nowAction);

    trayMenu->addSeparator();

    // A standard text row: [14px check column][label ...][shortcut hint].
    const auto makeRow = [&](const QString &text, const QString &shortcut,
                             QLabel **checkOut) -> ClickableRow * {
        ClickableRow *row = new ClickableRow(trayMenu);
        QHBoxLayout *layout = new QHBoxLayout(row);
        layout->setContentsMargins(theme::kSpace2, 7, theme::kSpace2, 7);
        layout->setSpacing(theme::kSpace3);

        QLabel *check = new QLabel(row);
        check->setFixedWidth(14);
        check->setAlignment(Qt::AlignCenter);
        check->setText(QString::fromUtf8("✓")); // ✓
        check->setStyleSheet(QString("color: %1; background: transparent; border: none;").arg(theme::kAccent));
        check->setFont(theme::uiFont(11));

        QLabel *label = new QLabel(text, row);
        label->setStyleSheet(QString("color: %1; background: transparent; border: none;").arg(theme::kText));
        label->setFont(theme::uiFont(13));

        layout->addWidget(check, 0);
        layout->addWidget(label, 1);
        if (!shortcut.isEmpty()) {
            QLabel *hint = new QLabel(shortcut, row);
            hint->setStyleSheet(QString("color: %1; background: transparent; border: none;").arg(theme::kNeutral600));
            hint->setFont(theme::uiFont(11));
            layout->addWidget(hint, 0);
        }
        if (checkOut) {
            *checkOut = check;
        } else {
            check->setVisible(false); // reserve the column, no checkmark
        }
        return row;
    };

    ClickableRow *showRow = makeRow("Show Up Next", "Alt+U", &showUpNextCheck);
    showUpNextCheck->setVisible(false);
    showRow->onActivate = closeThen([this]() { emit showUpNextToggled(); });
    QWidgetAction *showAction = new QWidgetAction(trayMenu);
    showAction->setDefaultWidget(showRow);
    trayMenu->addAction(showAction);

    ClickableRow *lockRowW = makeRow("Lock it in place", "Alt+L", &lockCheck);
    lockCheck->setVisible(false);
    lockRowW->onActivate = closeThen([this]() { emit lockToggled(); });
    QWidgetAction *lockAction = new QWidgetAction(trayMenu);
    lockAction->setDefaultWidget(lockRowW);
    trayMenu->addAction(lockAction);

    trayMenu->addSeparator();

    ClickableRow *settingsRow = makeRow("Settings…", QString(), nullptr);
    settingsRow->onActivate = closeThen([this]() { emit settingsRequested(); });
    QWidgetAction *settingsAction = new QWidgetAction(trayMenu);
    settingsAction->setDefaultWidget(settingsRow);
    trayMenu->addAction(settingsAction);

    ClickableRow *quitRow = makeRow("Quit Overtune", QString(), nullptr);
    quitRow->onActivate = closeThen([]() { QApplication::quit(); });
    QWidgetAction *quitAction = new QWidgetAction(trayMenu);
    quitAction->setDefaultWidget(quitRow);
    trayMenu->addAction(quitAction);

    connect(trayIcon, &QSystemTrayIcon::activated, this, &TrayManager::handleTrayActivation);
    trayIcon->show();
}

void TrayManager::updateNowPlaying(const QString &track, const QString &artist,
                                   const QString &artUrl, bool liked) {
    const QString title = track.isEmpty() ? QStringLiteral("Nothing playing") : track;
    const QString who = artist.isEmpty() ? QStringLiteral("Spotify") : artist;
    // Elide against the text column (264 minus padding, art, heart, spacing).
    const int budget = 264 - 2 * theme::kSpace2 - theme::kSpace2 - 34 - theme::kSpace3 - 16 - theme::kSpace3;
    nowTitleLabel->setText(QFontMetrics(nowTitleLabel->font()).elidedText(title, Qt::ElideRight, budget));
    nowArtistLabel->setText(QFontMetrics(nowArtistLabel->font()).elidedText(who, Qt::ElideRight, budget));
    nowLikeLabel->setText(liked ? QString::fromUtf8("♥") : QString()); // ♥ only when liked

    if (!artUrl.isEmpty() && artUrl != lastArtUrl) {
        lastArtUrl = artUrl;
        QNetworkReply *reply = network->get(QNetworkRequest(QUrl(artUrl)));
        connect(reply, &QNetworkReply::finished, this, [this, reply]() { onArtDownloaded(reply); });
    }
}

void TrayManager::onArtDownloaded(QNetworkReply *reply) {
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }
    QPixmap pixmap;
    if (!pixmap.loadFromData(reply->readAll())) {
        return;
    }
    nowArtLabel->setText("");
    nowArtLabel->setStyleSheet("border: none; background: transparent;");
    nowArtLabel->setPixmap(roundedThumbnail(pixmap, 34, theme::kRadiusSm));
}

void TrayManager::setQueueEnabled(bool enabled) {
    showUpNextCheck->setVisible(enabled);
}

void TrayManager::setQueueLocked(bool locked) {
    lockCheck->setVisible(locked);
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
