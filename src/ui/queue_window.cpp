#include "queue_window.h"
#include "theme.h"

#include <QEasingCurve>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __APPLE__
void applyMacOverlayWindowBehavior(QWidget *widget);
#endif

namespace {
using theme::animMs;

constexpr int kArtSize = 44;       // queue-row album art
constexpr int kNowArtSize = 56;    // now-playing album art
constexpr int kNowHeight = 56;
constexpr int kNowGap = 11;         // now art -> text (space-4)
constexpr int kRowHeight = 52;
constexpr int kRowGap = 3;          // between rows (space-1)
constexpr int kRowContentGap = 8;   // inside a row (space-3)
constexpr int kMargin = 17;         // window padding all round (space-6)
constexpr int kHeaderGap = 8;       // header -> first row (space-3)
constexpr int kDividerMarginTop = 17;
constexpr int kDividerMarginBottom = 11;
constexpr int kResizeGripPx = 8;
constexpr int kMinWidth = 240;
constexpr int kMaxWidth = 900;
constexpr int kFadeMs = 220;
constexpr int kHeightAnimMs = 260;
constexpr int kSlideMs = 280;
constexpr int kRowFadeInMs = 240;
constexpr int kRowFadeOutMs = 180;
constexpr int kRowEnterOffsetPx = 14;
constexpr int kHeaderIconSize = 11;  // padlock glyph in the header hint
constexpr int kRowBadgeSize = 13;    // heart / sparkle on each row
constexpr int kIndexSlot = 16;
constexpr int kHoverMaxAlpha = 15;   // out of 255, ~6% (the divider hover tint)

// Painted directly instead of via stylesheets: animating setStyleSheet forces
// a repolish every frame, which caused visible glitches.
class HoverHighlight : public QWidget {
public:
    explicit HoverHighlight(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    void setColor(const QColor &c) { color = c; update(); }
    qreal alphaFraction() const { return alpha; }
    void setAlphaFraction(qreal a) {
        alpha = qBound<qreal>(0.0, a, 1.0);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (alpha <= 0.0) {
            return;
        }
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QColor c = color;
        c.setAlpha(int(alpha * kHoverMaxAlpha));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(rect(), theme::kRadiusMd, theme::kRadiusMd);
    }

private:
    QColor color;
    qreal alpha = 0.0;
};

// A 1px rule that fades to transparent over its first and last 48px — a
// Nocturne signature that replaces a bare gap between the now block and queue.
class FadingRule : public QWidget {
public:
    explicit FadingRule(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedHeight(1);
    }
    void setColor(const QColor &c) { color = c; update(); }

protected:
    void paintEvent(QPaintEvent *) override {
        const qreal w = width();
        if (w <= 0) {
            return;
        }
        QPainter p(this);
        QLinearGradient g(0, 0, w, 0);
        QColor edge = color;
        edge.setAlpha(0);
        const qreal f = qMin(0.5, 48.0 / w);
        g.setColorAt(0.0, edge);
        g.setColorAt(f, color);
        g.setColorAt(1.0 - f, color);
        g.setColorAt(1.0, edge);
        p.fillRect(rect(), g);
    }

private:
    QColor color{233, 233, 237, 41}; // text @16%
};

QPixmap lockPixmap(int size, const QColor &color) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, qMax(1.5, size * 0.13));
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(size * 0.26, size * 0.10, size * 0.48, size * 0.52), 0, 180 * 16);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(size * 0.15, size * 0.44, size * 0.70, size * 0.48), size * 0.14, size * 0.14);
    return pm;
}

// A filled play triangle (9x10), drawn into a square so it centres in the slot.
QPixmap playTrianglePixmap(int box, const QColor &color) {
    QPixmap pm(box, box);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    const qreal ox = (box - 9) / 2.0, oy = (box - 10) / 2.0;
    QPainterPath tri;
    tri.moveTo(ox, oy);
    tri.lineTo(ox + 9, oy + 5);
    tri.lineTo(ox, oy + 10);
    tri.closeSubpath();
    p.drawPath(tri);
    return pm;
}

void drawSparkle(QPainter &p, const QPointF &center, qreal radius) {
    const qreal waist = radius * 0.28;
    QPainterPath path;
    path.moveTo(center.x(), center.y() - radius);
    path.quadTo(center.x() + waist, center.y() - waist, center.x() + radius, center.y());
    path.quadTo(center.x() + waist, center.y() + waist, center.x(), center.y() + radius);
    path.quadTo(center.x() - waist, center.y() + waist, center.x() - radius, center.y());
    path.quadTo(center.x() - waist, center.y() - waist, center.x(), center.y() - radius);
    path.closeSubpath();
    p.drawPath(path);
}

QPixmap sparklePixmap(int size, const QColor &color) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    drawSparkle(p, QPointF(size * 0.40, size * 0.42), size * 0.34);
    drawSparkle(p, QPointF(size * 0.78, size * 0.76), size * 0.20);
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

    // The now-playing block lives in its own widget so a track change can
    // slide the whole thing as one unit, mirroring the queue-row motion.
    nowWidget = new QWidget(containerWidget);
    nowWidget->setStyleSheet("background: transparent; border: none;");
    QGraphicsOpacityEffect *nowEffect = new QGraphicsOpacityEffect(nowWidget);
    nowEffect->setOpacity(1.0);
    nowWidget->setGraphicsEffect(nowEffect);

    nowArtLabel = new QLabel(nowWidget);
    nowArtLabel->setFixedSize(kNowArtSize, kNowArtSize);
    nowArtLabel->setAlignment(Qt::AlignCenter);
    nowArtLabel->setText(QStringLiteral("♪"));
    nowTitleLabel = new QLabel("Nothing playing", nowWidget);
    nowArtistLabel = new QLabel("Spotify", nowWidget);
    nowTimeLabel = new QLabel(nowWidget);
    nowTimeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    nowProgressBar = new QProgressBar(nowWidget);
    nowProgressBar->setTextVisible(false);
    nowProgressBar->setFixedHeight(3);
    nowProgressBar->setRange(0, 1);
    nowProgressBar->setValue(0);

    // "Locked" chip on the now-playing title line (replaces the corner padlock).
    lockedChip = new QWidget(nowWidget);
    lockedChip->setAttribute(Qt::WA_TransparentForMouseEvents);
    QHBoxLayout *chipLayout = new QHBoxLayout(lockedChip);
    chipLayout->setContentsMargins(theme::kSpace3, 3, theme::kSpace3, 3);
    chipLayout->setSpacing(5);
    lockedChipIcon = new QLabel(lockedChip);
    lockedChipIcon->setFixedSize(10, 10);
    lockedChipText = new QLabel(QStringLiteral("Locked"), lockedChip);
    chipLayout->addWidget(lockedChipIcon);
    chipLayout->addWidget(lockedChipText);
    lockedChip->hide();

    nowTickTimer = new QTimer(this);
    nowTickTimer->setInterval(250);
    connect(nowTickTimer, &QTimer::timeout, this, &QueueWindow::updateNowPlayingTime);

    dividerWidget = new FadingRule(containerWidget);

    headerLabel = new QLabel("UP NEXT", containerWidget);
    headerCountLabel = new QLabel(containerWidget);
    lockHintIconLabel = new QLabel(containerWidget);
    lockHintIconLabel->setFixedSize(kHeaderIconSize, kHeaderIconSize);
    lockHintTextLabel = new QLabel(QStringLiteral("Alt+L to lock"), containerWidget);
    emptyLabel = new QLabel(QStringLiteral("Nothing queued. Songs you add in Spotify — or picked by Smart Shuffle — show up here."), containerWidget);
    emptyLabel->setWordWrap(true);

    geometryAnimation = new QPropertyAnimation(this, "geometry", this);
    geometryAnimation->setDuration(animMs(kHeightAnimMs));
    geometryAnimation->setEasingCurve(QEasingCurve::OutCubic);

    fadeAnimation = new QPropertyAnimation(this, "windowOpacity", this);
    fadeAnimation->setDuration(animMs(kFadeMs));
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

void QueueWindow::setNowPlaying(const QString &trackId, const QString &title, const QString &artist,
                                const QString &artUrl, int progressMs, int durationMs, bool isPlaying) {
    // Only a real track change animates; metadata re-emits for the same
    // track just refresh the text in place.
    const bool isNewTrack = !trackId.isEmpty() && trackId != nowTrackId;
    const bool shouldAnimate = isNewTrack && !nowTrackId.isEmpty() &&
                               isVisible() && queueSettings.showNowPlaying;
    QPixmap oldSnapshot;
    bool fromList = false;
    if (shouldAnimate) {
        oldSnapshot = nowWidget->grab();
        for (const UpcomingTrack &t : queue) {
            if (t.trackId == trackId) {
                fromList = true;
                break;
            }
        }
    }
    if (!trackId.isEmpty()) {
        nowTrackId = trackId;
    }

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

    if (shouldAnimate) {
        animateNowSwap(fromList, oldSnapshot);
    }
}

void QueueWindow::layoutNowContents() {
    const int textX = kNowArtSize + kNowGap;
    const int textW = qMax(60, nowWidget->width() - textX);
    nowArtLabel->move(0, (kNowHeight - kNowArtSize) / 2);

    // Reserve room on the title line for the "Locked" chip when it shows.
    int chipReserve = 0;
    if (lockedChipVisible()) {
        lockedChip->adjustSize();
        chipReserve = lockedChip->width() + theme::kSpace2;
        lockedChip->move(textX + textW - lockedChip->width(), 2);
        lockedChip->raise();
    }
    nowTitleLabel->setGeometry(textX, 2, qMax(40, textW - chipReserve), 20);
    nowArtistLabel->setGeometry(textX, 24, textW, 16);

    // Progress rail flexes; the time sits to its right.
    const int timeW = qMin(textW / 2, QFontMetrics(nowTimeLabel->font()).horizontalAdvance("0:00 / 0:00") + 2);
    nowProgressBar->setGeometry(textX, 46, qMax(20, textW - timeW - kRowContentGap), 3);
    nowTimeLabel->setGeometry(textX + textW - timeW, 41, timeW, 14);
}

void QueueWindow::animateNowSwap(bool upFlow, const QPixmap &oldSnapshot) {
    const QPoint target(kMargin, kMargin);
    const int offset = kRowEnterOffsetPx + 4;

    QLabel *ghost = new QLabel(containerWidget);
    ghost->setAttribute(Qt::WA_TransparentForMouseEvents);
    ghost->setStyleSheet("border: none; background: transparent;");
    ghost->setPixmap(oldSnapshot);
    ghost->setGeometry(nowWidget->geometry());
    QGraphicsOpacityEffect *ghostEffect = new QGraphicsOpacityEffect(ghost);
    ghostEffect->setOpacity(1.0);
    ghost->setGraphicsEffect(ghostEffect);
    ghost->show();

    QPropertyAnimation *ghostSlide = new QPropertyAnimation(ghost, "pos", ghost);
    ghostSlide->setDuration(animMs(kRowFadeOutMs));
    ghostSlide->setEasingCurve(QEasingCurve::InQuad);
    ghostSlide->setStartValue(ghost->pos());
    ghostSlide->setEndValue(ghost->pos() + QPoint(0, upFlow ? -offset : offset));
    ghostSlide->start(QAbstractAnimation::DeleteWhenStopped);

    QPropertyAnimation *ghostFade = new QPropertyAnimation(ghostEffect, "opacity", ghost);
    ghostFade->setDuration(animMs(kRowFadeOutMs));
    ghostFade->setEasingCurve(QEasingCurve::InQuad);
    ghostFade->setStartValue(1.0);
    ghostFade->setEndValue(0.0);
    connect(ghostFade, &QPropertyAnimation::finished, ghost, &QWidget::deleteLater);
    ghostFade->start(QAbstractAnimation::DeleteWhenStopped);

    if (nowSwapAnimation) {
        nowSwapAnimation->stop();
    }
    nowWidget->move(target + QPoint(0, upFlow ? offset : -offset));
    if (QGraphicsOpacityEffect *effect = opacityEffectOf(nowWidget)) {
        effect->setOpacity(0.0);
        QPropertyAnimation *fadeIn = new QPropertyAnimation(effect, "opacity", nowWidget);
        fadeIn->setDuration(animMs(kRowFadeInMs));
        fadeIn->setEasingCurve(QEasingCurve::OutQuad);
        fadeIn->setStartValue(0.0);
        fadeIn->setEndValue(1.0);
        fadeIn->start(QAbstractAnimation::DeleteWhenStopped);
    }

    QPropertyAnimation *slideIn = new QPropertyAnimation(nowWidget, "pos", nowWidget);
    slideIn->setDuration(animMs(kSlideMs));
    slideIn->setEasingCurve(QEasingCurve::OutCubic);
    slideIn->setStartValue(nowWidget->pos());
    slideIn->setEndValue(target);
    nowSwapAnimation = slideIn;
    slideIn->start(QAbstractAnimation::DeleteWhenStopped);
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
    containerWidget->setStyleSheet(QString("background-color: %1; border-radius: %2px; border: 1px solid %3;")
        .arg(overlaySettings.backgroundColor).arg(theme::kRadiusLg).arg(theme::kNeutral700));

    headerLabel->setFont([]{ QFont f = theme::uiFont(10); f.setCapitalization(QFont::AllUppercase); f.setLetterSpacing(QFont::AbsoluteSpacing, 1.4); return f; }());
    headerLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(overlaySettings.accentColor));
    headerCountLabel->setFont(theme::uiFont(10, QFont::Normal, /*tabular=*/true));
    headerCountLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kNeutral600));
    QFont hintFont = theme::uiFont(10);
    hintFont.setCapitalization(QFont::AllUppercase);
    hintFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    lockHintTextLabel->setFont(hintFont);
    lockHintTextLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kNeutral600));
    lockHintIconLabel->setStyleSheet("border: none; background: transparent;");
    lockHintIconLabel->setPixmap(lockPixmap(kHeaderIconSize, QColor(theme::kNeutral600)));

    emptyLabel->setFont(theme::uiFont(13));
    emptyLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kNeutral500));

    nowTitleLabel->setFont([]{ QFont f = theme::uiFont(16, QFont::Medium); f.setLetterSpacing(QFont::PercentageSpacing, 99); return f; }());
    nowTitleLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(overlaySettings.primaryTextColor));
    nowArtistLabel->setFont(theme::uiFont(13));
    nowArtistLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kNeutral500));
    nowTimeLabel->setFont(theme::uiFont(11, QFont::Normal, /*tabular=*/true));
    nowTimeLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kNeutral600));
    nowProgressBar->setStyleSheet(QString(
        "QProgressBar { background-color: %1; border: none; border-radius: 1px; }"
        "QProgressBar::chunk { background-color: %2; border-radius: 1px; }")
        .arg(theme::kNeutral800, overlaySettings.progressBarColor));
    if (nowArtLabel->pixmap(Qt::ReturnByValue).isNull()) {
        nowArtLabel->setFont(theme::uiFont(22));
        nowArtLabel->setStyleSheet(QString("border: none; border-radius: %1px; background-color: %2; color: %3;")
            .arg(theme::kRadiusMd).arg(theme::kNeutral800, theme::kAccent700));
    }

    lockedChip->setStyleSheet(QString("background-color: %1; border-radius: 6px;").arg(theme::kAccent900));
    lockedChipIcon->setPixmap(lockPixmap(10, QColor(theme::kAccent300)));
    QFont chipFont = theme::uiFont(10, QFont::Medium);
    chipFont.setCapitalization(QFont::AllUppercase);
    chipFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
    lockedChipText->setFont(chipFont);
    lockedChipText->setStyleSheet(QString("color: %1; background: transparent; border: none;").arg(theme::kAccent300));

    static_cast<FadingRule *>(dividerWidget)->setColor(QColor(233, 233, 237, 41));

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
    if (queueSettings.windowX != oldX || queueSettings.windowY != oldY) {
        applyStoredOrDefaultPosition();
    }

    if (queueSettings.locked != wasLocked) {
        dragging = false;
        resizingEdge = EdgeHit::None;
        setCursor(Qt::ArrowCursor);
        for (Row &row : rows) {
            animateRowHover(row, false);
        }
        const bool wasVisible = isVisible();
        setWindowFlag(Qt::WindowTransparentForInput, queueSettings.locked);
        if (wasVisible && queueSettings.enabled) {
            setWindowOpacity(queueSettings.opacityPercent / 100.0);
            show();
        }
    }

    for (const Row &row : rows) {
        styleRowLabels(row);
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
    return qMax(kHeaderIconSize, headerLabel->sizeHint().height());
}

int QueueWindow::nowBlockHeight() const {
    return queueSettings.showNowPlaying ? kNowHeight : 0;
}

int QueueWindow::dividerBlockHeight() const {
    return queueSettings.showNowPlaying ? (kDividerMarginTop + 1 + kDividerMarginBottom) : 0;
}

bool QueueWindow::lockedChipVisible() const {
    return queueSettings.locked && queueSettings.showLockIcon && queueSettings.showNowPlaying;
}

int QueueWindow::rowY(int index) const {
    return kMargin + nowBlockHeight() + dividerBlockHeight() + headerHeight() + kHeaderGap + index * (kRowHeight + kRowGap);
}

int QueueWindow::rowWidth() const {
    return qMax(60, width() - 2 * kMargin);
}

int QueueWindow::contentHeightFor(int rowCount) const {
    const int top = kMargin + nowBlockHeight() + dividerBlockHeight() + headerHeight() + kHeaderGap;
    if (rowCount <= 0) {
        return top + emptyLabel->heightForWidth(rowWidth()) + kMargin;
    }
    return top + rowCount * kRowHeight + (rowCount - 1) * kRowGap + kMargin;
}

void QueueWindow::refreshRows(bool animate) {
    const int shown = qMin(int(queue.size()), queueSettings.maxSongs);

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
                row.widget->move(kMargin, rowY(i));
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
    row.widget->setCursor(Qt::PointingHandCursor);
    row.widget->installEventFilter(this);

    row.hoverBg = new HoverHighlight(row.widget);
    row.hoverBg->setGeometry(row.widget->rect());
    row.hoverBg->lower();

    QHBoxLayout *rowLayout = new QHBoxLayout(row.widget);
    rowLayout->setContentsMargins(theme::kSpace3, 0, theme::kSpace3, 0);
    rowLayout->setSpacing(kRowContentGap);

    // Index slot: the row number cross-fades to a play glyph on hover.
    row.indexSlot = new QWidget(row.widget);
    row.indexSlot->setFixedSize(kIndexSlot, 20);
    row.indexLabel = new QLabel(row.indexSlot);
    row.indexLabel->setGeometry(0, 0, kIndexSlot, 20);
    row.indexLabel->setAlignment(Qt::AlignCenter);
    row.playLabel = new QLabel(row.indexSlot);
    row.playLabel->setGeometry(0, 0, kIndexSlot, 20);
    row.playLabel->setAlignment(Qt::AlignCenter);
    row.playLabel->setPixmap(playTrianglePixmap(kIndexSlot, QColor(theme::kAccent400)));
    QGraphicsOpacityEffect *indexEffect = new QGraphicsOpacityEffect(row.indexLabel);
    indexEffect->setOpacity(1.0);
    row.indexLabel->setGraphicsEffect(indexEffect);
    QGraphicsOpacityEffect *playEffect = new QGraphicsOpacityEffect(row.playLabel);
    playEffect->setOpacity(0.0);
    row.playLabel->setGraphicsEffect(playEffect);

    row.artLabel = new QLabel(row.widget);
    row.artLabel->setFixedSize(kArtSize, kArtSize);
    row.artLabel->setAlignment(Qt::AlignCenter);
    row.artLabel->setText(QStringLiteral("♪"));
    row.artUrl = track.artUrl;

    QVBoxLayout *textLayout = new QVBoxLayout();
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(0);
    row.titleLabel = new QLabel(row.widget);
    row.artistLabel = new QLabel(row.widget);
    textLayout->addWidget(row.titleLabel);
    textLayout->addWidget(row.artistLabel);

    row.sparkleLabel = new QLabel(row.widget);
    row.sparkleLabel->setFixedWidth(kRowBadgeSize);
    row.sparkleLabel->setAlignment(Qt::AlignCenter);
    row.sparkleLabel->hide();

    row.likeLabel = new QLabel(row.widget);
    row.likeLabel->setFixedWidth(kRowBadgeSize);
    row.likeLabel->setAlignment(Qt::AlignCenter);
    row.likeLabel->hide();

    row.durationLabel = new QLabel(row.widget);
    row.durationLabel->setMinimumWidth(30);
    row.durationLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    rowLayout->addWidget(row.indexSlot, 0);
    rowLayout->addWidget(row.artLabel, 0);
    rowLayout->addLayout(textLayout, 1);
    rowLayout->addWidget(row.sparkleLabel, 0);
    rowLayout->addWidget(row.likeLabel, 0);
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
    QColor hoverColor(queueSettings.hoverColor);
    if (!hoverColor.isValid()) {
        hoverColor = QColor(theme::kText);
    }
    static_cast<HoverHighlight *>(row.hoverBg)->setColor(hoverColor);
    row.indexLabel->setFont(theme::uiFont(12, QFont::Normal, /*tabular=*/true));
    row.indexLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kNeutral600));
    row.titleLabel->setFont(theme::uiFont(14, QFont::Medium));
    row.titleLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(overlaySettings.primaryTextColor));
    row.artistLabel->setFont(theme::uiFont(12));
    row.artistLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kNeutral500));
    row.durationLabel->setFont(theme::uiFont(11, QFont::Normal, /*tabular=*/true));
    row.durationLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kNeutral600));
    if (row.artLabel->pixmap(Qt::ReturnByValue).isNull()) {
        row.artLabel->setFont(theme::uiFont(16));
        row.artLabel->setStyleSheet(QString("border: none; border-radius: %1px; background-color: %2; color: %3;")
            .arg(theme::kRadiusMd).arg(theme::kNeutral800, theme::kAccent800));
    }
    refreshRowBadges(row);
}

void QueueWindow::refreshRowBadges(const Row &row) const {
    // One badge slot: a Smart Shuffle sparkle when the recommendation can't be
    // liked, else a liked heart. When neither applies, nothing shows — the empty
    // outline heart is dropped, so a badge always means something.
    const bool showSparkle = row.smartShuffle && !row.liked;
    row.sparkleLabel->setPixmap(sparklePixmap(kRowBadgeSize, QColor(overlaySettings.accentColor)));
    row.sparkleLabel->setVisible(showSparkle);
    if (row.liked) {
        row.likeLabel->setText(QStringLiteral("♥"));
        row.likeLabel->setStyleSheet(QString("color: %1; font-size: 13px; border: none; background: transparent;").arg(overlaySettings.accentColor));
    }
    row.likeLabel->setVisible(row.liked && !showSparkle);
}

void QueueWindow::updateRowContent(Row &row, const UpcomingTrack &track, int index) {
    row.fullTitle = track.title.isEmpty() ? QStringLiteral("Loading…") : track.title;
    row.fullArtist = track.artist.isEmpty() ? QStringLiteral("…") : track.artist;
    row.liked = track.liked;
    row.smartShuffle = track.smartShuffle;
    row.indexLabel->setText(QString::number(index + 1)); // no trailing period
    row.durationLabel->setText(track.durationMs > 0 ? formatDuration(track.durationMs) : QString());
    if (row.artUrl != track.artUrl && !track.artUrl.isEmpty()) {
        row.artUrl = track.artUrl;
        setArtOnLabel(row.artLabel, row.artUrl, kArtSize);
    }
    refreshRowBadges(row);
}

void QueueWindow::animateRowHover(Row &row, bool hovered) {
    if (row.hovered == hovered) {
        return;
    }
    row.hovered = hovered;

    if (row.hoverAnimation) {
        row.hoverAnimation->stop();
    }

    HoverHighlight *highlight = static_cast<HoverHighlight *>(row.hoverBg);
    QLabel *indexLabel = row.indexLabel;
    QLabel *playLabel = row.playLabel;
    QVariantAnimation *fade = new QVariantAnimation(highlight);
    fade->setDuration(hovered ? 150 : 220);
    fade->setEasingCurve(QEasingCurve::OutQuad);
    fade->setStartValue(highlight->alphaFraction());
    fade->setEndValue(hovered ? 1.0 : 0.0);
    connect(fade, &QVariantAnimation::valueChanged, highlight, [highlight, indexLabel, playLabel](const QVariant &value) {
        const qreal v = value.toReal();
        highlight->setAlphaFraction(v);
        if (auto *ie = qobject_cast<QGraphicsOpacityEffect *>(indexLabel->graphicsEffect())) {
            ie->setOpacity(1.0 - v);
        }
        if (auto *pe = qobject_cast<QGraphicsOpacityEffect *>(playLabel->graphicsEffect())) {
            pe->setOpacity(v);
        }
    });
    row.hoverAnimation = fade;
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void QueueWindow::moveRowTo(Row &row, int index, bool animate) {
    const QPoint target(kMargin, rowY(index));
    if (row.posAnimation) {
        row.posAnimation->stop();
    }
    if (!animate || row.widget->pos() == target) {
        row.widget->move(target);
        return;
    }

    QPropertyAnimation *slide = new QPropertyAnimation(row.widget, "pos", row.widget);
    slide->setDuration(animMs(kSlideMs));
    slide->setEasingCurve(QEasingCurve::OutCubic);
    slide->setStartValue(row.widget->pos());
    slide->setEndValue(target);
    row.posAnimation = slide;
    slide->start(QAbstractAnimation::DeleteWhenStopped);
}

void QueueWindow::fadeInRowAt(Row &row, int index, int direction) {
    const QPoint target(kMargin, rowY(index));
    row.widget->move(target - QPoint(0, direction * kRowEnterOffsetPx));
    row.widget->show();

    if (QGraphicsOpacityEffect *effect = opacityEffectOf(row.widget)) {
        effect->setOpacity(0.0);
        QPropertyAnimation *fade = new QPropertyAnimation(effect, "opacity", row.widget);
        fade->setDuration(animMs(kRowFadeInMs));
        fade->setEasingCurve(QEasingCurve::OutQuad);
        fade->setStartValue(0.0);
        fade->setEndValue(1.0);
        fade->start(QAbstractAnimation::DeleteWhenStopped);
    }

    QPropertyAnimation *slide = new QPropertyAnimation(row.widget, "pos", row.widget);
    slide->setDuration(animMs(kSlideMs));
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

    if (QGraphicsOpacityEffect *effect = opacityEffectOf(widget)) {
        QPropertyAnimation *fade = new QPropertyAnimation(effect, "opacity", widget);
        fade->setDuration(animMs(kRowFadeOutMs));
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
    slide->setDuration(animMs(kRowFadeOutMs));
    slide->setEasingCurve(QEasingCurve::InQuad);
    slide->setStartValue(widget->pos());
    slide->setEndValue(widget->pos() + QPoint(0, direction * kRowEnterOffsetPx));
    slide->start(QAbstractAnimation::DeleteWhenStopped);
}

void QueueWindow::relayoutStatics() {
    containerWidget->setGeometry(rect());

    const bool showNow = queueSettings.showNowPlaying;
    nowWidget->setVisible(showNow);
    lockedChip->setVisible(lockedChipVisible());

    if (showNow) {
        if (!nowSwapAnimation) {
            nowWidget->setGeometry(kMargin, kMargin, rowWidth(), kNowHeight);
        } else {
            nowWidget->resize(rowWidth(), kNowHeight);
        }
        layoutNowContents();
    }

    dividerWidget->setVisible(showNow);
    if (showNow) {
        dividerWidget->setGeometry(kMargin, kMargin + nowBlockHeight() + kDividerMarginTop, rowWidth(), 1);
    }

    // Header row: "UP NEXT" + count on the left, the lock hint on the right.
    const int headerY = kMargin + nowBlockHeight() + dividerBlockHeight();
    const int hH = headerHeight();
    int x = kMargin;
    const int labelW = headerLabel->sizeHint().width();
    headerLabel->setGeometry(x, headerY, labelW, hH);
    x += labelW + theme::kSpace2;
    const int countW = qMax(8, headerCountLabel->sizeHint().width());
    headerCountLabel->setGeometry(x, headerY, countW, hH);

    const int hintTextW = lockHintTextLabel->sizeHint().width();
    const int hintW = kHeaderIconSize + 5 + hintTextW;
    const int hx = width() - kMargin - hintW;
    const bool hintFits = hx > x + countW + theme::kSpace3;
    lockHintIconLabel->setVisible(hintFits);
    lockHintTextLabel->setVisible(hintFits);
    if (hintFits) {
        lockHintIconLabel->move(hx, headerY + (hH - kHeaderIconSize) / 2);
        lockHintTextLabel->setGeometry(hx + kHeaderIconSize + 5, headerY, hintTextW, hH);
    }

    emptyLabel->setGeometry(kMargin + theme::kSpace3, rowY(0), qMax(60, rowWidth() - 2 * theme::kSpace3), emptyLabel->heightForWidth(rowWidth()));

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
    // Text budget: row width minus padding, index column, album art, badge,
    // duration column and the layout spacing. QLabel doesn't elide on its own.
    const int textBudget = qMax(60, rowWidth() - 2 * theme::kSpace3 - kIndexSlot - kArtSize
                                        - kRowBadgeSize - 30 - 4 * kRowContentGap);
    for (Row &row : rows) {
        row.titleLabel->setText(QFontMetrics(row.titleLabel->font()).elidedText(row.fullTitle, Qt::ElideRight, textBudget));
        row.artistLabel->setText(QFontMetrics(row.artistLabel->font()).elidedText(row.fullArtist, Qt::ElideRight, textBudget));
    }

    const int nowBudget = qMax(60, width() - (kMargin + kNowArtSize + kNowGap) - kMargin);
    int chipReserve = 0;
    if (lockedChipVisible()) {
        lockedChip->adjustSize();
        chipReserve = lockedChip->width() + theme::kSpace2;
    }
    const int titleBudget = qMax(40, nowBudget - chipReserve);
    nowTitleLabel->setText(QFontMetrics(nowTitleLabel->font()).elidedText(nowTitle.isEmpty() ? QStringLiteral("Nothing playing") : nowTitle, Qt::ElideRight, titleBudget));
    nowArtistLabel->setText(QFontMetrics(nowArtistLabel->font()).elidedText(nowArtist.isEmpty() ? QStringLiteral("Spotify") : nowArtist, Qt::ElideRight, nowBudget));

    headerCountLabel->setText(rows.isEmpty() ? QString() : QString::number(rows.size()));
}

void QueueWindow::setArtOnLabel(QLabel *artLabel, const QString &artUrl, int size) {
    if (artUrl.isEmpty()) {
        return;
    }

    const QString cacheKey = artUrl + '@' + QString::number(size);
    const auto cached = artCache.constFind(cacheKey);
    if (cached != artCache.constEnd()) {
        artLabel->setText("");
        artLabel->setStyleSheet("border: none; border-radius: 8px; background: transparent;");
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
        const QPixmap rounded = roundedThumbnail(pixmap, size, theme::kRadiusMd);
        artCache.insert(cacheKey, rounded);
        if (target) {
            target->setText("");
            target->setStyleSheet("border: none; border-radius: 8px; background: transparent;");
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
