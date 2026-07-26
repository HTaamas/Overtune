#include "osd_window.h"
#include <QScreen>
#include <QGuiApplication>
#include <QFont>
#include <QGraphicsDropShadowEffect>
#include <QFontMetrics>
#include <QEasingCurve>
#include <QPainter>
#include <QPainterPath>
#include <QCursor>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __APPLE__
void applyMacOverlayWindowBehavior(QWidget *widget);
void prepareMacOverlayFocusRestore();
void restoreMacOverlayFocus();
#endif

namespace {
// Honors the OS "reduce motion" accessibility setting.
int animMs(int ms) {
    static const bool reduced = []() {
#ifdef _WIN32
        BOOL animEnabled = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animEnabled, 0)) {
            return !animEnabled;
        }
#endif
        return false;
    }();
    return reduced ? 0 : ms;
}

constexpr auto kContainerStyle = "background-color: #1c1c1c; border-radius: 12px; border: 1px solid #333333;";
constexpr auto kAlbumArtFallbackStyle = "border: none; border-radius: 6px; background-color: #2c2c2c; color: #1DB954; font-size: 32px;";
constexpr auto kTrackLabelStyle = "color: #ffffff; font-weight: bold; font-size: 16px; border: none;";
constexpr auto kArtistLabelStyle = "color: #aaaaaa; font-size: 13px; border: none;";
constexpr auto kTimeLabelStyle = "color: #888888; font-size: 11px; font-family: monospace; border: none;";
constexpr auto kSongProgressStyle =
    "QProgressBar { background-color: #333333; border: none; border-radius: 2px; }"
    "QProgressBar::chunk { background-color: #ffffff; border-radius: 2px; }";
constexpr auto kVolumeAvailableStyle =
    "QProgressBar { background-color: #333333; border: none; border-radius: 4px; }"
    "QProgressBar::chunk { background-color: #1DB954; border-radius: 4px; }";
constexpr auto kVolumeUnavailableStyle =
    "QProgressBar { background-color: #2e3834; border: none; border-radius: 4px; }"
    "QProgressBar::chunk { background-color: #6f8f7c; border-radius: 4px; }";

// Type scale (px). Every label sits on one of these steps.
constexpr int kFontTitle = 16;     // OSD track title
constexpr int kFontSecondary = 12; // artist / volume caption
constexpr int kFontMono = 11;      // time / numeric caption

QString makeProgressStyle(const QString &backgroundColor, const QString &chunkColor, int radius) {
    return QString(
        "QProgressBar { background-color: %1; border: none; border-radius: %2px; }"
        "QProgressBar::chunk { background-color: %3; border-radius: %2px; }"
    ).arg(backgroundColor).arg(radius).arg(chunkColor);
}

QString artPlaceholderStyle(const QString &bg, const QString &accent) {
    return QString("border: none; border-radius: 6px; background-color: %1; color: %2; font-size: 32px;")
        .arg(bg, accent);
}

// A small speaker glyph so the volume bar is unmistakably the volume control.
QPixmap speakerPixmap(int size, const QColor &color) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);

    // Speaker body: a small rectangle with a triangular cone.
    const qreal s = size;
    QPainterPath body;
    body.moveTo(s * 0.12, s * 0.38);
    body.lineTo(s * 0.30, s * 0.38);
    body.lineTo(s * 0.50, s * 0.20);
    body.lineTo(s * 0.50, s * 0.80);
    body.lineTo(s * 0.30, s * 0.62);
    body.lineTo(s * 0.12, s * 0.62);
    body.closeSubpath();
    p.drawPath(body);

    // Two sound arcs.
    QPen pen(color, qMax(1.2, s * 0.07));
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(s * 0.40, s * 0.30, s * 0.30, s * 0.40), -60 * 16, 120 * 16);
    p.drawArc(QRectF(s * 0.40, s * 0.20, s * 0.50, s * 0.60), -55 * 16, 110 * 16);
    return pm;
}

// A four-point sparkle: a slim diamond with concave edges.
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

// Spotify's Smart Shuffle mark: a large sparkle with a smaller one below-right.
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

QPixmap makeRoundedPixmap(const QPixmap &source, int targetSize, qreal radius) {
    QPixmap scaled = source.scaled(targetSize, targetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

    QPixmap result(targetSize, targetSize);
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, targetSize, targetSize), radius, radius);
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, scaled);

    return result;
}
}

class ScrollingLabel : public QWidget {
public:
    explicit ScrollingLabel(QWidget *parent = nullptr)
        : QWidget(parent), label(new QLabel(this)), timer(new QTimer(this)), leftFade(new QWidget(this)), rightFade(new QWidget(this)) {
        label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        label->move(0, 0);

        leftFade->setAttribute(Qt::WA_TransparentForMouseEvents);
        rightFade->setAttribute(Qt::WA_TransparentForMouseEvents);
        leftFade->setStyleSheet("background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 rgba(28,28,28,255), stop:1 rgba(28,28,28,0)); border: none;");
        rightFade->setStyleSheet("background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 rgba(28,28,28,0), stop:1 rgba(28,28,28,255)); border: none;");
        leftFade->hide();
        rightFade->hide();

        setAttribute(Qt::WA_OpaquePaintEvent, false);
        setAttribute(Qt::WA_NoSystemBackground, true);

        timer->setInterval(30);
        connect(timer, &QTimer::timeout, this, [this]() { tick(); });
    }

    void setText(const QString &text) {
        if (fullText == text) {
            return;
        }
        fullText = text;
        restart();
    }

    void setLabelStyleSheet(const QString &style) {
        label->setStyleSheet(style);
    }

    void setScrollingEnabled(bool enabled) {
        scrollingEnabled = enabled;

        if (loopWidth <= 0) {
            timer->stop();
            updateFadeVisibility();
            return;
        }

        if (scrollingEnabled) {
            timer->start();
        } else {
            timer->stop();
            offsetPx = 0;
            pauseTicks = 20;
            label->move(0, 0);
        }

        updateFadeVisibility();
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);

        leftFade->setGeometry(0, 0, edgeFadePx, height());
        rightFade->setGeometry(width() - edgeFadePx, 0, edgeFadePx, height());
        leftFade->raise();
        rightFade->raise();

        restart();
    }

private:
    void restart() {
        offsetPx = 0;
        pauseTicks = 20;

        label->setText(fullText);
        const QFontMetrics metrics = label->fontMetrics();
        const int textWidth = metrics.horizontalAdvance(fullText);

        label->setText(QStringLiteral("     "));
        const int gapWidth = metrics.horizontalAdvance(QStringLiteral("     "));

        if (textWidth <= contentsRect().width() + 2) {
            timer->stop();
            label->setText(fullText);
            label->setFixedSize(width(), height());
            label->move(0, 0);
            loopWidth = 0;
            leftFade->hide();
            rightFade->hide();
            return;
        }

        const QString repeated = fullText + QStringLiteral("     ") + fullText;
        label->setText(repeated);
        const int repeatedWidth = metrics.horizontalAdvance(repeated);
        label->setFixedSize(repeatedWidth, height());
        label->move(0, 0);

        loopWidth = textWidth + gapWidth;
        if (scrollingEnabled) {
            timer->start();
        } else {
            timer->stop();
        }
        updateFadeVisibility();
    }

    void tick() {
        if (loopWidth <= 0) {
            return;
        }

        if (pauseTicks > 0) {
            --pauseTicks;
            return;
        }

        offsetPx += 1;
        if (offsetPx >= loopWidth) {
            offsetPx = 0;
            pauseTicks = 20;
        }

        label->move(-offsetPx, 0);
        updateFadeVisibility();
    }

    void updateFadeVisibility() {
        if (loopWidth <= 0) {
            leftFade->hide();
            rightFade->hide();
            return;
        }

        rightFade->show();
        rightFade->raise();

        if (offsetPx > 0 && scrollingEnabled) {
            leftFade->show();
            leftFade->raise();
        } else {
            leftFade->hide();
        }
    }

    QLabel *label;
    QTimer *timer;
    QString fullText;
    int offsetPx = 0;
    int loopWidth = 0;
    int pauseTicks = 0;
    bool scrollingEnabled = true;
    QWidget *leftFade;
    QWidget *rightFade;
    const int edgeFadePx = 8;
};

OSDWindow::OSDWindow(QWidget *parent) : QWidget(parent), network(new QNetworkAccessManager(this)) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    applyPlatformOverlayBehavior();
    setFocusPolicy(Qt::NoFocus);
    setWindowModality(Qt::NonModal);
    clearFocus();

#if defined(Q_OS_LINUX)
    setAttribute(Qt::WA_X11DoNotAcceptFocus, true);
#endif

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    containerWidget = new QWidget(this);
    containerWidget->setStyleSheet(kContainerStyle);

    QHBoxLayout *hLayout = new QHBoxLayout(containerWidget);
    hLayout->setContentsMargins(15, 15, 15, 15);
    hLayout->setSpacing(15);

    albumArtLabel = new QLabel(this);
    albumArtLabel->setFixedSize(80, 80);
    albumArtLabel->setStyleSheet(artPlaceholderStyle(overlaySettings.borderColor, overlaySettings.accentColor));
    albumArtLabel->setScaledContents(false);
    albumArtLabel->setAlignment(Qt::AlignCenter);
    albumArtLabel->setText("🎵");

    pauseOverlay = new QLabel(albumArtLabel);
    pauseOverlay->setFixedSize(albumArtLabel->size());
    pauseOverlay->setAlignment(Qt::AlignCenter);
    QPixmap pauseIcon(20, 20);
    pauseIcon.fill(Qt::transparent);
    {
        QPainter painter(&pauseIcon);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 235));
        painter.drawRoundedRect(QRectF(4, 3, 4, 14), 1.5, 1.5);
        painter.drawRoundedRect(QRectF(12, 3, 4, 14), 1.5, 1.5);
    }
    pauseOverlay->setPixmap(pauseIcon);
    pauseOverlay->setStyleSheet("background-color: rgba(0, 0, 0, 110); border-radius: 6px; border: none;");
    pauseOverlay->move(0, 0);

    pauseOverlayEffect = new QGraphicsOpacityEffect(pauseOverlay);
    pauseOverlayEffect->setOpacity(0.0);
    pauseOverlay->setGraphicsEffect(pauseOverlayEffect);

    pauseOverlayFade = new QPropertyAnimation(pauseOverlayEffect, "opacity", this);
    pauseOverlayFade->setDuration(animMs(200));
    pauseOverlayFade->setEasingCurve(QEasingCurve::InOutQuad);

    pauseOverlay->show();
    hLayout->addWidget(albumArtLabel);

    QVBoxLayout *textLayout = new QVBoxLayout();
    textLayout->setSpacing(2);

    trackLabel = new ScrollingLabel(this);
    trackLabel->setLabelStyleSheet(kTrackLabelStyle);
    trackLabel->setFixedHeight(24);

    heartLabel = new QLabel(this);
    heartLabel->setFixedWidth(20);
    heartLabel->setFixedHeight(24);
    heartLabel->setAlignment(Qt::AlignCenter);

    // Title line: scrolling track name on the left, liked heart pinned right.
    QHBoxLayout *titleRow = new QHBoxLayout();
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(6);
    titleRow->addWidget(trackLabel, 1);
    titleRow->addWidget(heartLabel, 0);

    artistLabel = new ScrollingLabel(this);
    artistLabel->setLabelStyleSheet(kArtistLabelStyle);
    artistLabel->setFixedHeight(20);

    volumeLabel = new QLabel(this);
    volumeLabel->setFixedHeight(16);
    volumeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    volumeBar = new QProgressBar(this);
    volumeBar->setRange(0, 100);
    volumeBar->setTextVisible(false);
    volumeBar->setFixedHeight(8);

    // Speaker glyph pinned to the volume bar so it reads unmistakably as the
    // volume control (vs the thinner song-progress bar above it).
    speakerIconLabel = new QLabel(this);
    speakerIconLabel->setFixedSize(14, 14);
    speakerIconLabel->setAlignment(Qt::AlignCenter);

    songProgressBar = new QProgressBar(this);
    songProgressBar->setTextVisible(false);
    songProgressBar->setFixedHeight(4);
    songProgressBar->setStyleSheet(kSongProgressStyle);

    timeLabel = new QLabel(this);
    timeLabel->setStyleSheet(kTimeLabelStyle);
    timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timeLabel->setFixedHeight(16);

    // Progress row: the thin song bar with the elapsed/total time beside it.
    QHBoxLayout *progressRow = new QHBoxLayout();
    progressRow->setContentsMargins(0, 0, 0, 0);
    progressRow->setSpacing(8);
    progressRow->addWidget(songProgressBar, 1);
    progressRow->addWidget(timeLabel, 0);

    // Volume row: speaker icon + the volume bar + the percentage — so each bar
    // is self-labeling and can't be confused with the other.
    QHBoxLayout *volumeRow = new QHBoxLayout();
    volumeRow->setContentsMargins(0, 0, 0, 0);
    volumeRow->setSpacing(6);
    volumeRow->addWidget(speakerIconLabel, 0);
    volumeRow->addWidget(volumeBar, 1);
    volumeRow->addWidget(volumeLabel, 0);

    textLayout->addLayout(titleRow);
    textLayout->addWidget(artistLabel);
    textLayout->addStretch(); // Push progress and volume to the bottom
    textLayout->addLayout(progressRow);
    textLayout->addSpacing(4);
    textLayout->addLayout(volumeRow);

    hLayout->addLayout(textLayout);
    hLayout->setStretch(1, 1);

    mainLayout->addWidget(containerWidget);

    setFixedSize(440, 110); // Exactly album art (80) + top margin (15) + bottom margin (15)

    positionOnActiveScreen();

    hideTimer = new QTimer(this);
    hideTimer->setSingleShot(true);
    connect(hideTimer, &QTimer::timeout, this, [this]() {
        this->hide();
        if (progressTimer->isActive()) progressTimer->stop();
    });

    progressTimer = new QTimer(this);
    connect(progressTimer, &QTimer::timeout, this, &OSDWindow::updateSongProgress);

    overlaySettings = AppSettings::loadOverlaySettings();
    applyOverlaySettings(overlaySettings);
}

void OSDWindow::showVolume(int volume, const QString &track, const QString &artist, const QString &albumArtUrl, int progressMs, int durationMs, bool isPlaying, bool volumeControlSupported) {
    positionOnActiveScreen();
    trackLabel->setText(track.isEmpty() ? "Loading..." : track);
    artistLabel->setText(artist.isEmpty() ? "Spotify" : artist);
    volumeBar->setValue(volume);

    currentProgressMs = progressMs;
    totalDurationMs = durationMs;
    isPlayingNow = isPlaying;
    volumeControlSupportedNow = volumeControlSupported;
    trackLabel->setScrollingEnabled(isPlayingNow);
    artistLabel->setScrollingEnabled(isPlayingNow);
    setPausedOverlayVisible(!isPlayingNow);
    updateVolumeVisualState();
    songProgressBar->setRange(0, durationMs);
    songProgressBar->setValue(progressMs);
    timeLabel->setText(formatTime(progressMs) + " / " + formatTime(durationMs));

    if (!albumArtUrl.isEmpty() && albumArtUrl != lastArtUrl) {
        lastArtUrl = albumArtUrl;
        QNetworkRequest request(albumArtUrl);
        QNetworkReply *reply = network->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            onImageDownloaded(reply);
            showOverlay();
        });
    } else {
        showOverlay();
    }
}

void OSDWindow::setLikedState(bool liked, bool smartShuffle) {
    likedNow = liked;
    smartShuffleNow = smartShuffle;

    // A Smart Shuffle recommendation can't be liked, so its sparkle stands in
    // for the heart; a liked heart always takes precedence.
    if (!liked && smartShuffle) {
        heartLabel->setText("");
        heartLabel->setStyleSheet("border: none; background: transparent;");
        heartLabel->setPixmap(sparklePixmap(16, QColor(overlaySettings.accentColor)));
        return;
    }
    heartLabel->setPixmap(QPixmap());
    heartLabel->setText(liked ? "♥" : "♡");
    heartLabel->setStyleSheet(QString("color: %1; font-size: 16px; border: none;")
        .arg(liked ? overlaySettings.accentColor : overlaySettings.mutedTextColor));
}

void OSDWindow::syncProgress(int progressMs, bool isPlaying, bool volumeControlSupported) {
    currentProgressMs = progressMs;
    isPlayingNow = isPlaying;
    volumeControlSupportedNow = volumeControlSupported;
    trackLabel->setScrollingEnabled(isPlayingNow);
    artistLabel->setScrollingEnabled(isPlayingNow);
    setPausedOverlayVisible(!isPlayingNow);
    updateVolumeVisualState();
    if (isVisible()) {
        songProgressBar->setValue(currentProgressMs);
        timeLabel->setText(formatTime(currentProgressMs) + " / " + formatTime(totalDurationMs));
    }
}

void OSDWindow::updateVolumeVisualState() {
    if (volumeControlSupportedNow) {
        volumeLabel->setText(QString("%1%").arg(volumeBar->value()));
        volumeLabel->setStyleSheet(QString("color: %1; font-size: %2px; font-weight: 600; border: none;").arg(overlaySettings.secondaryTextColor).arg(kFontSecondary));
        volumeBar->setStyleSheet(makeProgressStyle(overlaySettings.borderColor, overlaySettings.accentColor, 4));
        return;
    }

    volumeLabel->setText("unavailable");
    volumeLabel->setStyleSheet(QString("color: %1; font-size: %2px; font-weight: 600; border: none;").arg(overlaySettings.mutedTextColor).arg(kFontSecondary));
    volumeBar->setStyleSheet(makeProgressStyle(overlaySettings.borderColor, overlaySettings.mutedTextColor, 4));
}

void OSDWindow::setPausedOverlayVisible(bool visible) {
    pauseOverlayFade->stop();
    pauseOverlayFade->setStartValue(pauseOverlayEffect->opacity());
    pauseOverlayFade->setEndValue(visible ? 1.0 : 0.0);
    pauseOverlayFade->start();
}

void OSDWindow::updateSongProgress() {
    if (isPlayingNow && totalDurationMs > 0 && currentProgressMs < totalDurationMs) {
        currentProgressMs += 100;
        songProgressBar->setValue(currentProgressMs);
        timeLabel->setText(formatTime(currentProgressMs) + " / " + formatTime(totalDurationMs));
    }
}

void OSDWindow::showOverlay() {
#ifdef __APPLE__
    prepareMacOverlayFocusRestore();
#endif
    applyPlatformOverlayBehavior();
    show();
#ifdef __APPLE__
    restoreMacOverlayFocus();
#endif
    hideTimer->start(overlaySettings.hideDurationMs);
    progressTimer->start(100);
}

void OSDWindow::positionOnActiveScreen() {
    QScreen *targetScreen = this->screen();
    if (!targetScreen) {
        targetScreen = QGuiApplication::screenAt(QCursor::pos());
    }
    if (!targetScreen) {
        targetScreen = QGuiApplication::primaryScreen();
    }
    if (!targetScreen) {
        return;
    }

    const QRect screenGeometry = targetScreen->availableGeometry();
    move(
        screenGeometry.x() + (screenGeometry.width() - width()) / 2,
        screenGeometry.y() + screenGeometry.height() - 180
    );
}

void OSDWindow::applyPlatformOverlayBehavior() {
#ifdef __APPLE__
    applyMacOverlayWindowBehavior(this);
#endif
}

QString OSDWindow::formatTime(int ms) {
    int totalSeconds = ms / 1000;
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
}

void OSDWindow::applyAlbumArtFallback() {
    albumArtLabel->setPixmap(QPixmap());
    albumArtLabel->setStyleSheet(artPlaceholderStyle(overlaySettings.borderColor, overlaySettings.accentColor));
    albumArtLabel->setText("♪");
}

void OSDWindow::onImageDownloaded(QNetworkReply *reply) {
    if (reply->error() == QNetworkReply::NoError) {
        QByteArray imageData = reply->readAll();
        QPixmap pixmap;
        if (pixmap.loadFromData(imageData)) {
            albumArtLabel->setText(""); // Clear fallback
            albumArtLabel->setStyleSheet("border: none; border-radius: 6px; background: transparent;");
            albumArtLabel->setPixmap(makeRoundedPixmap(pixmap, albumArtLabel->width(), 6.0));
        } else {
            applyAlbumArtFallback();
        }
    } else {
        albumArtLabel->setPixmap(QPixmap());
        albumArtLabel->setStyleSheet(artPlaceholderStyle(overlaySettings.borderColor, overlaySettings.accentColor));
        albumArtLabel->setText("🎵");
        qDebug() << "Image Download Error:" << reply->errorString();
    }
    reply->deleteLater();
}

void OSDWindow::applyOverlaySettings(const OverlaySettings &settings) {
    overlaySettings = settings;
    setFixedSize(overlaySettings.overlayWidth, height());
    refreshStyles();
    updateVolumeVisualState();
}

void OSDWindow::refreshStyles() {
    containerWidget->setStyleSheet(QString("background-color: %1; border-radius: 12px; border: 1px solid %2;")
        .arg(overlaySettings.backgroundColor, overlaySettings.borderColor));
    trackLabel->setLabelStyleSheet(QString("color: %1; font-weight: bold; font-size: %2px; border: none;").arg(overlaySettings.primaryTextColor).arg(kFontTitle));
    artistLabel->setLabelStyleSheet(QString("color: %1; font-size: %2px; border: none;").arg(overlaySettings.secondaryTextColor).arg(kFontSecondary));
    timeLabel->setStyleSheet(QString("color: %1; font-size: %2px; font-family: monospace; border: none;").arg(overlaySettings.mutedTextColor).arg(kFontMono));
    speakerIconLabel->setPixmap(speakerPixmap(14, QColor(overlaySettings.secondaryTextColor)));
    setLikedState(likedNow, smartShuffleNow);
    songProgressBar->setStyleSheet(makeProgressStyle(overlaySettings.borderColor, overlaySettings.progressBarColor, 2));

    if (albumArtLabel->pixmap(Qt::ReturnByValue).isNull()) {
        applyAlbumArtFallback();
    }
}
