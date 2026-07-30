#ifndef OSD_WINDOW_H
#define OSD_WINDOW_H

#include <QWidget>
#include "settings/app_settings.h"
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPixmap>
#include <QVariantAnimation>

class ScrollingLabel;
class OSDCard; // custom card: paints the bleed art, volume edge and pause scrim

// The OSD is design option 3a: a 496x148 card with the album art bleeding into
// the ground on the left and the volume shown as the card's own lit bottom
// edge. It appears for hideDurationMs when the volume or track changes and is
// a passive, click-through display — no transport controls.
class OSDWindow : public QWidget {
    Q_OBJECT
public:
    explicit OSDWindow(QWidget *parent = nullptr);
    void showVolume(int volume, const QString &track, const QString &artist, const QString &albumArtUrl = "", int progressMs = 0, int durationMs = 0, bool isPlaying = false, bool volumeControlSupported = true);
    void syncProgress(int progressMs, bool isPlaying, bool volumeControlSupported);
    // Badge on the title line: a filled/outlined accent heart for the liked
    // state, or Spotify's sparkle when the song is a Smart Shuffle
    // recommendation (which can't be liked). Liked always wins.
    void setLikedState(bool liked, bool smartShuffle = false);
    void applyOverlaySettings(const OverlaySettings &settings);

private slots:
    void onImageDownloaded(QNetworkReply *reply);
    void updateSongProgress();

private:
    void showOverlay();
    void applyAlbumArtFallback();
    void positionOnActiveScreen();
    void applyPlatformOverlayBehavior();
    void updateVolumeVisualState();
    void setPausedOverlayVisible(bool visible);
    void animateVolumeTo(int volume);
    QString formatTime(int ms);
    void refreshStyles();
    void relayout();
    void updateArtistElide();

    OSDCard *card;
    QWidget *textColumn;
    ScrollingLabel *trackLabel;
    QLabel *artistLabel;
    QLabel *timeLabel;
    QLabel *volCaptionLabel;   // "SPOTIFY VOLUME"
    QLabel *volumeNumberLabel; // the number, tabular
    QLabel *percentLabel;      // "%"
    QLabel *heartLabel;
    QProgressBar *songProgressBar;
    QTimer *hideTimer;
    QTimer *progressTimer;
    QNetworkAccessManager *network;
    QVariantAnimation *volumeAnimation;
    QVariantAnimation *pauseAnimation;

    bool likedNow = false;
    bool smartShuffleNow = false;
    int currentVolumeValue = 0;
    int currentProgressMs = 0;
    int totalDurationMs = 0;
    bool isPlayingNow = false;
    bool volumeControlSupportedNow = true;
    QString lastArtUrl;
    QString fullArtist;
    OverlaySettings overlaySettings;
};

#endif // OSD_WINDOW_H
