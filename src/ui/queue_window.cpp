#include "queue_window.h"

#include <QEasingCurve>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#ifdef __APPLE__
void applyMacOverlayWindowBehavior(QWidget *widget);
#endif

namespace {
constexpr int kArtSize = 36;
constexpr int kNowArtSize = 48;
constexpr int kNowHeight = 58;
constexpr int kNowGap = 12;
constexpr int kRowHeight = 36;
constexpr int kRowGap = 6;
constexpr int kMarginX = 15;
constexpr int kMarginTop = 12;
constexpr int kMarginBottom = 12;
constexpr int kHeaderGap = 8;
constexpr int kResizeGripPx = 6;
constexpr int kMinWidth = 240;
constexpr int kMaxWidth = 900;
constexpr int kFadeMs = 220;
constexpr int kHeightAnimMs = 260;
constexpr int kSlideMs = 280;
constexpr int kRowFadeInMs = 240;
constexpr int kRowFadeOutMs = 180;
constexpr int kRowEnterOffsetPx = 14;
constexpr int kLockIconSize = 14;

QPixmap lockPixmap(int size, const QColor &color) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Shackle
    QPen pen(color, qMax(1.5, size * 0.13));
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(size * 0.26, size * 0.10, size * 0.48, size * 0.52), 0, 180 * 16);

    // Body
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(size * 0.15, size * 0.44, size * 0.70, size * 0.48), size * 0.14, size * 0.14);
    return pm;
}

QPixmap roundedThumbnail(const QPixmap &source, int targetSize, qreal radius) {
    QPixmap scaled = source.scaled(targetSize, targetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

    QPixmap result(targetSize, targetSize);
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, targetSize, targetSize), radius, radius);
    painter.setClipPath(path);
    const int x = (targetSize - scaled.width()) / 2;
    const int y = (targetSize - scaled.height()) / 2;
    painter.drawPixmap(x, y, scaled);
    return result;
}

QGraphicsOpacityEffect *opacityEffectOf(QWidget *widget) {
    return qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect());
}
}

QueueWindow::QueueWindow(QWidget *parent) : QWidget(parent), network(new QNetworkAccessManager(this)) {
    // Same overlay behavior as the OSD, minus WindowTransparentForInput:
    // this window has to receive mouse events so it can be dragged and resized.
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setMinimumWidth(kMinWidth);
    setMaximumWidth(kMaxWidth);
#if defined(Q_OS_LINUX)
    setAttribute(Qt::WA_X11DoNotAcceptFocus, true);
#endif
#ifdef __APPLE__
    applyMacOverlayWindowBehavior(this);
#endif

    // No layouts above row level: rows are placed by hand so slide/fade
    // transitions can drive their positions directly.
    containerWidget = new QWidget(this);
    containerWidget->setMouseTracking(true);
    containerWidget->installEventFilter(this);

    nowArtLabel = new QLabel(containerWidget);
    nowArtLabel->setFixedSize(kNowArtSize, kNowArtSize);
    nowArtLabel->setAlignment(Qt::AlignCenter);
    nowArtLabel->setText("🎵");
    nowTitleLabel = new QLabel("Nothing playing", containerWidget);
    nowArtistLabel = new QLabel("Spotify", containerWidget);
    nowTimeLabel = new QLabel(containerWidget);
    nowTimeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    nowProgressBar = new QProgressBar(containerWidget);
    nowProgressBar->setTextVisible(false);
    nowProgressBar->setRange(0, 1);
    nowProgressBar->setValue(0);

    nowTickTimer = new QTimer(this);
    nowTickTimer->setInterval(250);
    connect(nowTickTimer, &QTimer::timeout, this, &QueueWindow::updateNowPlayingTime);

    lockIconLabel = new QLabel(containerWidget);
    lockIconLabel->setFixedSize(kLockIconSize, kLockIconSize);
    lockIconLabel->setStyleSheet("border: none; background: transparent;");
    lockIconLabel->hide();

    headerLabel = new QLabel("UP NEXT", containerWidget);
    emptyLabel = new QLabel("Queue is empty", containerWidget);

    geometryAnimation = new QPropertyAnimation(this, "geometry", this);
    geometryAnimation->setDuration(kHeightAnimMs);
    geometryAnimation->setEasingCurve(QEasingCurve::OutCubic);

    fadeAnimation = new QPropertyAnimation(this, "windowOpacity", this);
    fadeAnimation->setDuration(kFadeMs);
    fadeAnimation->setEasingCurve(QEasingCurve::InOutQuad);

    overlaySettings = AppSettings::loadOverlaySettings();
    queueSettings = AppSettings::loadQueueSettings();
    setWindowFlag(Qt::WindowTransparentForInput, queueSettings.locked);
    applyOverlaySettings(overlaySettings);
    resize(effectiveWidth(), contentHeightFor(0));
    applyStoredOrDefaultPosition();
    updateVisibility();
}

void QueueWindow::setQueue(const QList<UpcomingTrack> &tracks) {
    queue = tracks;
    refreshRows(/*animate=*/isVisible());
}

void QueueWindow::setNowPlaying(const QString &title, const QString &artist, const QString &artUrl,
                                int progressMs, int durationMs, bool isPlaying) {
    nowTitle = title.isEmpty() ? QStringLiteral("Loading...") : title;
    nowArtist = artist.isEmpty() ? QStringLiteral("Spotify") : artist;
    nowDurationMs = durationMs;
    syncNowPlayingProgress(progressMs, isPlaying);

    if (!artUrl.isEmpty() && artUrl != nowArtUrl) {
        nowArtUrl = artUrl;
        setArtOnLabel(nowArtLabel, nowArtUrl, kNowArtSize);
    }

    nowProgressBar->setRange(0, qMax(1, nowDurationMs));
    updateElides();
    updateNowPlayingTime();
}

void QueueWindow::syncNowPlayingProgress(int progressMs, bool isPlaying) {
    nowProgressMs = progressMs;
    nowIsPlaying = isPlaying;
    if (nowIsPlaying) {
        nowClock.restart();
        if (!nowTickTimer->isActive()) {
            nowTickTimer->start();
        }
    } else {
        nowTickTimer->stop();
    }
    updateNowPlayingTime();
}

int QueueWindow::estimatedNowProgress() const {
    int estimated = nowProgressMs;
    if (nowIsPlaying && nowClock.isValid()) {
        estimated += int(nowClock.elapsed());
    }
    if (nowDurationMs > 0) {
        estimated = qBound(0, estimated, nowDurationMs);
    }
    return estimated;
}

void QueueWindow::updateNowPlayingTime() {
    const int progress = estimatedNowProgress();
    nowProgressBar->setValue(nowDurationMs > 0 ? progress : 0);
    nowTimeLabel->setText(nowDurationMs > 0
        ? formatDuration(progress) + " / " + formatDuration(nowDurationMs)
        : QString());
}

void QueueWindow::applyOverlaySettings(const OverlaySettings &settings) {
    overlaySettings = settings;
    containerWidget->setStyleSheet(QString("background-color: %1; border-radius: 12px; border: 1px solid %2;")
        .arg(overlaySettings.backgroundColor, overlaySettings.borderColor));
    headerLabel->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold; letter-spacing: 1px; border: none; background: transparent;")
        .arg(overlaySettings.accentColor));
    emptyLabel->setStyleSheet(QString("color: %1; font-size: 12px; border: none; background: transparent;")
        .arg(overlaySettings.secondaryTextColor));
    nowTitleLabel->setStyleSheet(QString("color: %1; font-weight: bold; font-size: 14px; border: none; background: transparent;")
        .arg(overlaySettings.primaryTextColor));
    nowArtistLabel->setStyleSheet(QString("color: %1; font-size: 12px; border: none; background: transparent;")
        .arg(overlaySettings.secondaryTextColor));
    nowTimeLabel->setStyleSheet(QString("color: %1; font-size: 10px; font-family: monospace; border: none; background: transparent;")
        .arg(overlaySettings.mutedTextColor));
    nowProgressBar->setStyleSheet(QString(
        "QProgressBar { background-color: %1; border: none; border-radius: 2px; }"
        "QProgressBar::chunk { background-color: %2; border-radius: 2px; }")
        .arg(overlaySettings.borderColor, overlaySettings.progressBarColor));
    if (nowArtLabel->pixmap(Qt::ReturnByValue).isNull()) {
        nowArtLabel->setStyleSheet(QString("border: none; border-radius: 6px; background-color: #2c2c2c; color: %1; font-size: 22px;")
            .arg(overlaySettings.accentColor));
    }
    lockIconLabel->setPixmap(lockPixmap(kLockIconSize, QColor(overlaySettings.accentColor)));
    for (const Row &row : rows) {
        styleRowLabels(row);
    }
    refreshRows(/*animate=*/false);
}

void QueueWindow::applyQueueSettings(const QueueSettings &settings) {
    const int oldX = queueSettings.windowX;
    const int oldY = queueSettings.windowY;
    const bool wasLocked = queueSettings.locked;
    queueSettings = settings;
    // Keep the live drag position authoritative; settings only override it
    // when they carry a different stored spot (e.g. first apply after load).
    if (queueSettings.windowX != oldX || queueSettings.windowY != oldY) {
        applyStoredOrDefaultPosition();
    }

    if (queueSettings.locked != wasLocked) {
        dragging = false;
        resizingEdge = EdgeHit::None;
        setCursor(Qt::ArrowCursor);
        // Changing a window flag hides the window; restore it without a fade.
        const bool wasVisible = isVisible();
        setWindowFlag(Qt::WindowTransparentForInput, queueSettings.locked);
        if (wasVisible && queueSettings.enabled) {
            setWindowOpacity(queueSettings.opacityPercent / 100.0);
            show();
        }
    }

    refreshRows(/*animate=*/isVisible());
    updateVisibility();
}

int QueueWindow::effectiveWidth() const {
    if (queueSettings.windowWidth > 0) {
        return qBound(kMinWidth, queueSettings.windowWidth, kMaxWidth);
    }
    return qBound(kMinWidth, overlaySettings.overlayWidth, kMaxWidth);
}

int QueueWindow::headerHeight() const {
    return headerLabel->sizeHint().height();
}

int QueueWindow::nowBlockHeight() const {
    return queueSettings.showNowPlaying ? kNowHeight + kNowGap : 0;
}

bool QueueWindow::lockIconVisible() const {
    return queueSettings.locked && queueSettings.showLockIcon;
}

int QueueWindow::rowY(int index) const {
    return kMarginTop + nowBlockHeight() + headerHeight() + kHeaderGap + index * (kRowHeight + kRowGap);
}

int QueueWindow::rowWidth() const {
    return qMax(60, width() - 2 * kMarginX);
}

int QueueWindow::contentHeightFor(int rowCount) const {
    const int top = kMarginTop + nowBlockHeight() + headerHeight() + kHeaderGap;
    if (rowCount <= 0) {
        return top + emptyLabel->sizeHint().height() + kMarginBottom;
    }
    return top + rowCount * kRowHeight + (rowCount - 1) * kRowGap + kMarginBottom;
}

void QueueWindow::refreshRows(bool animate) {
    const int shown = qMin(int(queue.size()), queueSettings.maxSongs);

    // Duplicate-aware keys: the same track queued twice gets #0, #1... so a
    // shift of the list still matches each on-screen row to one queue entry.
    QHash<QString, int> ordinal;
    QStringList targetKeys;
    for (int i = 0; i < shown; ++i) {
        const QString &id = queue.at(i).trackId;
        targetKeys.append(id + '#' + QString::number(ordinal[id]++));
    }

    QHash<QString, int> oldIndexByKey;
    for (int i = 0; i < rows.size(); ++i) {
        oldIndexByKey.insert(rows.at(i).key, i);
    }

    // Which way is the list flowing? Skipping forward shifts survivors up
    // (direction -1); going back to the previous song shifts them down (+1).
    // Entering and exiting rows follow that same direction so the whole list
    // reads as one motion instead of rows crossing through each other.
    int shiftSum = 0;
    for (int i = 0; i < shown; ++i) {
        const auto oldIndex = oldIndexByKey.constFind(targetKeys.at(i));
        if (oldIndex != oldIndexByKey.constEnd()) {
            shiftSum += i - oldIndex.value();
        }
    }
    const int direction = shiftSum > 0 ? 1 : -1;

    QList<Row> newRows;
    QSet<int> reused;
    for (int i = 0; i < shown; ++i) {
        const UpcomingTrack &track = queue.at(i);
        const auto oldIndex = oldIndexByKey.constFind(targetKeys.at(i));
        if (oldIndex != oldIndexByKey.constEnd()) {
            Row row = rows.at(oldIndex.value());
            reused.insert(oldIndex.value());
            updateRowContent(row, track, i);
            moveRowTo(row, i, animate);
            newRows.append(row);
        } else {
            Row row = makeRow(track);
            row.key = targetKeys.at(i);
            updateRowContent(row, track, i);
            if (animate) {
                fadeInRowAt(row, i, direction);
            } else {
                row.widget->move(kMarginX, rowY(i));
                row.widget->show();
            }
            newRows.append(row);
        }
    }

    for (int i = 0; i < rows.size(); ++i) {
        if (!reused.contains(i)) {
            discardRow(rows.at(i), animate, direction);
        }
    }
    rows = newRows;

    emptyLabel->setVisible(shown == 0);
    updateElides();
    relayoutStatics();
    animateToContentHeight();
}

QueueWindow::Row QueueWindow::makeRow(const UpcomingTrack &track) {
    Row row;
    row.widget = new QWidget(containerWidget);
    row.widget->setFixedSize(rowWidth(), kRowHeight);
    row.widget->setStyleSheet("background: transparent; border: none;");
    // Hover moves stop at the row widget (they don't bubble up like clicks
    // do), so the click affordance has to live on the row itself.
    row.widget->setCursor(Qt::PointingHandCursor);
    row.widget->installEventFilter(this);

    // Sits behind the labels (created first = lowest); fades in on hover.
    row.hoverBg = new QWidget(row.widget);
    row.hoverBg->setGeometry(row.widget->rect());
    QGraphicsOpacityEffect *hoverEffect = new QGraphicsOpacityEffect(row.hoverBg);
    hoverEffect->setOpacity(0.0);
    row.hoverBg->setGraphicsEffect(hoverEffect);
    row.hoverBg->lower();

    QHBoxLayout *rowLayout = new QHBoxLayout(row.widget);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);

    row.indexLabel = new QLabel(row.widget);
    row.indexLabel->setFixedWidth(22);
    row.indexLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    row.artLabel = new QLabel(row.widget);
    row.artLabel->setFixedSize(kArtSize, kArtSize);
    row.artLabel->setAlignment(Qt::AlignCenter);
    row.artLabel->setText("♪");
    row.artUrl = track.artUrl;


    QVBoxLayout *textLayout = new QVBoxLayout();
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(0);
    row.titleLabel = new QLabel(row.widget);
    row.artistLabel = new QLabel(row.widget);
    textLayout->addWidget(row.titleLabel);
    textLayout->addWidget(row.artistLabel);

    row.durationLabel = new QLabel(row.widget);
    row.durationLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);

    rowLayout->addWidget(row.indexLabel, 0);
    rowLayout->addWidget(row.artLabel, 0);
    rowLayout->addLayout(textLayout, 1);
    rowLayout->addWidget(row.durationLabel, 0);

    QGraphicsOpacityEffect *effect = new QGraphicsOpacityEffect(row.widget);
    effect->setOpacity(1.0);
    row.widget->setGraphicsEffect(effect);

    styleRowLabels(row);
    if (!row.artUrl.isEmpty()) {
        setArtOnLabel(row.artLabel, row.artUrl, kArtSize);
    }
    return row;
}

void QueueWindow::styleRowLabels(const Row &row) const {
    const QColor accent(overlaySettings.accentColor);
    row.hoverBg->setStyleSheet(QString("background-color: rgba(%1,%2,%3,14%); border: none; border-radius: 6px;")
        .arg(accent.red()).arg(accent.green()).arg(accent.blue()));
    row.indexLabel->setStyleSheet(QString("color: %1; font-size: 12px; font-family: monospace; border: none; background: transparent;").arg(overlaySettings.mutedTextColor));
    row.titleLabel->setStyleSheet(QString("color: %1; font-weight: bold; font-size: 13px; border: none; background: transparent;").arg(overlaySettings.primaryTextColor));
    row.artistLabel->setStyleSheet(QString("color: %1; font-size: 12px; border: none; background: transparent;").arg(overlaySettings.secondaryTextColor));
    row.durationLabel->setStyleSheet(QString("color: %1; font-size: 11px; font-family: monospace; border: none; background: transparent;").arg(overlaySettings.mutedTextColor));
    if (row.artLabel->pixmap(Qt::ReturnByValue).isNull()) {
        row.artLabel->setStyleSheet(QString("border: none; border-radius: 5px; background-color: #2c2c2c; color: %1; font-size: 16px;").arg(overlaySettings.accentColor));
    }
}

void QueueWindow::updateRowContent(Row &row, const UpcomingTrack &track, int index) {
    row.fullTitle = track.title.isEmpty() ? QStringLiteral("Loading…") : track.title;
    row.fullArtist = track.artist.isEmpty() ? QStringLiteral("…") : track.artist;
    row.indexLabel->setText(QString::number(index + 1) + ".");
    row.durationLabel->setText(track.durationMs > 0 ? formatDuration(track.durationMs) : QString());
    if (row.artUrl != track.artUrl && !track.artUrl.isEmpty()) {
        row.artUrl = track.artUrl;
        setArtOnLabel(row.artLabel, row.artUrl, kArtSize);
    }
}

void QueueWindow::animateRowHover(Row &row, bool hovered) {
    QGraphicsOpacityEffect *effect = opacityEffectOf(row.hoverBg);
    if (!effect) {
        return;
    }
    if (row.hoverAnimation) {
        row.hoverAnimation->stop();
    }

    QPropertyAnimation *fade = new QPropertyAnimation(effect, "opacity", row.hoverBg);
    fade->setDuration(hovered ? 150 : 220);
    fade->setEasingCurve(QEasingCurve::OutQuad);
    fade->setStartValue(effect->opacity());
    fade->setEndValue(hovered ? 1.0 : 0.0);
    row.hoverAnimation = fade;
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void QueueWindow::moveRowTo(Row &row, int index, bool animate) {
    const QPoint target(kMarginX, rowY(index));
    if (row.posAnimation) {
        row.posAnimation->stop();
    }
    if (!animate || row.widget->pos() == target) {
        row.widget->move(target);
        return;
    }

    QPropertyAnimation *slide = new QPropertyAnimation(row.widget, "pos", row.widget);
    slide->setDuration(kSlideMs);
    slide->setEasingCurve(QEasingCurve::OutCubic);
    slide->setStartValue(row.widget->pos());
    slide->setEndValue(target);
    row.posAnimation = slide;
    slide->start(QAbstractAnimation::DeleteWhenStopped);
}

void QueueWindow::fadeInRowAt(Row &row, int index, int direction) {
    const QPoint target(kMarginX, rowY(index));
    // Enter from the side the list is flowing away from: from above when the
    // survivors slide down (prev song), from below when they slide up.
    row.widget->move(target - QPoint(0, direction * kRowEnterOffsetPx));
    row.widget->show();

    if (QGraphicsOpacityEffect *effect = opacityEffectOf(row.widget)) {
        effect->setOpacity(0.0);
        QPropertyAnimation *fade = new QPropertyAnimation(effect, "opacity", row.widget);
        fade->setDuration(kRowFadeInMs);
        fade->setEasingCurve(QEasingCurve::OutQuad);
        fade->setStartValue(0.0);
        fade->setEndValue(1.0);
        fade->start(QAbstractAnimation::DeleteWhenStopped);
    }

    QPropertyAnimation *slide = new QPropertyAnimation(row.widget, "pos", row.widget);
    slide->setDuration(kSlideMs);
    slide->setEasingCurve(QEasingCurve::OutCubic);
    slide->setStartValue(row.widget->pos());
    slide->setEndValue(target);
    row.posAnimation = slide;
    slide->start(QAbstractAnimation::DeleteWhenStopped);
}

void QueueWindow::discardRow(const Row &row, bool animate, int direction) {
    QWidget *widget = row.widget;
    if (row.posAnimation) {
        row.posAnimation->stop();
    }
    if (!animate) {
        widget->deleteLater();
        return;
    }

    // Removed rows keep drifting the way the list is moving and dissolve.
    if (QGraphicsOpacityEffect *effect = opacityEffectOf(widget)) {
        QPropertyAnimation *fade = new QPropertyAnimation(effect, "opacity", widget);
        fade->setDuration(kRowFadeOutMs);
        fade->setEasingCurve(QEasingCurve::InQuad);
        fade->setStartValue(effect->opacity());
        fade->setEndValue(0.0);
        QObject::connect(fade, &QPropertyAnimation::finished, widget, &QWidget::deleteLater);
        fade->start(QAbstractAnimation::DeleteWhenStopped);
    } else {
        widget->deleteLater();
        return;
    }

    QPropertyAnimation *slide = new QPropertyAnimation(widget, "pos", widget);
    slide->setDuration(kRowFadeOutMs);
    slide->setEasingCurve(QEasingCurve::InQuad);
    slide->setStartValue(widget->pos());
    slide->setEndValue(widget->pos() + QPoint(0, direction * kRowEnterOffsetPx));
    slide->start(QAbstractAnimation::DeleteWhenStopped);
}

void QueueWindow::relayoutStatics() {
    containerWidget->setGeometry(rect());

    // Now-playing block: art on the left, title/artist/progress to the right.
    const bool showNow = queueSettings.showNowPlaying;
    nowArtLabel->setVisible(showNow);
    nowTitleLabel->setVisible(showNow);
    nowArtistLabel->setVisible(showNow);
    nowProgressBar->setVisible(showNow);
    nowTimeLabel->setVisible(showNow);
    // The lock badge occupies the window's top-right corner. Whatever shares
    // that line (the now-playing title, or the header when the now block is
    // hidden) gets its width trimmed so it can't run under the badge — this
    // matters for RTL text (e.g. Arabic titles), which renders right-aligned.
    lockIconLabel->setVisible(lockIconVisible());
    const int lockReserve = lockIconVisible() ? kLockIconSize + 6 : 0;
    lockIconLabel->move(width() - kMarginX - kLockIconSize, kMarginTop + 2);
    lockIconLabel->raise();

    if (showNow) {
        const int textX = kMarginX + kNowArtSize + 10;
        const int textW = qMax(60, width() - textX - kMarginX);
        nowArtLabel->move(kMarginX, kMarginTop + (kNowHeight - kNowArtSize) / 2);
        nowTitleLabel->setGeometry(textX, kMarginTop + 4, qMax(60, textW - lockReserve), 18);
        nowArtistLabel->setGeometry(textX, kMarginTop + 22, textW, 15);
        nowProgressBar->setGeometry(textX, kMarginTop + 42, textW, 4);
        nowTimeLabel->setGeometry(textX, kMarginTop + 47, textW, 12);
    }

    headerLabel->setGeometry(kMarginX, kMarginTop + nowBlockHeight(), rowWidth() - (showNow ? 0 : lockReserve), headerHeight());
    emptyLabel->setGeometry(kMarginX, rowY(0), rowWidth(), emptyLabel->sizeHint().height());
    for (Row &row : rows) {
        row.widget->setFixedSize(rowWidth(), kRowHeight);
        row.hoverBg->setGeometry(row.widget->rect());
    }
}

void QueueWindow::animateToContentHeight() {
    const int targetWidth = (resizingEdge != EdgeHit::None) ? width() : effectiveWidth();
    const int targetHeight = contentHeightFor(rows.size());

    const QRect target(pos(), QSize(targetWidth, targetHeight));
    if (!isVisible() || resizingEdge != EdgeHit::None) {
        geometryAnimation->stop();
        setGeometry(target);
        return;
    }
    if (target == geometry()) {
        return;
    }

    geometryAnimation->stop();
    geometryAnimation->setStartValue(geometry());
    geometryAnimation->setEndValue(target);
    geometryAnimation->start();
}

void QueueWindow::updateElides() {
    // Text budget: row width minus index column, album art, duration column
    // and layout spacing. QLabel doesn't elide on its own.
    const int textBudget = qMax(60, rowWidth() - 22 - kArtSize - 44 - 24);

    for (Row &row : rows) {
        row.titleLabel->setText(QFontMetrics(row.titleLabel->font()).elidedText(row.fullTitle, Qt::ElideRight, textBudget));
        row.artistLabel->setText(QFontMetrics(row.artistLabel->font()).elidedText(row.fullArtist, Qt::ElideRight, textBudget));
    }

    const int nowBudget = qMax(60, width() - (kMarginX + kNowArtSize + 10) - kMarginX);
    const int titleBudget = qMax(60, nowBudget - (lockIconVisible() ? kLockIconSize + 6 : 0));
    nowTitleLabel->setText(QFontMetrics(nowTitleLabel->font()).elidedText(nowTitle.isEmpty() ? QStringLiteral("Nothing playing") : nowTitle, Qt::ElideRight, titleBudget));
    nowArtistLabel->setText(QFontMetrics(nowArtistLabel->font()).elidedText(nowArtist.isEmpty() ? QStringLiteral("Spotify") : nowArtist, Qt::ElideRight, nowBudget));
}

void QueueWindow::setArtOnLabel(QLabel *artLabel, const QString &artUrl, int size) {
    if (artUrl.isEmpty()) {
        return;
    }

    const QString cacheKey = artUrl + '@' + QString::number(size);
    const auto cached = artCache.constFind(cacheKey);
    if (cached != artCache.constEnd()) {
        artLabel->setText("");
        artLabel->setStyleSheet("border: none; border-radius: 5px; background: transparent;");
        artLabel->setPixmap(*cached);
        return;
    }

    QNetworkReply *reply = network->get(QNetworkRequest(QUrl(artUrl)));
    QPointer<QLabel> target(artLabel);
    connect(reply, &QNetworkReply::finished, this, [this, reply, target, cacheKey, size]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            return;
        }
        QPixmap pixmap;
        if (!pixmap.loadFromData(reply->readAll())) {
            return;
        }

        if (artCache.size() > 150) {
            artCache.clear();
        }
        const QPixmap rounded = roundedThumbnail(pixmap, size, 5.0);
        artCache.insert(cacheKey, rounded);

        // The row may have been discarded while the download was in flight.
        if (target) {
            target->setText("");
            target->setStyleSheet("border: none; border-radius: 5px; background: transparent;");
            target->setPixmap(rounded);
        }
    });
}

void QueueWindow::updateVisibility() {
    if (queueSettings.enabled) {
        if (!isVisible()) {
            setWindowOpacity(0.0);
            show();
        }
        fadeTo(queueSettings.opacityPercent / 100.0, /*hideWhenDone=*/false);
    } else if (isVisible()) {
        fadeTo(0.0, /*hideWhenDone=*/true);
    }
}

void QueueWindow::fadeTo(qreal targetOpacity, bool hideWhenDone) {
    fadeAnimation->stop();
    fadeAnimation->disconnect(this);
    fadeAnimation->setStartValue(windowOpacity());
    fadeAnimation->setEndValue(targetOpacity);
    if (hideWhenDone) {
        connect(fadeAnimation, &QPropertyAnimation::finished, this, [this]() { hide(); });
    }
    fadeAnimation->start();
}

void QueueWindow::applyStoredOrDefaultPosition() {
    if (queueSettings.windowX != INT_MIN && queueSettings.windowY != INT_MIN) {
        // Only restore a spot that is still on some screen (monitor layouts change).
        const QPoint stored(queueSettings.windowX, queueSettings.windowY);
        if (QGuiApplication::screenAt(stored + QPoint(20, 20))) {
            move(stored);
            return;
        }
    }

    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) {
        return;
    }
    const QRect geo = screen->availableGeometry();
    move(geo.x() + geo.width() - width() - 24, geo.y() + (geo.height() - height()) / 3);
}

QueueWindow::EdgeHit QueueWindow::edgeHitTest(const QPoint &windowPos) const {
    if (windowPos.x() <= kResizeGripPx) {
        return EdgeHit::Left;
    }
    if (windowPos.x() >= width() - kResizeGripPx) {
        return EdgeHit::Right;
    }
    return EdgeHit::None;
}

int QueueWindow::rowIndexAt(const QPoint &windowPos) const {
    // containerWidget fills the window, so window coords match row coords.
    for (int i = 0; i < rows.size(); ++i) {
        if (rows.at(i).widget->geometry().contains(windowPos)) {
            return i;
        }
    }
    return -1;
}

void QueueWindow::updateHoverCursor(const QPoint &windowPos) {
    if (resizingEdge != EdgeHit::None || dragging) {
        return;
    }
    if (edgeHitTest(windowPos) != EdgeHit::None) {
        setCursor(Qt::SizeHorCursor);
    } else if (rowIndexAt(windowPos) >= 0) {
        setCursor(Qt::PointingHandCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }
}

bool QueueWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == containerWidget && event->type() == QEvent::MouseMove) {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
        updateHoverCursor(containerWidget->mapTo(this, mouseEvent->position().toPoint()));
    }

    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        for (Row &row : rows) {
            if (row.widget == watched) {
                animateRowHover(row, event->type() == QEvent::Enter);
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QueueWindow::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        const EdgeHit hit = edgeHitTest(event->position().toPoint());
        if (hit != EdgeHit::None) {
            resizingEdge = hit;
            resizeStartGeometry = geometry();
            resizeStartGlobal = event->globalPosition().toPoint();
            geometryAnimation->stop();
        } else {
            dragging = true;
            dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        }
        pressGlobalPos = event->globalPosition().toPoint();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void QueueWindow::mouseMoveEvent(QMouseEvent *event) {
    if (resizingEdge != EdgeHit::None && (event->buttons() & Qt::LeftButton)) {
        const int deltaX = event->globalPosition().toPoint().x() - resizeStartGlobal.x();
        QRect geo = resizeStartGeometry;
        if (resizingEdge == EdgeHit::Right) {
            geo.setWidth(qBound(kMinWidth, resizeStartGeometry.width() + deltaX, kMaxWidth));
        } else {
            const int newWidth = qBound(kMinWidth, resizeStartGeometry.width() - deltaX, kMaxWidth);
            geo.setX(resizeStartGeometry.right() - newWidth + 1);
            geo.setWidth(newWidth);
        }
        setGeometry(geo);
        event->accept();
        return;
    }

    if (dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - dragOffset);
        event->accept();
        return;
    }

    updateHoverCursor(event->position().toPoint());
    QWidget::mouseMoveEvent(event);
}

void QueueWindow::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && (dragging || resizingEdge != EdgeHit::None)) {
        const bool wasResizing = resizingEdge != EdgeHit::None;
        dragging = false;
        resizingEdge = EdgeHit::None;

        if (wasResizing) {
            queueSettings.windowWidth = width();
            emit windowResized(width());
        }
        emit windowMoved(pos());

        // A press that never travelled is a click, not a drag: activate the
        // row under the cursor. (Locked mode never gets here — the window is
        // transparent for input entirely.)
        const bool wasClick = !wasResizing &&
            (event->globalPosition().toPoint() - pressGlobalPos).manhattanLength() < 5;
        if (wasClick) {
            const int index = rowIndexAt(event->position().toPoint());
            if (index >= 0 && index < queue.size()) {
                emit trackActivated(queue.at(index).trackId, queue.at(index).uid);
            }
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void QueueWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    relayoutStatics();
    updateElides();
}

QString QueueWindow::formatDuration(int ms) {
    const int totalSeconds = ms / 1000;
    return QString("%1:%2").arg(totalSeconds / 60).arg(totalSeconds % 60, 2, 10, QChar('0'));
}
