#include "osd_window.h"
#include "theme.h"
#include <QScreen>
#include <QGuiApplication>
#include <QFont>
#include <QFontMetrics>
#include <QEasingCurve>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
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
using theme::animMs;

constexpr int kArtSize = 148;    // full-bleed album art, flush to the card edges
constexpr int kFrameHeight = 148;
constexpr int kRadius = 14;
constexpr int kEdgeHeight = 3;   // the lit bottom-edge volume strip

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

// Cover-crops source to a size x size square (no rounding — the card clips the
// corners with its own rounded-rect overflow).
QPixmap coverSquare(const QPixmap &source, int size) {
    QPixmap scaled = source.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPixmap result(size, size);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const int x = (size - scaled.width()) / 2;
    const int y = (size - scaled.height()) / 2;
    painter.drawPixmap(x, y, scaled);
    return result;
}
}

// The card: everything that isn't a text widget is painted here so the art can
// bleed under the card's rounded corners (overflow:hidden), the volume can be
// the card's own lit bottom edge, and the pause scrim can cover the whole art
// block. Text widgets are children laid on top of this background.
class OSDCard : public QWidget {
public:
    explicit OSDCard(QWidget *parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setArt(const QPixmap &pm) { art = pm; update(); }
    void setColors(const QColor &background, const QColor &fallbackGround,
                   const QColor &fallbackGlyph, const QColor &edgeHairline) {
        bg = background;
        artFallbackGround = fallbackGround;
        artFallbackGlyph = fallbackGlyph;
        hairline = edgeHairline;
        update();
    }
    void setVolumeColors(const QColor &fill, bool glow) {
        volFill = fill;
        volGlow = glow;
        update();
    }
    void setVolumeFraction(qreal f) { volFrac = qBound<qreal>(0.0, f, 1.0); update(); }
    qreal volumeFraction() const { return volFrac; }
    void setPauseOpacity(qreal o) { pauseOpacity = qBound<qreal>(0.0, o, 1.0); update(); }
    qreal pauseOpacityValue() const { return pauseOpacity; }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QRectF r = rect();

        QPainterPath clip;
        clip.addRoundedRect(r, kRadius, kRadius);
        p.setClipPath(clip);

        p.fillRect(r, bg);

        // Album art, full-bleed on the left.
        const QRectF artRect(0, 0, kArtSize, kArtSize);
        if (!art.isNull()) {
            p.drawPixmap(artRect.topLeft(), art);
        } else {
            p.fillRect(artRect, artFallbackGround);
            QFont glyphFont;
            glyphFont.setPixelSize(40);
            p.setFont(glyphFont);
            p.setPen(artFallbackGlyph);
            p.drawText(artRect, Qt::AlignCenter, QStringLiteral("♪")); // ♪
        }

        // Dissolve the art's right side into the ground.
        QLinearGradient dissolve(0, 0, kArtSize, 0);
        QColor clear = bg;
        clear.setAlpha(0);
        dissolve.setColorAt(0.0, clear);
        dissolve.setColorAt(0.45, clear);
        dissolve.setColorAt(1.0, bg);
        p.fillRect(artRect, dissolve);

        // Paused: a scrim + two bars over the whole art block.
        if (pauseOpacity > 0.001) {
            p.fillRect(artRect, QColor(0, 0, 0, int(110 * pauseOpacity)));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, int(235 * pauseOpacity)));
            const qreal cx = kArtSize / 2.0, cy = kArtSize / 2.0;
            p.drawRoundedRect(QRectF(cx - 12, cy - 22, 8, 44), 2, 2);
            p.drawRoundedRect(QRectF(cx + 4, cy - 22, 8, 44), 2, 2);
        }

        // Volume as the card's lit bottom edge.
        const qreal ey = r.height() - kEdgeHeight;
        QColor track(233, 233, 237);
        track.setAlpha(20); // rgba(233,233,237,0.08)
        p.fillRect(QRectF(0, ey, r.width(), kEdgeHeight), track);

        const qreal fillW = r.width() * volFrac;
        if (fillW > 0.0) {
            if (volGlow) {
                // A soft halo around the whole fill: concentric rounded rects
                // growing outward and fading out, so the glow bleeds up and
                // sideways past the tip and dissolves — no hard edge, no bright
                // seam where it meets the fill (which is drawn crisp on top).
                p.setPen(Qt::NoPen);
                const int layers = 9;
                const qreal maxGrow = 16.0;
                for (int i = layers; i >= 1; --i) {
                    const qreal t = qreal(i) / layers; // 1 (outer) .. ~0.11 (inner)
                    const qreal grow = maxGrow * t;
                    QColor c = volFill;
                    c.setAlpha(int(58.0 * (1.0 - t) * (1.0 - t) + 8));
                    QPainterPath hp;
                    hp.addRoundedRect(QRectF(-grow, ey - grow, fillW + 2 * grow, kEdgeHeight + 2 * grow),
                                      grow + 1.5, grow + 1.5);
                    p.fillPath(hp, c);
                }
            }
            p.fillRect(QRectF(0, ey, fillW, kEdgeHeight), volFill);
        }

        // shadow-md option (a): a 1px hairline edge instead of an ambient blur.
        p.setClipping(false);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(hairline, 1));
        QPainterPath edge;
        edge.addRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), kRadius - 0.5, kRadius - 0.5);
        p.drawPath(edge);
    }

private:
    QPixmap art;
    QColor bg{theme::kBg};
    QColor artFallbackGround{theme::kNeutral800};
    QColor artFallbackGlyph{theme::kAccent700};
    QColor hairline{theme::kNeutral700};
    QColor volFill{theme::kAccent};
    bool volGlow = true;
    qreal volFrac = 0.0;
    qreal pauseOpacity = 0.0;
};

// A single-line label that scrolls its text horizontally when it overflows and
// shows it statically (no edge fades) when it fits. Ported unchanged from the
// previous OSD apart from a configurable fade colour and font.
class ScrollingLabel : public QWidget {
public:
    explicit ScrollingLabel(QWidget *parent = nullptr)
        : QWidget(parent), label(new QLabel(this)), timer(new QTimer(this)),
          leftFade(new QWidget(this)), rightFade(new QWidget(this)) {
        label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        label->move(0, 0);

        leftFade->setAttribute(Qt::WA_TransparentForMouseEvents);
        rightFade->setAttribute(Qt::WA_TransparentForMouseEvents);
        setFadeColor(QColor(theme::kBg));
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

    void setLabelStyleSheet(const QString &style) { label->setStyleSheet(style); }
    void setLabelFont(const QFont &font) {
        label->setFont(font);
        restart();
    }

    void setFadeColor(const QColor &color) {
        const QString rgb = QString("%1,%2,%3").arg(color.red()).arg(color.green()).arg(color.blue());
        leftFade->setStyleSheet(QString("background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 rgba(%1,255), stop:1 rgba(%1,0)); border: none;").arg(rgb));
        rightFade->setStyleSheet(QString("background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 rgba(%1,0), stop:1 rgba(%1,255)); border: none;").arg(rgb));
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
    const int edgeFadePx = 12;
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

    card = new OSDCard(this);
    mainLayout->addWidget(card);

    // Right-hand text column, painted on top of the card.
    textColumn = new QWidget(card);
    textColumn->setStyleSheet("background: transparent; border: none;");
    QVBoxLayout *textLayout = new QVBoxLayout(textColumn);
    textLayout->setContentsMargins(theme::kSpace4, theme::kSpace6, theme::kSpace6, theme::kSpace6);
    textLayout->setSpacing(theme::kSpace2);
    textLayout->addStretch(1);

    trackLabel = new ScrollingLabel(textColumn);
    trackLabel->setFixedHeight(30);
    heartLabel = new QLabel(textColumn);
    heartLabel->setFixedWidth(20);
    heartLabel->setFixedHeight(24);
    heartLabel->setAlignment(Qt::AlignCenter);

    QHBoxLayout *titleRow = new QHBoxLayout();
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(theme::kSpace3);
    titleRow->addWidget(trackLabel, 1);
    titleRow->addWidget(heartLabel, 0);

    artistLabel = new QLabel(textColumn);
    artistLabel->setFixedHeight(18);

    songProgressBar = new QProgressBar(textColumn);
    songProgressBar->setTextVisible(false);
    songProgressBar->setFixedHeight(2);
    timeLabel = new QLabel(textColumn);
    timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timeLabel->setFixedHeight(16);

    QHBoxLayout *progressRow = new QHBoxLayout();
    progressRow->setContentsMargins(0, 0, 0, 0);
    progressRow->setSpacing(theme::kSpace3);
    progressRow->addWidget(songProgressBar, 1);
    progressRow->addWidget(timeLabel, 0);

    volCaptionLabel = new QLabel(QStringLiteral("SPOTIFY VOLUME"), textColumn);
    volumeNumberLabel = new QLabel(textColumn);
    percentLabel = new QLabel(QStringLiteral("%"), textColumn);

    QHBoxLayout *volumeRow = new QHBoxLayout();
    volumeRow->setContentsMargins(0, 0, 0, 0);
    volumeRow->setSpacing(theme::kSpace2);
    volumeRow->addWidget(volCaptionLabel, 1, Qt::AlignBottom);
    volumeRow->addWidget(volumeNumberLabel, 0, Qt::AlignBottom);
    volumeRow->addWidget(percentLabel, 0, Qt::AlignBottom);

    textLayout->addLayout(titleRow);
    textLayout->addWidget(artistLabel);
    textLayout->addLayout(progressRow);
    textLayout->addLayout(volumeRow);
    textLayout->addStretch(1);

    setFixedSize(496, kFrameHeight);
    positionOnActiveScreen();

    hideTimer = new QTimer(this);
    hideTimer->setSingleShot(true);
    connect(hideTimer, &QTimer::timeout, this, [this]() {
        this->hide();
        if (progressTimer->isActive()) progressTimer->stop();
    });

    progressTimer = new QTimer(this);
    connect(progressTimer, &QTimer::timeout, this, &OSDWindow::updateSongProgress);

    volumeAnimation = new QVariantAnimation(this);
    volumeAnimation->setEasingCurve(QEasingCurve::OutQuad);
    connect(volumeAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        card->setVolumeFraction(v.toReal());
    });

    pauseAnimation = new QVariantAnimation(this);
    pauseAnimation->setEasingCurve(QEasingCurve::InOutQuad);
    connect(pauseAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        card->setPauseOpacity(v.toReal());
    });

    overlaySettings = AppSettings::loadOverlaySettings();
    applyOverlaySettings(overlaySettings);
}

void OSDWindow::relayout() {
    card->setGeometry(rect());
    textColumn->setGeometry(kArtSize, 0, width() - kArtSize, height());
}

void OSDWindow::showVolume(int volume, const QString &track, const QString &artist, const QString &albumArtUrl, int progressMs, int durationMs, bool isPlaying, bool volumeControlSupported) {
    positionOnActiveScreen();
    trackLabel->setText(track.isEmpty() ? "Loading..." : track);
    fullArtist = artist.isEmpty() ? QStringLiteral("Spotify") : artist;
    updateArtistElide();

    currentVolumeValue = volume;
    currentProgressMs = progressMs;
    totalDurationMs = durationMs;
    isPlayingNow = isPlaying;
    volumeControlSupportedNow = volumeControlSupported;
    trackLabel->setScrollingEnabled(isPlayingNow);
    setPausedOverlayVisible(!isPlayingNow);
    updateVolumeVisualState();
    animateVolumeTo(volume);
    songProgressBar->setRange(0, qMax(1, durationMs));
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
        heartLabel->setPixmap(sparklePixmap(15, QColor(overlaySettings.accentColor)));
        return;
    }
    heartLabel->setPixmap(QPixmap());
    heartLabel->setText(liked ? QStringLiteral("♥") : QStringLiteral("♡")); // ♥ / ♡
    heartLabel->setStyleSheet(QString("color: %1; font-size: 15px; border: none; background: transparent;")
        .arg(liked ? overlaySettings.accentColor : theme::kNeutral600));
}

void OSDWindow::syncProgress(int progressMs, bool isPlaying, bool volumeControlSupported) {
    currentProgressMs = progressMs;
    isPlayingNow = isPlaying;
    volumeControlSupportedNow = volumeControlSupported;
    trackLabel->setScrollingEnabled(isPlayingNow);
    setPausedOverlayVisible(!isPlayingNow);
    updateVolumeVisualState();
    if (isVisible()) {
        songProgressBar->setValue(currentProgressMs);
        timeLabel->setText(formatTime(currentProgressMs) + " / " + formatTime(totalDurationMs));
    }
}

void OSDWindow::animateVolumeTo(int volume) {
    const qreal target = qBound(0, volume, 100) / 100.0;
    volumeAnimation->stop();
    const int dur = animMs(190);
    if (dur == 0) { // reduce motion: a duration-0 QVariantAnimation is unreliable
        card->setVolumeFraction(target);
        return;
    }
    volumeAnimation->setDuration(dur);
    volumeAnimation->setStartValue(card->volumeFraction());
    volumeAnimation->setEndValue(target);
    volumeAnimation->start();
}

void OSDWindow::updateVolumeVisualState() {
    if (volumeControlSupportedNow) {
        volumeNumberLabel->setText(QString::number(currentVolumeValue));
        volumeNumberLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kAccent300));
        percentLabel->show();
        card->setVolumeColors(QColor(overlaySettings.accentColor), /*glow=*/true);
        return;
    }
    volumeNumberLabel->setText(QStringLiteral("unavailable"));
    volumeNumberLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kNeutral600));
    percentLabel->hide();
    card->setVolumeColors(QColor(theme::kNeutral600), /*glow=*/false);
}

void OSDWindow::setPausedOverlayVisible(bool visible) {
    const qreal target = visible ? 1.0 : 0.0;
    pauseAnimation->stop();
    const int dur = animMs(200);
    if (dur == 0) { // reduce motion: a duration-0 QVariantAnimation is unreliable
        card->setPauseOpacity(target);
        return;
    }
    pauseAnimation->setDuration(dur);
    pauseAnimation->setStartValue(card->pauseOpacityValue());
    pauseAnimation->setEndValue(target);
    pauseAnimation->start();
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

void OSDWindow::updateArtistElide() {
    // Compute the budget from the column geometry (reliable even before the
    // layout has run), not from the label's not-yet-assigned width.
    const int budget = qMax(20, width() - kArtSize - theme::kSpace4 - theme::kSpace6);
    artistLabel->setText(QFontMetrics(artistLabel->font()).elidedText(fullArtist, Qt::ElideRight, budget));
}

void OSDWindow::applyAlbumArtFallback() {
    card->setArt(QPixmap());
}

void OSDWindow::onImageDownloaded(QNetworkReply *reply) {
    if (reply->error() == QNetworkReply::NoError) {
        QByteArray imageData = reply->readAll();
        QPixmap pixmap;
        if (pixmap.loadFromData(imageData)) {
            card->setArt(coverSquare(pixmap, kArtSize));
        } else {
            applyAlbumArtFallback();
        }
    } else {
        applyAlbumArtFallback();
        qDebug() << "Image Download Error:" << reply->errorString();
    }
    reply->deleteLater();
}

void OSDWindow::applyOverlaySettings(const OverlaySettings &settings) {
    overlaySettings = settings;
    setFixedSize(qMax(320, overlaySettings.overlayWidth), kFrameHeight);
    relayout();
    refreshStyles();
    updateVolumeVisualState();
}

void OSDWindow::refreshStyles() {
    card->setColors(QColor(overlaySettings.backgroundColor),
                    QColor(overlaySettings.borderColor),
                    QColor(theme::kAccent700),
                    QColor(theme::kNeutral700));

    // Title: 21px/500, letter-spacing -0.02em, in Inter.
    QFont titleFont = theme::uiFont(21, QFont::Medium);
    titleFont.setLetterSpacing(QFont::PercentageSpacing, 98);
    trackLabel->setLabelFont(titleFont);
    trackLabel->setLabelStyleSheet(QString("color: %1; border: none; background: transparent;").arg(overlaySettings.primaryTextColor));
    trackLabel->setFadeColor(QColor(overlaySettings.backgroundColor));

    artistLabel->setFont(theme::uiFont(13));
    artistLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(overlaySettings.secondaryTextColor));

    timeLabel->setFont(theme::uiFont(11, QFont::Normal, /*tabular=*/true));
    timeLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kNeutral600));

    QFont captionFont = theme::uiFont(10);
    captionFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.2); // ~0.12em at 10px
    captionFont.setCapitalization(QFont::AllUppercase);
    volCaptionLabel->setFont(captionFont);
    volCaptionLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kNeutral600));

    volumeNumberLabel->setFont(theme::uiFont(15, QFont::Medium, /*tabular=*/true));
    percentLabel->setFont(theme::uiFont(10));
    percentLabel->setStyleSheet(QString("color: %1; border: none; background: transparent;").arg(theme::kAccent400));

    songProgressBar->setStyleSheet(QString(
        "QProgressBar { background-color: %1; border: none; border-radius: 1px; }"
        "QProgressBar::chunk { background-color: %2; border-radius: 1px; }")
        .arg(overlaySettings.borderColor, overlaySettings.progressBarColor));

    setLikedState(likedNow, smartShuffleNow);
    updateArtistElide();
}
