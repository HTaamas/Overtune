#ifndef QUEUE_WINDOW_H
#define QUEUE_WINDOW_H

#include <QWidget>
#include <QPoint>
#include <QPointer>
#include <QHash>
#include <QPixmap>
#include <QElapsedTimer>
#include "settings/app_settings.h"
#include "spotify/spotify_client.h"

class QLabel;
class QNetworkAccessManager;
class QProgressBar;
class QPropertyAnimation;
class QTimer;

// A small always-on-top "Up Next" panel in the same visual style as the OSD.
// Unlike the OSD it never auto-hides: it stays wherever the user drags it,
// can be resized from its left/right edges, and both position and width are
// persisted across launches.
//
// Queue updates are diffed against the rows on screen: rows that survive
// slide to their new slot, fresh rows fade/slide in, played rows fade out.
// Rows are positioned manually (no layout) so their motion can be animated.
class QueueWindow : public QWidget {
    Q_OBJECT
public:
    explicit QueueWindow(QWidget *parent = nullptr);

    void setQueue(const QList<UpcomingTrack> &tracks);
    // Mirrors the OSD's now-playing info (no volume): shown above "UP NEXT".
    void setNowPlaying(const QString &title, const QString &artist, const QString &artUrl,
                       int progressMs, int durationMs, bool isPlaying);
    void syncNowPlayingProgress(int progressMs, bool isPlaying);
    void applyOverlaySettings(const OverlaySettings &settings);
    void applyQueueSettings(const QueueSettings &settings);

signals:
    void windowMoved(const QPoint &topLeft);
    void windowResized(int width);
    void trackActivated(const QString &trackId, const QString &uid);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class EdgeHit { None, Left, Right };

    struct Row {
        QString key;        // trackId + duplicate ordinal; stable across shifts
        QString artUrl;
        QString fullTitle;
        QString fullArtist;
        QWidget *widget = nullptr;
        QWidget *hoverBg = nullptr;
        QLabel *indexLabel = nullptr;
        QLabel *artLabel = nullptr;
        QLabel *titleLabel = nullptr;
        QLabel *artistLabel = nullptr;
        QLabel *durationLabel = nullptr;
        QPointer<QPropertyAnimation> posAnimation;
        QPointer<QPropertyAnimation> hoverAnimation;
    };

    void refreshRows(bool animate);
    Row makeRow(const UpcomingTrack &track);
    void styleRowLabels(const Row &row) const;
    void updateRowContent(Row &row, const UpcomingTrack &track, int index);
    void moveRowTo(Row &row, int index, bool animate);
    void fadeInRowAt(Row &row, int index, int direction);
    void animateRowHover(Row &row, bool hovered);
    void discardRow(const Row &row, bool animate, int direction);

    int headerHeight() const;
    int nowBlockHeight() const;
    int rowY(int index) const;
    int rowWidth() const;
    int contentHeightFor(int rowCount) const;
    void relayoutStatics();
    void animateToContentHeight();
    void updateElides();
    void updateVisibility();
    void fadeTo(qreal targetOpacity, bool hideWhenDone);
    void applyStoredOrDefaultPosition();
    void setArtOnLabel(QLabel *artLabel, const QString &artUrl, int size);
    int estimatedNowProgress() const;
    void updateNowPlayingTime();
    bool lockIconVisible() const;
    EdgeHit edgeHitTest(const QPoint &windowPos) const;
    int rowIndexAt(const QPoint &windowPos) const;
    void updateHoverCursor(const QPoint &windowPos);
    int effectiveWidth() const;
    static QString formatDuration(int ms);

    QWidget *containerWidget;
    QLabel *headerLabel;
    QLabel *emptyLabel;
    QLabel *nowArtLabel;
    QLabel *nowTitleLabel;
    QLabel *nowArtistLabel;
    QLabel *nowTimeLabel;
    QLabel *lockIconLabel;
    QProgressBar *nowProgressBar;
    QTimer *nowTickTimer;
    QList<Row> rows;
    QNetworkAccessManager *network;
    QHash<QString, QPixmap> artCache;
    QPropertyAnimation *geometryAnimation;
    QPropertyAnimation *fadeAnimation;

    QList<UpcomingTrack> queue;
    OverlaySettings overlaySettings;
    QueueSettings queueSettings;

    QString nowTitle;
    QString nowArtist;
    QString nowArtUrl;
    int nowProgressMs = 0;
    int nowDurationMs = 0;
    bool nowIsPlaying = false;
    QElapsedTimer nowClock;

    QPoint dragOffset;
    QPoint pressGlobalPos;
    bool dragging = false;
    EdgeHit resizingEdge = EdgeHit::None;
    QRect resizeStartGeometry;
    QPoint resizeStartGlobal;
};

#endif // QUEUE_WINDOW_H
