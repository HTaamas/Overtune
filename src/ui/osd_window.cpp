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

#ifdef __APPLE__
void applyMacOverlayWindowBehavior(QWidget *widget);
void prepareMacOverlayFocusRestore();
void restoreMacOverlayFocus();
#endif

namespace {
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

QString makeProgressStyle(const QString &backgroundColor, const QString &chunkColor, int radius) {
    return QString(
        "QProgressBar { background-color: %1; border: none; border-radius: %2px; }"
        "QProgressBar::chunk { background-color: %3; border-radius: %2px; }"
    ).arg(backgroundColor).arg(radius).arg(chunkColor);
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
    albumArtLabel->setStyleSheet(QString("border: none; border-radius: 6px; background-color: #2c2c2c; color: %1; font-size: 32px;").arg(overlaySettings.accentColor));
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
    pauseOverlayFade->setDuration(200);
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
    volumeLabel->setFixedHeight(18);
    volumeLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    volumeBar = new QProgressBar(this);
    volumeBar->setRange(0, 100);
    volumeBar->setTextVisible(false);
    volumeBar->setFixedHeight(8);

    // Song Progress Area
    QVBoxLayout *progressArea = new QVBoxLayout();
    progressArea->setSpacing(3);

    songProgressBar = new QProgressBar(this);
    songProgressBar->setTextVisible(false);
    songProgressBar->setFixedHeight(4);
    songProgressBar->setStyleSheet(kSongProgressStyle);

    timeLabel = new QLabel(this);
    timeLabel->setStyleSheet(kTimeLabelStyle);
    timeLabel->setAlignment(Qt::AlignRight);

    QHBoxLayout *statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(8);
    statusRow->addWidget(volumeLabel, 1);
    statusRow->addWidget(timeLabel, 0);

    progressArea->addWidget(songProgressBar);
    progressArea->addLayout(statusRow);

    textLayout->addLayout(titleRow);
    textLayout->addWidget(artistLabel);
    textLayout->addStretch(); // Move stretch here to push progress and volume to the bottom
    textLayout->addLayout(progressArea);
    textLayout->addSpacing(3); // Small gap between status row and volume bar
    textLayout->addWidget(volumeBar);

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

void OSDWindow::setLikedState(bool liked) {
    likedNow = liked;
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
        volumeLabel->setText(QString("Volume %1%").arg(volumeBar->value()));
        volumeLabel->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: 600; border: none;").arg(overlaySettings.secondaryTextColor));
        volumeBar->setStyleSheet(makeProgressStyle("#333333", overlaySettings.accentColor, 4));
        return;
    }

    volumeLabel->setText("Volume unavailable on this device");
    volumeLabel->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: 600; border: none;").arg(overlaySettings.mutedTextColor));
    volumeBar->setStyleSheet(makeProgressStyle("#2e3834", overlaySettings.mutedTextColor, 4));
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
    albumArtLabel->setStyleSheet(QString("border: none; border-radius: 6px; background-color: #2c2c2c; color: %1; font-size: 32px;").arg(overlaySettings.accentColor));
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
        albumArtLabel->setStyleSheet(QString("border: none; border-radius: 6px; background-color: #2c2c2c; color: %1; font-size: 32px;").arg(overlaySettings.accentColor));
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
    trackLabel->setLabelStyleSheet(QString("color: %1; font-weight: bold; font-size: 16px; border: none;").arg(overlaySettings.primaryTextColor));
    artistLabel->setLabelStyleSheet(QString("color: %1; font-size: 13px; border: none;").arg(overlaySettings.secondaryTextColor));
    timeLabel->setStyleSheet(QString("color: %1; font-size: 11px; font-family: monospace; border: none;").arg(overlaySettings.mutedTextColor));
    setLikedState(likedNow);
    songProgressBar->setStyleSheet(makeProgressStyle(overlaySettings.borderColor, overlaySettings.progressBarColor, 2));

    if (albumArtLabel->pixmap(Qt::ReturnByValue).isNull()) {
        applyAlbumArtFallback();
    }
}
