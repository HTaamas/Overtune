#include "settings_dialog.h"
#include "theme.h"
#include "spotify/spotify_client.h"
#include "key_capture_button.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <cmath>
#include <functional>

namespace {
// WCAG relative luminance + contrast ratio, for the readable-text tag.
double channelLuminance(int c8) {
    const double c = c8 / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double relativeLuminance(const QColor &c) {
    return 0.2126 * channelLuminance(c.red()) +
           0.7152 * channelLuminance(c.green()) +
           0.0722 * channelLuminance(c.blue());
}
double contrastRatio(const QColor &a, const QColor &b) {
    const double la = relativeLuminance(a) + 0.05;
    const double lb = relativeLuminance(b) + 0.05;
    return la > lb ? la / lb : lb / la;
}

QLabel *heading(const QString &text, QWidget *parent) {
    QLabel *l = new QLabel(text, parent);
    l->setFont(theme::uiFont(19, QFont::Medium));
    l->setStyleSheet(QString("color: %1; background: transparent;").arg(theme::kText));
    return l;
}
QLabel *subline(const QString &text, QWidget *parent) {
    QLabel *l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setFont(theme::uiFont(12));
    l->setStyleSheet(QString("color: %1; background: transparent;").arg(theme::kNeutral500));
    return l;
}

QString outlinedPrimaryQss() {
    return QString(
        "QPushButton { background: transparent; border: 1px solid %1; border-radius: %2px;"
        " padding: 6px 14px; color: %1; }"
        "QPushButton:hover { background: rgba(145,132,217,0.12); }"
        "QPushButton:pressed { background: rgba(145,132,217,0.22); }"
        "QPushButton:disabled { color: %3; border-color: %3; }")
        .arg(theme::kAccent).arg(theme::kRadiusMd).arg(theme::kNeutral700);
}
QString secondaryQss() {
    return QString(
        "QPushButton { background: transparent; border: 1px solid %1; border-radius: %2px;"
        " padding: 6px 14px; color: %3; }"
        "QPushButton:hover { background: rgba(233,233,237,0.07); }")
        .arg(theme::kDivider).arg(theme::kRadiusMd).arg(theme::kText);
}
QString hexInputQss() {
    return QString(
        "QLineEdit { background: %1; border: 1px solid %2; border-radius: %3px; padding: 4px 8px;"
        " color: %4; font-family: 'Consolas','Menlo',monospace; font-size: 12px; }"
        "QLineEdit:focus { border: 1px solid %5; }")
        .arg(theme::kSurface, theme::kDivider).arg(theme::kRadiusMd).arg(theme::kText, theme::kAccent);
}
QString chipSpinQss() {
    return QString(
        "QSpinBox { background: %1; border: 1px solid %2; border-radius: %3px; padding: 2px 8px;"
        " color: %4; }"
        "QSpinBox::up-button, QSpinBox::down-button { width: 0; height: 0; border: none; }")
        .arg(theme::kSurface, theme::kDivider).arg(theme::kRadiusSm).arg(theme::kText);
}

// The mini overlay rendered inside a preset card: an art block + three bars.
class PresetMini : public QWidget {
public:
    PresetMini(const QString &ground, const QString &art, const QString &text,
               const QString &secondary, const QString &accent, QWidget *parent = nullptr)
        : QWidget(parent), cGround(ground), cArt(art), cText(text), cSecondary(secondary), cAccent(accent) {
        setFixedHeight(44);
        setMinimumWidth(120);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = rect();
        QPainterPath clip;
        clip.addRoundedRect(r, theme::kRadiusSm, theme::kRadiusSm);
        p.setClipPath(clip);
        p.fillRect(r, cGround);

        const qreal pad = 7;
        QRectF art(pad, pad, 30, 30);
        p.setBrush(cArt);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(art, 4, 4);

        const qreal bx = art.right() + 6;
        const qreal bw = r.width() - bx - pad;
        p.setBrush(cText);
        p.drawRoundedRect(QRectF(bx, pad + 3, bw * 0.7, 5), 2, 2);
        p.setBrush(cSecondary);
        p.drawRoundedRect(QRectF(bx, pad + 14, bw * 0.45, 4), 2, 2);
        p.setBrush(cAccent);
        p.drawRoundedRect(QRectF(bx, pad + 23, bw, 4), 2, 2);
    }

private:
    QColor cGround, cArt, cText, cSecondary, cAccent;
};

// A clickable colour swatch with a selection ring.
class Swatch : public QWidget {
public:
    explicit Swatch(const QString &hex, QWidget *parent = nullptr) : QWidget(parent), hexStr(hex) {
        setFixedSize(26, 26);
        setCursor(Qt::PointingHandCursor);
    }
    QString hex() const { return hexStr; }
    void setSelected(bool s) {
        if (sel != s) { sel = s; update(); }
    }
    std::function<void()> onClick;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = rect().adjusted(2, 2, -2, -2);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(hexStr));
        p.drawRoundedRect(r, theme::kRadiusSm, theme::kRadiusSm);
        if (sel) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(theme::kAccent), 2));
            p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), theme::kRadiusSm, theme::kRadiusSm);
        } else {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(233, 233, 237, 30), 1));
            p.drawRoundedRect(r, theme::kRadiusSm, theme::kRadiusSm);
        }
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        if (onClick) onClick();
    }

private:
    QString hexStr;
    bool sel = false;
};

} // namespace

// Defined at global scope so it matches the header's forward declaration.
// The persistent live overlay preview at the bottom of the content pane: a
// simplified mini-OSD that re-renders from the pending OverlaySettings.
class OverlayPreview : public QWidget {
public:
    explicit OverlayPreview(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedHeight(114);
        setMinimumWidth(280);
    }
    void setSettings(const OverlaySettings &s) { settings = s; update(); }

protected:
    // A true miniature of the OSD (option 3a): full-bleed art dissolving into
    // the ground, title/artist, the progress rail, the SPOTIFY VOLUME row, and
    // the lit bottom edge — all in the pending colours, so it can't drift from
    // the real overlay.
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QRectF r = rect();

        // Well.
        QLinearGradient well(r.topLeft(), r.bottomRight());
        well.setColorAt(0.0, QColor(theme::kNeutral900));
        well.setColorAt(1.0, QColor(settings.backgroundColor));
        QPainterPath wp;
        wp.addRoundedRect(r, theme::kRadiusMd, theme::kRadiusMd);
        p.fillPath(wp, well);

        // The OSD card, inset in the well.
        const QRectF card = r.adjusted(9, 9, -9, -9);
        const qreal radius = theme::kRadiusLg * (card.height() / 148.0);
        QPainterPath cp;
        cp.addRoundedRect(card, radius, radius);
        p.save();
        p.setClipPath(cp);
        p.fillRect(card, QColor(settings.backgroundColor));

        // Full-bleed album art (square, flush to the card edges) with a fallback
        // glyph, dissolving into the ground on its right.
        const qreal artW = card.height();
        const QRectF art(card.left(), card.top(), artW, card.height());
        p.fillRect(art, QColor(settings.borderColor));
        p.setFont(theme::uiFont(int(artW * 0.28)));
        p.setPen(QColor(theme::kAccent700));
        p.drawText(art, Qt::AlignCenter, QStringLiteral("♪"));
        QLinearGradient dissolve(art.left(), 0, art.right(), 0);
        QColor clear = QColor(settings.backgroundColor);
        clear.setAlpha(0);
        dissolve.setColorAt(0.0, clear);
        dissolve.setColorAt(0.45, clear);
        dissolve.setColorAt(1.0, QColor(settings.backgroundColor));
        p.fillRect(art, dissolve);

        // Text column.
        const qreal tx = art.right() + 11;
        const qreal rightPad = 12;
        const qreal tw = card.right() - rightPad - tx;
        const qreal top = card.top();

        p.setPen(QColor(settings.primaryTextColor));
        p.setFont(theme::uiFont(15, QFont::Medium));
        p.drawText(QRectF(tx, top + 12, tw, 20), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Lemonade (feat. NAV)"));
        p.setPen(QColor(settings.secondaryTextColor));
        p.setFont(theme::uiFont(11));
        p.drawText(QRectF(tx, top + 33, tw, 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Internet Money, Gunna"));

        // Progress rail + time.
        const qreal py = top + 56;
        const qreal timeW = 58;
        const qreal railW = qMax<qreal>(20, tw - timeW - 8);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(settings.borderColor));
        p.drawRoundedRect(QRectF(tx, py, railW, 2), 1, 1);
        p.setBrush(QColor(settings.progressBarColor));
        p.drawRoundedRect(QRectF(tx, py, railW * 0.62, 2), 1, 1);
        p.setPen(QColor(theme::kNeutral600));
        p.setFont(theme::uiFont(10, QFont::Normal, true));
        p.drawText(QRectF(tx + railW + 8, py - 7, timeW, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("1:58 / 2:44"));

        // SPOTIFY VOLUME caption + number + %.
        const qreal vy = top + 68;
        const qreal pctX = card.right() - rightPad - 8;
        const qreal numX = pctX - 26;
        QFont cap = theme::uiFont(9);
        cap.setCapitalization(QFont::AllUppercase);
        cap.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
        p.setFont(cap);
        p.setPen(QColor(theme::kNeutral600));
        p.drawText(QRectF(tx, vy, numX - tx - 6, 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Spotify volume"));
        p.setPen(QColor(theme::kAccent300));
        p.setFont(theme::uiFont(13, QFont::Medium, true));
        p.drawText(QRectF(numX, vy, 24, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("70"));
        p.setPen(QColor(theme::kAccent400));
        p.setFont(theme::uiFont(9));
        p.drawText(QRectF(pctX, vy, 8, 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("%"));

        // Lit bottom edge = the volume, with the soft accent halo.
        const qreal ey = card.bottom() - 3;
        QColor trackC(233, 233, 237);
        trackC.setAlpha(20);
        p.fillRect(QRectF(card.left(), ey, card.width(), 3), trackC);
        const qreal fillW = card.width() * 0.70;
        p.setPen(Qt::NoPen);
        for (int i = 9; i >= 1; --i) {
            const qreal t = qreal(i) / 9;
            const qreal grow = 13 * t;
            QColor c(settings.accentColor);
            c.setAlpha(int(50.0 * (1.0 - t) * (1.0 - t) + 7));
            QPainterPath hp;
            hp.addRoundedRect(QRectF(card.left() - grow, ey - grow, fillW + 2 * grow, 3 + 2 * grow), grow + 1.5, grow + 1.5);
            p.fillPath(hp, c);
        }
        p.fillRect(QRectF(card.left(), ey, fillW, 3), QColor(settings.accentColor));
        p.restore();

        // shadow-md hairline edge.
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(theme::kNeutral700), 1));
        p.drawPath(cp);
    }

private:
    OverlaySettings settings;
};

namespace {
// Colour roles shown in the "Customise" disclosure, each with a curated ramp.
struct RoleSpec {
    const char *label;
    const char *field;
    QStringList ramp;
    bool textRole;
};
} // namespace

// Bridge so the header's incomplete ColorRole type isn't needed.
struct ColorRole {};

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Overtune Settings");
    setModal(false);
    setFixedWidth(760);
    setMinimumHeight(600);
    setStyleSheet(QString(
        "QDialog { background: %1; }"
        "QLabel { color: %2; background: transparent; }"
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { background: transparent; width: 9px; margin: 0; }"
        "QScrollBar::handle:vertical { background: %3; border-radius: 4px; min-height: 24px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
        "QCheckBox { spacing: 8px; color: %4; background: transparent; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 4px; border: 1px solid %5; background: %6; }"
        "QCheckBox::indicator:checked { background: %7; border-color: %7; }"
        "QSlider::groove:horizontal { height: 3px; background: %8; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { height: 3px; background: %7; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 11px; height: 11px; margin: -4px 0; border-radius: 6px; background: %7; }")
        .arg(theme::kBg, theme::kText, theme::kNeutral800, theme::kNeutral300,
             theme::kNeutral700, theme::kSurface, theme::kAccent, theme::kNeutral800));

    QHBoxLayout *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildSidebar());

    QWidget *content = new QWidget(this);
    QVBoxLayout *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(theme::kSpace6, theme::kSpace6, theme::kSpace8, theme::kSpace6);
    contentLayout->setSpacing(theme::kSpace4);

    stack = new QStackedWidget(content);
    stack->setStyleSheet("background: transparent;");
    stack->addWidget(buildConnectionPage());
    stack->addWidget(buildOverlayPage());
    stack->addWidget(buildUpNextPage());
    stack->addWidget(buildKeybindsPage());
    contentLayout->addWidget(stack, 1);

    // Persistent live overlay preview.
    QLabel *previewLabel = new QLabel(QStringLiteral("LIVE OVERLAY"), content);
    QFont plf = theme::uiFont(10);
    plf.setLetterSpacing(QFont::AbsoluteSpacing, 1.4);
    previewLabel->setFont(plf);
    previewLabel->setStyleSheet(QString("color: %1; background: transparent;").arg(theme::kNeutral600));
    contentLayout->addWidget(previewLabel);
    preview = new OverlayPreview(content);
    contentLayout->addWidget(preview);

    // Footer actions.
    QHBoxLayout *footer = new QHBoxLayout();
    footer->addStretch(1);
    QPushButton *resetButton = new QPushButton(QStringLiteral("Reset"), content);
    resetButton->setStyleSheet(secondaryQss());
    resetButton->setCursor(Qt::PointingHandCursor);
    connect(resetButton, &QPushButton::clicked, this, [this]() {
        setOverlaySettings(OverlaySettings{});
        emit overlaySettingsChanged();
    });
    QPushButton *doneButton = new QPushButton(QStringLiteral("Done"), content);
    doneButton->setStyleSheet(outlinedPrimaryQss());
    doneButton->setCursor(Qt::PointingHandCursor);
    connect(doneButton, &QPushButton::clicked, this, &QDialog::hide);
    footer->addWidget(resetButton);
    footer->addWidget(doneButton);
    contentLayout->addLayout(footer);

    root->addWidget(content, 1);

    wireOverlayControls();
    wireKeybindControls();
    wireQueueControls();

    selectSection(0);
    refreshUi();
}

QWidget *SettingsDialog::buildSidebar() {
    QWidget *sidebar = new QWidget(this);
    sidebar->setFixedWidth(196);
    sidebar->setStyleSheet(QString("background: %1;").arg(theme::kSurface));
    QVBoxLayout *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(theme::kSpace4, theme::kSpace6, theme::kSpace4, theme::kSpace6);
    layout->setSpacing(theme::kSpace1);

    // Wordmark.
    QWidget *brand = new QWidget(sidebar);
    QHBoxLayout *brandLayout = new QHBoxLayout(brand);
    brandLayout->setContentsMargins(theme::kSpace2, 0, theme::kSpace2, theme::kSpace6);
    brandLayout->setSpacing(theme::kSpace2);
    QLabel *mark = new QLabel(brand);
    mark->setFixedSize(16, 16);
    mark->setStyleSheet(QString("background: %1; border-radius: 5px;").arg(theme::kAccent));
    QLabel *word = new QLabel(QStringLiteral("Overtune"), brand);
    word->setFont(theme::uiFont(15, QFont::Medium));
    word->setStyleSheet(QString("color: %1;").arg(theme::kText));
    brandLayout->addWidget(mark);
    brandLayout->addWidget(word);
    brandLayout->addStretch(1);
    layout->addWidget(brand);

    const QStringList sections = {"Connection", "Overlay", "Up Next", "Keybinds"};
    for (int i = 0; i < sections.size(); ++i) {
        QPushButton *nav = new QPushButton(sections.at(i), sidebar);
        nav->setCheckable(true);
        nav->setCursor(Qt::PointingHandCursor);
        nav->setFont(theme::uiFont(13));
        nav->setStyleSheet(QString(
            "QPushButton { text-align: left; padding: 8px %1px; border-radius: %2px;"
            " border: none; color: %3; background: transparent; }"
            "QPushButton:hover { background: rgba(233,233,237,0.07); }"
            "QPushButton:checked { color: %4; background: rgba(145,132,217,0.14);"
            " border-left: 2px solid %4; padding-left: %5px; }")
            .arg(theme::kSpace3).arg(theme::kRadiusMd).arg(theme::kNeutral400)
            .arg(theme::kAccent).arg(theme::kSpace3 - 2));
        connect(nav, &QPushButton::clicked, this, [this, i]() { selectSection(i); });
        navButtons.append(nav);
        layout->addWidget(nav);
    }

    layout->addStretch(1);

    // Connection indicator.
    QWidget *status = new QWidget(sidebar);
    QHBoxLayout *statusLayout = new QHBoxLayout(status);
    statusLayout->setContentsMargins(theme::kSpace3, theme::kSpace3, theme::kSpace3, 0);
    statusLayout->setSpacing(theme::kSpace2);
    sidebarDot = new QLabel(status);
    sidebarDot->setFixedSize(8, 8);
    sidebarStatus = new QLabel(QStringLiteral("Not connected"), status);
    sidebarStatus->setFont(theme::uiFont(11));
    sidebarStatus->setStyleSheet(QString("color: %1;").arg(theme::kNeutral500));
    statusLayout->addWidget(sidebarDot);
    statusLayout->addWidget(sidebarStatus);
    statusLayout->addStretch(1);
    layout->addWidget(status);

    return sidebar;
}

static QScrollArea *wrapScroll(QWidget *page) {
    QScrollArea *area = new QScrollArea();
    area->setWidgetResizable(true);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setFrameShape(QFrame::NoFrame);
    area->setStyleSheet("QScrollArea { background: transparent; border: none; }");
    area->setWidget(page);
    // The viewport otherwise paints QPalette::Base (a #1e1e1e in Windows dark
    // mode) instead of letting the dialog's Nocturne ground show through.
    area->viewport()->setStyleSheet("background: transparent;");
    area->viewport()->setAutoFillBackground(false);
    return area;
}

QWidget *SettingsDialog::buildConnectionPage() {
    QWidget *page = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpace4);

    layout->addWidget(heading("Connection", page));
    layout->addWidget(subline("Authorize Overtune to read and control your Spotify playback. No password or cookie is ever handled by the app.", page));

    QWidget *statusRow = new QWidget(page);
    QHBoxLayout *statusLayout = new QHBoxLayout(statusRow);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->setSpacing(theme::kSpace3);
    QLabel *statusLabel = new QLabel(QStringLiteral("Status"), statusRow);
    statusLabel->setStyleSheet(QString("color: %1;").arg(theme::kNeutral300));
    connectionValueLabel = new QLabel(statusRow);
    connectionValueLabel->setStyleSheet(QString("color: %1;").arg(theme::kText));
    statusLayout->addWidget(statusLabel);
    statusLayout->addWidget(connectionValueLabel);
    statusLayout->addStretch(1);
    layout->addWidget(statusRow);

    helpTextLabel = new QLabel(page);
    helpTextLabel->setWordWrap(true);
    helpTextLabel->setOpenExternalLinks(true);
    helpTextLabel->setStyleSheet(QString("color: %1;").arg(theme::kNeutral400));
    layout->addWidget(helpTextLabel);

    connectButton = new QPushButton(page);
    connectButton->setCursor(Qt::PointingHandCursor);
    connectButton->setStyleSheet(outlinedPrimaryQss());
    connect(connectButton, &QPushButton::clicked, this, &SettingsDialog::connectSpotifyRequested);
    layout->addWidget(connectButton, 0, Qt::AlignLeft);

    QLabel *logLabel = new QLabel(QStringLiteral("Connection log"), page);
    logLabel->setStyleSheet(QString("color: %1;").arg(theme::kNeutral500));
    layout->addWidget(logLabel);
    logViewer = new QPlainTextEdit(page);
    logViewer->setReadOnly(true);
    logViewer->setMinimumHeight(120);
    logViewer->setStyleSheet(QString(
        "QPlainTextEdit { background: %1; color: %2; font-family: 'Consolas','Menlo',monospace;"
        " font-size: 10px; border: 1px solid %3; border-radius: %4px; padding: 6px; }")
        .arg(theme::kBg, theme::kNeutral500, theme::kDivider).arg(theme::kRadiusMd));
    layout->addWidget(logViewer, 1);

    return wrapScroll(page);
}

QWidget *SettingsDialog::buildOverlayPage() {
    QWidget *page = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpace3);

    layout->addWidget(heading("Appearance", page));
    layout->addWidget(subline("Pick a theme for both overlays.", page));

    // Preset grid.
    struct PresetVisual { const char *ground; const char *art; const char *text; const char *secondary; const char *accent; };
    const QList<PresetVisual> visuals = {
        {"#161826", "#3f424d", "#e9e9ed", "#75798c", "#9184d9"},
        {"#0d0f16", "#292b31", "#cfd3e5", "#595d6c", "#b2b6ca"},
        {"#232532", "#423a6a", "#f3f5fe", "#9397ab", "#b5abfc"},
        {"#262a60", "#4c5397", "#f5f4ff", "#a7a1db", "#e7e5fe"},
    };
    QGridLayout *grid = new QGridLayout();
    grid->setSpacing(theme::kSpace3);
    const QList<theme::Preset> &presets = theme::presets();
    for (int i = 0; i < presets.size(); ++i) {
        // The whole card is a flat button; its children are mouse-transparent so
        // a click anywhere on it applies the preset.
        QPushButton *card = new QPushButton(page);
        card->setCursor(Qt::PointingHandCursor);
        // A QPushButton's sizeHint is text-based and ignores a child layout, so
        // reserve the height its mini + name need explicitly.
        card->setMinimumHeight(86);
        QVBoxLayout *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(theme::kSpace3, theme::kSpace3, theme::kSpace3, theme::kSpace3);
        cardLayout->setSpacing(theme::kSpace2);
        const PresetVisual &v = visuals.at(i);
        PresetMini *mini = new PresetMini(v.ground, v.art, v.text, v.secondary, v.accent, card);
        mini->setAttribute(Qt::WA_TransparentForMouseEvents);
        cardLayout->addWidget(mini);
        QWidget *nameRow = new QWidget(card);
        nameRow->setAttribute(Qt::WA_TransparentForMouseEvents);
        QHBoxLayout *nameLayout = new QHBoxLayout(nameRow);
        nameLayout->setContentsMargins(0, 0, 0, 0);
        nameLayout->setSpacing(theme::kSpace2);
        QLabel *name = new QLabel(presets.at(i).name, nameRow);
        name->setFont(theme::uiFont(12, QFont::Medium));
        name->setStyleSheet(QString("color: %1;").arg(theme::kText));
        QLabel *tag = new QLabel(QStringLiteral("Current"), nameRow);
        tag->setFont(theme::uiFont(9));
        tag->setStyleSheet(QString("color: %1; background: %2; border-radius: 4px; padding: 1px 6px;")
            .arg(theme::kAccent200, theme::kAccent800));
        tag->hide();
        nameLayout->addWidget(name);
        nameLayout->addWidget(tag);
        nameLayout->addStretch(1);
        cardLayout->addWidget(nameRow);

        card->setStyleSheet(QString(
            "QPushButton { background: %1; border-radius: %2px; border: 1px solid %3; text-align: left; }")
            .arg(theme::kSurface).arg(theme::kRadiusMd).arg(theme::kDivider));
        connect(card, &QPushButton::clicked, this, [this, i]() { applyPreset(i); });

        presetCards.append(card);
        presetTags.append(tag);
        grid->addWidget(card, i / 2, i % 2);
    }
    layout->addLayout(grid);

    // Disclosure.
    disclosureButton = new QPushButton(page);
    disclosureButton->setCursor(Qt::PointingHandCursor);
    disclosureButton->setFont(theme::uiFont(12));
    disclosureButton->setStyleSheet(QString(
        "QPushButton { text-align: left; color: %1; background: transparent; border: none; padding: %2px 0; }")
        .arg(theme::kAccent).arg(theme::kSpace3));
    connect(disclosureButton, &QPushButton::clicked, this, [this]() { setDisclosureExpanded(!disclosureExpanded); });
    layout->addWidget(disclosureButton);

    // Customise box (hidden until expanded).
    customiseBox = new QWidget(page);
    QVBoxLayout *cust = new QVBoxLayout(customiseBox);
    cust->setContentsMargins(0, 0, 0, 0);
    cust->setSpacing(theme::kSpace3);

    backgroundColorEdit = new QLineEdit(customiseBox);
    surfaceColorEdit = new QLineEdit(customiseBox);
    borderColorEdit = new QLineEdit(customiseBox);
    accentColorEdit = new QLineEdit(customiseBox);
    primaryTextColorEdit = new QLineEdit(customiseBox);
    secondaryTextColorEdit = new QLineEdit(customiseBox);
    mutedTextColorEdit = new QLineEdit(customiseBox);
    progressBarColorEdit = new QLineEdit(customiseBox);

    const QList<RoleSpec> roles = {
        {"Ground",         "background", {"#161826", "#232532", "#292b31", "#3f424d"}, false},
        {"Surface",        "surface",    {"#232532", "#2b2741", "#292b31", "#3f424d"}, false},
        {"Accent",         "accent",     {"#9184d9", "#a7a1db", "#b5abfc", "#e7e5fe"}, false},
        {"Text",           "primary",    {"#e9e9ed", "#cfd3e5", "#b2b6ca", "#9397ab"}, true},
        {"Secondary text", "secondary",  {"#9397ab", "#b2b6ca", "#75798c", "#cfd3e5"}, true},
        {"Muted text",     "muted",      {"#75798c", "#595d6c", "#9397ab", "#3f424d"}, true},
        {"Rail / trough",  "border",     {"#3f424d", "#595d6c", "#292b31", "#232532"}, false},
        {"Progress",       "progress",   {"#cfd3e5", "#b2b6ca", "#9184d9", "#e9e9ed"}, false},
    };
    for (const RoleSpec &role : roles) {
        cust->addWidget(makeColorRow(role.label, colorEditFor(role.field), role.ramp, role.textRole));
    }

    // Sliders.
    auto makeSlider = [&](const QString &label, int lo, int hi, QSlider *&slider, QLabel *&value) {
        QWidget *row = new QWidget(customiseBox);
        QHBoxLayout *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(theme::kSpace3);
        QLabel *l = new QLabel(label, row);
        l->setFixedWidth(104);
        l->setStyleSheet(QString("color: %1; font-size: 12px;").arg(theme::kNeutral400));
        slider = new QSlider(Qt::Horizontal, row);
        slider->setRange(lo, hi);
        value = new QLabel(row);
        value->setFixedWidth(64);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setFont(theme::uiFont(12, QFont::Normal, true));
        value->setStyleSheet(QString("color: %1;").arg(theme::kNeutral400));
        rl->addWidget(l);
        rl->addWidget(slider, 1);
        rl->addWidget(value);
        return row;
    };
    cust->addSpacing(theme::kSpace2);
    cust->addWidget(makeSlider("Overlay width", 320, 900, overlayWidthSlider, overlayWidthValue));
    cust->addWidget(makeSlider("Hide delay", 1000, 15000, hideDurationSlider, hideDurationValue));

    layout->addWidget(customiseBox);
    layout->addStretch(1);

    setDisclosureExpanded(false); // sets the "▸ Customise…" label and hides the box

    return wrapScroll(page);
}

QWidget *SettingsDialog::buildUpNextPage() {
    QWidget *page = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpace4);

    layout->addWidget(heading("Up Next", page));
    layout->addWidget(subline("How the always-on-top queue window behaves. Toggle it and its lock from the tray menu, or with Alt+U / Alt+L.", page));

    auto sentence = [&](const QString &pre, QWidget *chip, const QString &post) {
        QWidget *row = new QWidget(page);
        QHBoxLayout *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(theme::kSpace3);
        QLabel *a = new QLabel(pre, row);
        a->setStyleSheet(QString("color: %1; font-size: 13px;").arg(theme::kNeutral300));
        rl->addWidget(a);
        rl->addWidget(chip);
        QLabel *b = new QLabel(post, row);
        b->setStyleSheet(QString("color: %1; font-size: 13px;").arg(theme::kNeutral300));
        rl->addWidget(b);
        rl->addStretch(1);
        return row;
    };

    queueMaxSongsSpin = new QSpinBox(page);
    queueMaxSongsSpin->setRange(1, 30);
    queueMaxSongsSpin->setStyleSheet(chipSpinQss());
    queueMaxSongsSpin->setFixedWidth(52);
    layout->addWidget(sentence("Show", queueMaxSongsSpin, "upcoming songs, with the current one at the top."));

    queueOpacitySpin = new QSpinBox(page);
    queueOpacitySpin->setRange(20, 100);
    queueOpacitySpin->setSuffix("%");
    queueOpacitySpin->setStyleSheet(chipSpinQss());
    queueOpacitySpin->setFixedWidth(64);
    layout->addWidget(sentence("Window opacity", queueOpacitySpin, "— click-through when locked."));

    queueShowNowPlayingCheck = new QCheckBox("Show the current song above the queue", page);
    queueShowLockIconCheck = new QCheckBox("Show a “Locked” chip while the window is locked", page);
    layout->addWidget(queueShowNowPlayingCheck);
    layout->addWidget(queueShowLockIconCheck);

    QCheckBox *remember = new QCheckBox("Remember where I drag it", page);
    remember->setChecked(true);
    remember->setEnabled(false);
    remember->setToolTip("The window's position and size are always remembered.");
    layout->addWidget(remember);

    layout->addStretch(1);
    return wrapScroll(page);
}

QWidget *SettingsDialog::buildKeybindsPage() {
    QWidget *page = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpace4);

    layout->addWidget(heading("Keybinds", page));
    layout->addWidget(subline("Click a field, then press the key. The main key toggles volume control; hold Shift with it to skip, Ctrl for previous.", page));

    mainKeyButton = new KeyCaptureButton(page);
    likeKeyButton = new KeyCaptureButton(page);
    lockKeyButton = new KeyCaptureButton(page);
    showKeyButton = new KeyCaptureButton(page);

    auto altChip = [&](QWidget *parent) {
        QLabel *l = new QLabel(QStringLiteral("Alt"), parent);
        l->setFont(theme::uiFont(13));
        l->setStyleSheet(QString(
            "color: %1; background: %2; border: 1px solid %3; border-radius: %4px; padding: 6px 10px;")
            .arg(theme::kNeutral400, theme::kSurface, theme::kDivider).arg(theme::kRadiusMd));
        return l;
    };
    auto plus = [&](QWidget *parent) {
        QLabel *l = new QLabel(QStringLiteral("+"), parent);
        l->setStyleSheet(QString("color: %1;").arg(theme::kNeutral600));
        return l;
    };
    auto labelCol = [&](const QString &text, QWidget *parent) {
        QLabel *l = new QLabel(text, parent);
        l->setFixedWidth(150);
        l->setStyleSheet(QString("color: %1; font-size: 13px;").arg(theme::kNeutral300));
        return l;
    };

    // Main key row.
    {
        QWidget *row = new QWidget(page);
        QHBoxLayout *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(theme::kSpace4);
        rl->addWidget(labelCol("Main key", row));
        rl->addWidget(mainKeyButton);
        QLabel *hint = new QLabel(QStringLiteral("+ Shift skip · + Ctrl previous"), row);
        hint->setStyleSheet(QString("color: %1; font-size: 11px;").arg(theme::kNeutral600));
        rl->addWidget(hint);
        rl->addStretch(1);
        layout->addWidget(row);
    }

    // The main key is consumed by the global hook whether or not a modifier is
    // held, so binding it to a typing key makes that key dead in every other
    // app. Warn rather than block — repurposing Caps Lock (the default) is the
    // whole point, and someone may genuinely want an unusual binding.
    mainKeyWarning = new QLabel(page);
    mainKeyWarning->setWordWrap(true);
    mainKeyWarning->setFont(theme::uiFont(11));
    mainKeyWarning->setStyleSheet(QString(
        "color: %1; background: %2; border: 1px solid %3; border-radius: %4px; padding: 8px 10px;")
        .arg(theme::kWarnText, theme::kWarnBg, theme::kWarnBorder).arg(theme::kRadiusMd));
    mainKeyWarning->hide();
    layout->addWidget(mainKeyWarning);

    auto comboRow = [&](const QString &label, KeyCaptureButton *btn) {
        QWidget *row = new QWidget(page);
        QHBoxLayout *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(theme::kSpace2);
        rl->addWidget(labelCol(label, row));
        rl->addSpacing(theme::kSpace2);
        rl->addWidget(altChip(row));
        rl->addWidget(plus(row));
        rl->addWidget(btn);
        rl->addStretch(1);
        layout->addWidget(row);
    };
    comboRow("Like the song", likeKeyButton);
    comboRow("Lock Up Next", lockKeyButton);
    comboRow("Show / hide Up Next", showKeyButton);

    // Divider.
    QLabel *rule = new QLabel(page);
    rule->setFixedHeight(1);
    rule->setStyleSheet(QString(
        "background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 transparent,"
        " stop:0.08 %1, stop:0.92 %1, stop:1 transparent);").arg(theme::kDivider));
    layout->addWidget(rule);

    // Volume step segmented control.
    {
        QWidget *row = new QWidget(page);
        QHBoxLayout *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(theme::kSpace4);
        rl->addWidget(labelCol("Volume step", row));

        QWidget *seg = new QWidget(row);
        seg->setStyleSheet(QString("background: transparent; border: 1px solid %1; border-radius: %2px;")
            .arg(theme::kDivider).arg(theme::kRadiusMd));
        QHBoxLayout *segLayout = new QHBoxLayout(seg);
        segLayout->setContentsMargins(0, 0, 0, 0);
        segLayout->setSpacing(0);
        QButtonGroup *group = new QButtonGroup(this);
        const QList<QPair<QString, int>> opts = {{"5% coarse", 5}, {"2%", 2}, {"10%", 10}};
        for (int i = 0; i < opts.size(); ++i) {
            QPushButton *opt = new QPushButton(opts.at(i).first, seg);
            opt->setCheckable(true);
            opt->setCursor(Qt::PointingHandCursor);
            opt->setProperty("stepValue", opts.at(i).second);
            opt->setStyleSheet(QString(
                "QPushButton { border: none; %1 padding: 7px 12px; font-size: 12px; color: %2; background: transparent; }"
                "QPushButton:checked { color: %3; }")
                .arg(i == 0 ? QString() : QString("border-left: 1px solid %1;").arg(theme::kDivider))
                .arg(theme::kNeutral400, theme::kAccent));
            group->addButton(opt);
            volumeStepButtons.append(opt);
            segLayout->addWidget(opt);
            connect(opt, &QPushButton::clicked, this, [this]() { emit keybindSettingsChanged(); });
        }
        rl->addWidget(seg);
        QLabel *shiftHint = new QLabel(QStringLiteral("Shift → 1%"), row);
        shiftHint->setStyleSheet(QString("color: %1; font-size: 11px;").arg(theme::kNeutral600));
        rl->addWidget(shiftHint);
        rl->addStretch(1);
        layout->addWidget(row);
    }

#ifdef _WIN32
    runAsAdminCheck = new QCheckBox("Run as administrator (next launch)", page);
    runAsAdminCheck->setChecked(AppSettings::loadRunAsAdmin());
    connect(runAsAdminCheck, &QCheckBox::toggled, this, [](bool on) { AppSettings::saveRunAsAdmin(on); });
    layout->addWidget(runAsAdminCheck);
    QLabel *adminHint = subline("Enable only if you want the hotkeys to work while a program running as administrator has focus (some games / anti-cheat).", page);
    layout->addWidget(adminHint);
#endif

    layout->addStretch(1);
    return wrapScroll(page);
}

void SettingsDialog::selectSection(int index) {
    for (int i = 0; i < navButtons.size(); ++i) {
        navButtons.at(i)->setChecked(i == index);
    }
    stack->setCurrentIndex(index);
}

QLineEdit *SettingsDialog::colorEditFor(const QString &role) const {
    if (role == "background") return backgroundColorEdit;
    if (role == "surface") return surfaceColorEdit;
    if (role == "border") return borderColorEdit;
    if (role == "accent") return accentColorEdit;
    if (role == "primary") return primaryTextColorEdit;
    if (role == "secondary") return secondaryTextColorEdit;
    if (role == "muted") return mutedTextColorEdit;
    if (role == "progress") return progressBarColorEdit;
    return nullptr;
}

QWidget *SettingsDialog::makeColorRow(const QString &label, QLineEdit *edit, const QStringList &ramp, bool textRole) {
    QWidget *row = new QWidget(customiseBox);
    QHBoxLayout *rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(theme::kSpace2);

    QLabel *l = new QLabel(label, row);
    l->setFixedWidth(104);
    l->setStyleSheet(QString("color: %1; font-size: 12px;").arg(theme::kNeutral400));
    rl->addWidget(l);

    QList<QWidget *> group;
    for (const QString &hex : ramp) {
        Swatch *sw = new Swatch(hex, row);
        sw->onClick = [this, edit, hex]() { edit->setText(hex); };
        rl->addWidget(sw);
        group.append(sw);
    }
    swatchGroups.append(group);
    swatchRoleEdits.append(edit);

    edit->setStyleSheet(hexInputQss());
    edit->setFixedWidth(96);
    rl->addWidget(edit);

    if (textRole) {
        QLabel *tag = new QLabel(row);
        tag->setFont(theme::uiFont(10));
        rl->addWidget(tag);
        if (edit == primaryTextColorEdit) primaryContrastTag = tag;
        else if (edit == secondaryTextColorEdit) secondaryContrastTag = tag;
        else if (edit == mutedTextColorEdit) mutedContrastTag = tag;
    }
    rl->addStretch(1);
    return row;
}

void SettingsDialog::setDisclosureExpanded(bool expanded) {
    disclosureExpanded = expanded;
    disclosureButton->setText((expanded ? QStringLiteral("▾  ") : QStringLiteral("▸  ")) +
                              QStringLiteral("Customise colours, width and hide delay"));
    customiseBox->setVisible(expanded);
}

void SettingsDialog::applyPreset(int index) {
    const QList<theme::Preset> &presets = theme::presets();
    if (index < 0 || index >= presets.size()) {
        return;
    }
    const theme::Preset &p = presets.at(index);
    presetName = p.name;
    backgroundColorEdit->setText(p.background);
    accentColorEdit->setText(p.accent);
    refreshPresetSelection();
    emit overlaySettingsChanged();
}

void SettingsDialog::refreshPresetSelection() {
    const QList<theme::Preset> &presets = theme::presets();
    const QString bg = backgroundColorEdit->text().trimmed();
    const QString accent = accentColorEdit->text().trimmed();
    int current = -1;
    for (int i = 0; i < presets.size(); ++i) {
        if (QString(presets.at(i).background).compare(bg, Qt::CaseInsensitive) == 0 &&
            QString(presets.at(i).accent).compare(accent, Qt::CaseInsensitive) == 0) {
            current = i;
            break;
        }
    }
    presetName = current >= 0 ? presets.at(current).name : QStringLiteral("Custom");
    for (int i = 0; i < presetCards.size(); ++i) {
        const bool sel = (i == current);
        presetTags.at(i)->setVisible(sel);
        presetCards.at(i)->setStyleSheet(QString("background: %1; border-radius: %2px; border: 1px solid %3;")
            .arg(theme::kSurface).arg(theme::kRadiusMd)
            .arg(sel ? QString(theme::kAccent) : QString(theme::kDivider)));
    }
}

void SettingsDialog::updateColorUi() {
    // Swatch selection rings.
    for (int i = 0; i < swatchGroups.size(); ++i) {
        const QString current = swatchRoleEdits.at(i)->text().trimmed();
        for (QWidget *w : swatchGroups.at(i)) {
            Swatch *sw = static_cast<Swatch *>(w);
            sw->setSelected(sw->hex().compare(current, Qt::CaseInsensitive) == 0);
        }
    }

    // Contrast tags for text roles.
    const QColor bg(backgroundColorEdit->text().trimmed());
    auto setTag = [&](QLabel *tag, QLineEdit *edit) {
        if (!tag) return;
        const QColor c(edit->text().trimmed());
        if (!c.isValid() || !bg.isValid()) {
            tag->setText(QString());
            return;
        }
        const double ratio = contrastRatio(c, bg);
        tag->setText(QString("contrast %1:1").arg(ratio, 0, 'f', 1));
        const bool ok = ratio >= 4.5;
        tag->setStyleSheet(QString("color: %1; background: %2; border-radius: 4px; padding: 1px 6px;")
            .arg(ok ? QString(theme::kAccent200) : QString("#ffd7a8"),
                 ok ? QString(theme::kAccent800) : QString("#5c3a12")));
        tag->setToolTip(ok ? QStringLiteral("Readable on the ground.")
                           : QStringLiteral("Low contrast — aim for 4.5:1 or higher."));
    };
    setTag(primaryContrastTag, primaryTextColorEdit);
    setTag(secondaryContrastTag, secondaryTextColorEdit);
    setTag(mutedContrastTag, mutedTextColorEdit);

    if (preview) {
        preview->setSettings(overlaySettings());
    }
    refreshPresetSelection();
}

void SettingsDialog::showAuthorizationPrompt(const QString &url, const QString &code) {
    connectionValueLabel->setText("Waiting for authorization...");
    if (code.isEmpty()) {
        helpTextLabel->setText(
            QString("<b>Authorize Overtune</b><br>"
                    "A browser window was opened. Approve access and you'll be connected automatically.<br>"
                    "If it didn't open, <a href=\"%1\">click here</a>.").arg(url));
    } else {
        helpTextLabel->setText(
            QString("<b>Authorize Overtune</b><br>"
                    "A browser window was opened to <a href=\"%1\">%1</a>.<br>"
                    "If it didn't open, visit that link and confirm the code <b>%2</b>.").arg(url, code));
    }
}

void SettingsDialog::setAuthenticated(bool isAuthenticated) {
    authenticated = isAuthenticated;
    refreshUi();
}

void SettingsDialog::appendLog(const QString &text) {
    if (logViewer) {
        logViewer->appendPlainText(text);
    }
}

void SettingsDialog::setOverlaySettings(const OverlaySettings &settings) {
    presetName = settings.presetName;
    const QSignalBlocker b1(backgroundColorEdit), b2(surfaceColorEdit), b3(borderColorEdit),
        b4(accentColorEdit), b5(primaryTextColorEdit), b6(secondaryTextColorEdit),
        b7(mutedTextColorEdit), b8(progressBarColorEdit);
    backgroundColorEdit->setText(settings.backgroundColor);
    surfaceColorEdit->setText(settings.surfaceColor);
    borderColorEdit->setText(settings.borderColor);
    accentColorEdit->setText(settings.accentColor);
    primaryTextColorEdit->setText(settings.primaryTextColor);
    secondaryTextColorEdit->setText(settings.secondaryTextColor);
    mutedTextColorEdit->setText(settings.mutedTextColor);
    progressBarColorEdit->setText(settings.progressBarColor);
    {
        const QSignalBlocker bw(overlayWidthSlider), bh(hideDurationSlider);
        overlayWidthSlider->setValue(settings.overlayWidth);
        hideDurationSlider->setValue(settings.hideDurationMs);
    }
    overlayWidthValue->setText(QString("%1 px").arg(settings.overlayWidth));
    hideDurationValue->setText(QString("%1 s").arg(settings.hideDurationMs / 1000.0, 0, 'f', 1));
    updateColorUi();
}

OverlaySettings SettingsDialog::overlaySettings() const {
    OverlaySettings settings;
    settings.backgroundColor = backgroundColorEdit->text().trimmed();
    settings.surfaceColor = surfaceColorEdit->text().trimmed();
    settings.borderColor = borderColorEdit->text().trimmed();
    settings.accentColor = accentColorEdit->text().trimmed();
    settings.primaryTextColor = primaryTextColorEdit->text().trimmed();
    settings.secondaryTextColor = secondaryTextColorEdit->text().trimmed();
    settings.mutedTextColor = mutedTextColorEdit->text().trimmed();
    settings.progressBarColor = progressBarColorEdit->text().trimmed();
    settings.overlayWidth = overlayWidthSlider->value();
    settings.hideDurationMs = hideDurationSlider->value();
    settings.presetName = presetName;
    return settings;
}

void SettingsDialog::setKeybindSettings(const KeybindSettings &settings) {
    keybindFineStep = settings.fineStep;
    keybindUseShift = settings.useShiftForFineAdjust;
    mainKeyButton->setKeyHex(settings.mainKey);
    likeKeyButton->setKeyHex(settings.likeKey);
    lockKeyButton->setKeyHex(settings.lockKey);
    showKeyButton->setKeyHex(settings.showKey);
    refreshMainKeyWarning();
    for (QPushButton *b : volumeStepButtons) {
        b->setChecked(b->property("stepValue").toInt() == settings.coarseStep);
    }
}

// The global hook consumes the main key on every press, with or without a
// modifier, so anything you'd normally type stops reaching other applications.
// Keys that exist to be repurposed (Caps Lock), or that nothing types with
// (F-keys, media keys), are fine and stay silent.
void SettingsDialog::refreshMainKeyWarning() {
    if (!mainKeyWarning) {
        return;
    }
    bool ok = false;
    const uint vk = mainKeyButton->keyHex().toUInt(&ok, 16);
    if (!ok) {
        mainKeyWarning->hide();
        return;
    }

    const bool isTypingKey =
        (vk >= 'A' && vk <= 'Z') ||       // letters
        (vk >= '0' && vk <= '9') ||       // top-row digits
        (vk >= 0x60 && vk <= 0x69) ||     // numpad digits
        vk == 0x20 ||                     // Space
        vk == 0x0D ||                     // Enter
        vk == 0x09 ||                     // Tab
        vk == 0x08 ||                     // Backspace
        (vk >= 0xBA && vk <= 0xC0) ||     // ;=,-./` punctuation
        (vk >= 0xDB && vk <= 0xDE);       // []\'" punctuation

    if (!isTypingKey) {
        mainKeyWarning->hide();
        return;
    }
    mainKeyWarning->setText(
        QStringLiteral("Heads up: Overtune swallows this key system-wide while it's running, so "
                       "it won't type in any other app. Caps Lock, an F-key or a media key is a "
                       "safer choice — you can change it back here at any time."));
    mainKeyWarning->show();
}

KeybindSettings SettingsDialog::keybindSettings() const {
    KeybindSettings settings;
    settings.coarseStep = 5;
    for (QPushButton *b : volumeStepButtons) {
        if (b->isChecked()) {
            settings.coarseStep = b->property("stepValue").toInt();
        }
    }
    settings.fineStep = keybindFineStep;
    settings.useShiftForFineAdjust = keybindUseShift;
    settings.mainKey = mainKeyButton->keyHex();
    settings.likeKey = likeKeyButton->keyHex();
    settings.lockKey = lockKeyButton->keyHex();
    settings.showKey = showKeyButton->keyHex();
    return settings;
}

void SettingsDialog::setQueueSettings(const QueueSettings &settings) {
    const QSignalBlocker bm(queueMaxSongsSpin), bo(queueOpacitySpin),
        bn(queueShowNowPlayingCheck), bl(queueShowLockIconCheck);
    queueMaxSongsSpin->setValue(settings.maxSongs);
    queueOpacitySpin->setValue(settings.opacityPercent);
    queueShowNowPlayingCheck->setChecked(settings.showNowPlaying);
    queueShowLockIconCheck->setChecked(settings.showLockIcon);
    queueEnabledState = settings.enabled;
    queueLockedState = settings.locked;
    queueHoverColorState = settings.hoverColor;
    queueWindowX = settings.windowX;
    queueWindowY = settings.windowY;
    queueWindowWidth = settings.windowWidth;
}

QueueSettings SettingsDialog::queueSettings() const {
    QueueSettings settings;
    settings.enabled = queueEnabledState;
    settings.showNowPlaying = queueShowNowPlayingCheck->isChecked();
    settings.locked = queueLockedState;
    settings.showLockIcon = queueShowLockIconCheck->isChecked();
    settings.maxSongs = queueMaxSongsSpin->value();
    settings.opacityPercent = queueOpacitySpin->value();
    settings.hoverColor = queueHoverColorState;
    settings.windowX = queueWindowX;
    settings.windowY = queueWindowY;
    settings.windowWidth = queueWindowWidth;
    return settings;
}

void SettingsDialog::wireOverlayControls() {
    const QList<QLineEdit *> edits = {backgroundColorEdit, surfaceColorEdit, borderColorEdit,
        accentColorEdit, primaryTextColorEdit, secondaryTextColorEdit, mutedTextColorEdit, progressBarColorEdit};
    for (QLineEdit *edit : edits) {
        connect(edit, &QLineEdit::textChanged, this, [this]() {
            presetName = QStringLiteral("Custom");
            updateColorUi();
            emit overlaySettingsChanged();
        });
    }
    connect(overlayWidthSlider, &QSlider::valueChanged, this, [this](int v) {
        overlayWidthValue->setText(QString("%1 px").arg(v));
        emit overlaySettingsChanged();
    });
    connect(hideDurationSlider, &QSlider::valueChanged, this, [this](int v) {
        hideDurationValue->setText(QString("%1 s").arg(v / 1000.0, 0, 'f', 1));
        emit overlaySettingsChanged();
    });
}

void SettingsDialog::wireKeybindControls() {
    mainKeyButton->onChanged = [this]() { refreshMainKeyWarning(); emit keybindSettingsChanged(); };
    likeKeyButton->onChanged = [this]() { emit keybindSettingsChanged(); };
    lockKeyButton->onChanged = [this]() { emit keybindSettingsChanged(); };
    showKeyButton->onChanged = [this]() { emit keybindSettingsChanged(); };
}

void SettingsDialog::wireQueueControls() {
    connect(queueShowNowPlayingCheck, &QCheckBox::toggled, this, &SettingsDialog::queueSettingsChanged);
    connect(queueShowLockIconCheck, &QCheckBox::toggled, this, &SettingsDialog::queueSettingsChanged);
    connect(queueMaxSongsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit queueSettingsChanged(); });
    connect(queueOpacitySpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit queueSettingsChanged(); });
}

void SettingsDialog::refreshUi() {
    const QString value = authenticated ? QStringLiteral("Connected") : QStringLiteral("Not connected");
    connectionValueLabel->setText(value);
    sidebarStatus->setText(value);
    sidebarDot->setStyleSheet(authenticated
        ? QString("background: %1; border-radius: 4px;").arg(theme::kAccent)
        : QString("background: %1; border-radius: 4px;").arg(theme::kNeutral600));

    connectButton->setEnabled(true);
    connectButton->setText(authenticated ? QStringLiteral("Reconnect Spotify") : QStringLiteral("Connect Spotify"));

    if (!authenticated) {
        helpTextLabel->setText(
            "Click <b>Connect Spotify</b> to authorize. Your browser opens a Spotify login page — "
            "approve the request, and playback state streams in realtime.");
    }
}
