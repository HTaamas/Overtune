#include "settings_dialog.h"
#include "spotify/spotify_client.h"
#include "key_capture_button.h"

#include <QCheckBox>
#include <QColor>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QPlainTextEdit>
#include <cmath>

namespace {
// WCAG relative luminance + contrast ratio, for warning about unreadable
// user-chosen text colors.
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

// Fixed dark theme for the settings window, so it matches the overlays instead
// of the plain system widget style. Independent of the (user-themeable)
// overlay colors.
constexpr auto kSettingsStyle = R"(
QDialog { background-color: #181818; }
QWidget { color: #e8e8e8; font-size: 12px; }
QLabel { color: #cfcfcf; background: transparent; }
QTabWidget::pane { border: 1px solid #303030; border-radius: 8px; top: -1px; background: #1e1e1e; }
QTabBar::tab {
    background: transparent; color: #9a9a9a; padding: 7px 16px; margin-right: 2px;
    border-top-left-radius: 6px; border-top-right-radius: 6px;
}
QTabBar::tab:hover { color: #e8e8e8; }
QTabBar::tab:selected { background: #1e1e1e; color: #1DB954; border-bottom: 2px solid #1DB954; }
QLineEdit, QSpinBox {
    background: #2a2a2a; border: 1px solid #3a3a3a; border-radius: 6px;
    padding: 5px 8px; color: #f0f0f0; selection-background-color: #1DB954;
}
QLineEdit:focus, QSpinBox:focus { border: 1px solid #1DB954; }
QPushButton {
    background: #2a2a2a; border: 1px solid #3a3a3a; border-radius: 6px;
    padding: 6px 14px; color: #f0f0f0;
}
QPushButton:hover { background: #333333; border-color: #4a4a4a; }
QPushButton:pressed, QPushButton:checked { background: #1DB954; border-color: #1DB954; color: #06130b; }
QCheckBox { spacing: 8px; color: #d8d8d8; background: transparent; }
QCheckBox::indicator {
    width: 16px; height: 16px; border-radius: 4px;
    border: 1px solid #4a4a4a; background: #2a2a2a;
}
QCheckBox::indicator:checked { background: #1DB954; border-color: #1DB954; }
QDialogButtonBox QPushButton { min-width: 72px; }
)";

QWidget *createRow(const QString &labelText, QWidget *fieldWidget, QWidget *parent = nullptr) {
    QWidget *row = new QWidget(parent);
    QHBoxLayout *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    QLabel *label = new QLabel(labelText, row);
    label->setMinimumWidth(120);
    if (QLabel *textLabel = qobject_cast<QLabel *>(fieldWidget)) {
        textLabel->setWordWrap(true);
    }

    layout->addWidget(label);
    layout->addWidget(fieldWidget, 1);
    return row;
}

QLabel *createColorPreview(QWidget *parent = nullptr) {
    QLabel *preview = new QLabel(parent);
    preview->setFixedSize(28, 20);
    preview->setFrameShape(QFrame::StyledPanel);
    preview->setFrameShadow(QFrame::Sunken);
    return preview;
}
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Overtune Settings");
    setModal(false);
    resize(520, 470);
    setStyleSheet(kSettingsStyle);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);

    // Section-heading font. The window title bar already names the dialog, so
    // there is no redundant in-window "Overtune Settings" heading.
    QFont titleFont = font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setBold(true);

    tabs = new QTabWidget(this);

    QWidget *spotifyTab = new QWidget(this);
    QVBoxLayout *spotifyLayout = new QVBoxLayout(spotifyTab);
    spotifyLayout->setContentsMargins(14, 14, 14, 14);
    spotifyLayout->setSpacing(10);

    QLabel *spotifyTitle = new QLabel("Spotify Connection", spotifyTab);
    spotifyTitle->setFont(titleFont);
    spotifyTitle->setStyleSheet("color: #f0f0f0; background: transparent;");
    spotifyLayout->addWidget(spotifyTitle);

    connectionValueLabel = new QLabel(this);
    helpTextLabel = new QLabel(this);
    helpTextLabel->setWordWrap(true);

    spotifyLayout->addWidget(createRow("Status", connectionValueLabel, spotifyTab));
    spotifyLayout->addWidget(helpTextLabel);
    spotifyLayout->addSpacing(2);

    connectButton = new QPushButton(this);
    // Primary call to action: accent-filled to stand out from other buttons.
    connectButton->setStyleSheet(
        "QPushButton { background: #1DB954; border: none; border-radius: 6px;"
        " padding: 7px 18px; color: #06130b; font-weight: bold; }"
        "QPushButton:hover { background: #1ed760; }"
        "QPushButton:pressed { background: #17a349; }");
    connect(connectButton, &QPushButton::clicked, this, &SettingsDialog::connectSpotifyRequested);
    spotifyLayout->addWidget(connectButton, 0, Qt::AlignLeft);

    logViewer = new QPlainTextEdit(spotifyTab);
    logViewer->setReadOnly(true);
    logViewer->setMinimumHeight(120);
    logViewer->setStyleSheet("QPlainTextEdit { background-color: #101010; color: #b9b9b9; font-family: monospace; font-size: 10px; border: 1px solid #303030; border-radius: 6px; padding: 6px; }");
    spotifyLayout->addWidget(new QLabel("Connection log:", spotifyTab));
    // Let the log fill the remaining space so the tab reads as full, not empty.
    spotifyLayout->addWidget(logViewer, 1);
    tabs->addTab(spotifyTab, "Spotify");

    QWidget *overlayTab = new QWidget(this);
    QFormLayout *overlayLayout = new QFormLayout(overlayTab);
    overlayLayout->setContentsMargins(12, 12, 12, 12);
    overlayLayout->setSpacing(10);

    backgroundColorEdit = new QLineEdit(this);
    backgroundColorPreview = createColorPreview(this);
    borderColorEdit = new QLineEdit(this);
    borderColorPreview = createColorPreview(this);
    accentColorEdit = new QLineEdit(this);
    accentColorPreview = createColorPreview(this);
    primaryTextColorEdit = new QLineEdit(this);
    primaryTextColorPreview = createColorPreview(this);
    secondaryTextColorEdit = new QLineEdit(this);
    secondaryTextColorPreview = createColorPreview(this);
    mutedTextColorEdit = new QLineEdit(this);
    mutedTextColorPreview = createColorPreview(this);
    progressBarColorEdit = new QLineEdit(this);
    progressBarColorPreview = createColorPreview(this);
    overlayWidthSpin = new QSpinBox(this);
    overlayWidthSpin->setRange(320, 900);
    hideDurationSpin = new QSpinBox(this);
    hideDurationSpin->setRange(1000, 15000);
    hideDurationSpin->setSuffix(" ms");

    overlayLayout->addRow("Background", createColorFieldRow(backgroundColorEdit, backgroundColorPreview, overlayTab));
    overlayLayout->addRow("Border", createColorFieldRow(borderColorEdit, borderColorPreview, overlayTab));
    overlayLayout->addRow("Accent", createColorFieldRow(accentColorEdit, accentColorPreview, overlayTab));
    overlayLayout->addRow("Primary text", createColorFieldRow(primaryTextColorEdit, primaryTextColorPreview, overlayTab));
    overlayLayout->addRow("Secondary text", createColorFieldRow(secondaryTextColorEdit, secondaryTextColorPreview, overlayTab));
    overlayLayout->addRow("Muted text", createColorFieldRow(mutedTextColorEdit, mutedTextColorPreview, overlayTab));
    overlayLayout->addRow("Progress bar", createColorFieldRow(progressBarColorEdit, progressBarColorPreview, overlayTab));
    overlayLayout->addRow("Overlay width", overlayWidthSpin);
    overlayLayout->addRow("Hide delay", hideDurationSpin);
    tabs->addTab(overlayTab, "Overlay");

    QWidget *queueTab = new QWidget(this);
    QFormLayout *queueLayout = new QFormLayout(queueTab);
    queueLayout->setContentsMargins(12, 12, 12, 12);
    queueLayout->setSpacing(10);

    queueEnabledCheck = new QCheckBox("Show the Up Next window", this);
    queueShowNowPlayingCheck = new QCheckBox("Show the current song at the top", this);
    queueLockedCheck = new QCheckBox("Lock the window (click-through)", this);
    queueShowLockIconCheck = new QCheckBox("Show a lock icon while locked", this);
    queueMaxSongsSpin = new QSpinBox(this);
    queueMaxSongsSpin->setRange(1, 30);
    queueMaxSongsSpin->setSuffix(" songs");
    queueOpacitySpin = new QSpinBox(this);
    queueOpacitySpin->setRange(20, 100);
    queueOpacitySpin->setSuffix("%");
    queueHoverColorEdit = new QLineEdit(this);
    queueHoverColorPreview = createColorPreview(this);
    QLabel *queueHint = new QLabel("The Up Next window stays on top and never auto-hides. Drag it anywhere with the mouse, resize it from the left/right edge — position and size are remembered. Locking makes it click-through so it can't be moved or block clicks. Global shortcuts (configurable in Keybinds): Alt+L toggles the lock, Alt+U shows/hides the window.", this);
    queueHint->setWordWrap(true);

    queueLayout->addRow(QString(), queueEnabledCheck);
    queueLayout->addRow(QString(), queueShowNowPlayingCheck);
    queueLayout->addRow(QString(), queueLockedCheck);
    queueLayout->addRow(QString(), queueShowLockIconCheck);
    queueLayout->addRow("Songs shown", queueMaxSongsSpin);
    queueLayout->addRow("Opacity", queueOpacitySpin);
    queueLayout->addRow("Hover color", createColorFieldRow(queueHoverColorEdit, queueHoverColorPreview, queueTab));
    queueLayout->addRow(QString(), queueHint);
    tabs->addTab(queueTab, "Up Next");

    QWidget *keybindsTab = new QWidget(this);
    QFormLayout *keybindsLayout = new QFormLayout(keybindsTab);
    keybindsLayout->setContentsMargins(12, 12, 12, 12);
    keybindsLayout->setSpacing(10);

    coarseStepSpin = new QSpinBox(this);
    coarseStepSpin->setRange(1, 25);
    fineStepSpin = new QSpinBox(this);
    fineStepSpin->setRange(1, 25);
    mainKeyButton = new KeyCaptureButton(this);
    likeKeyButton = new KeyCaptureButton(this);
    lockKeyButton = new KeyCaptureButton(this);
    showKeyButton = new KeyCaptureButton(this);
    useShiftFineAdjustCheck = new QCheckBox("Use Shift for fine adjustment", this);
    QLabel *mainKeyHint = new QLabel("Click a key field, then press the key you want (or right-click to type a code by hand). The main key toggles volume control; hold Shift with it to Skip, or Ctrl for Previous. (Caps Lock is the default and works on macOS too.)", this);
    mainKeyHint->setWordWrap(true);

    keybindsLayout->addRow("Coarse step", coarseStepSpin);
    keybindsLayout->addRow("Fine step", fineStepSpin);
    keybindsLayout->addRow("Main key", mainKeyButton);
    keybindsLayout->addRow("Like key (with Alt)", likeKeyButton);
    keybindsLayout->addRow("Lock window key (with Alt)", lockKeyButton);
    keybindsLayout->addRow("Show/hide window key (with Alt)", showKeyButton);
    keybindsLayout->addRow(QString(), useShiftFineAdjustCheck);
    keybindsLayout->addRow(QString(), mainKeyHint);
    QLabel *comboKeyHint = new QLabel("Hold Alt with these keys: the like key saves the current song (default S); the lock key toggles the Up Next window's click-through lock (default L); the show/hide key toggles the window (default U).", this);
    comboKeyHint->setWordWrap(true);
    keybindsLayout->addRow(QString(), comboKeyHint);

#ifdef _WIN32
    runAsAdminCheck = new QCheckBox("Run as administrator", this);
    runAsAdminCheck->setChecked(AppSettings::loadRunAsAdmin());
    connect(runAsAdminCheck, &QCheckBox::toggled, this, [](bool on) { AppSettings::saveRunAsAdmin(on); });
    keybindsLayout->addRow(QString(), runAsAdminCheck);
    QLabel *adminHint = new QLabel("Off by default — the app needs no admin rights. Enable this only if you want the hotkeys to work while a program running as administrator has focus (some games/anti-cheat). Takes effect on the next launch.", this);
    adminHint->setWordWrap(true);
    keybindsLayout->addRow(QString(), adminHint);
#endif

    tabs->addTab(keybindsTab, "Keybinds");

    layout->addWidget(tabs);

    wireOverlayControls();
    wireKeybindControls();
    wireQueueControls();

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::hide);
    layout->addWidget(buttonBox);

    refreshUi();
}

void SettingsDialog::showAuthorizationPrompt(const QString &url, const QString &code) {
    connectionValueLabel->setText("Waiting for authorization...");
    if (code.isEmpty()) {
        helpTextLabel->setText(
            QString("<b>Authorize Overtune</b><br>"
                    "A browser window was opened. Approve access and you'll be connected automatically.<br>"
                    "If it didn't open, <a href=\"%1\">click here</a>.")
                .arg(url));
    } else {
        helpTextLabel->setText(
            QString("<b>Authorize Overtune</b><br>"
                    "A browser window was opened to <a href=\"%1\">%1</a>.<br>"
                    "If it didn't open, visit that link and confirm the code <b>%2</b>.")
                .arg(url, code));
    }
    helpTextLabel->setOpenExternalLinks(true);
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
    backgroundColorEdit->setText(settings.backgroundColor);
    updateColorPreview(backgroundColorEdit, backgroundColorPreview);
    borderColorEdit->setText(settings.borderColor);
    updateColorPreview(borderColorEdit, borderColorPreview);
    accentColorEdit->setText(settings.accentColor);
    updateColorPreview(accentColorEdit, accentColorPreview);
    primaryTextColorEdit->setText(settings.primaryTextColor);
    updateColorPreview(primaryTextColorEdit, primaryTextColorPreview, /*checkTextContrast=*/true);
    secondaryTextColorEdit->setText(settings.secondaryTextColor);
    updateColorPreview(secondaryTextColorEdit, secondaryTextColorPreview, /*checkTextContrast=*/true);
    mutedTextColorEdit->setText(settings.mutedTextColor);
    updateColorPreview(mutedTextColorEdit, mutedTextColorPreview, /*checkTextContrast=*/true);
    progressBarColorEdit->setText(settings.progressBarColor);
    updateColorPreview(progressBarColorEdit, progressBarColorPreview);
    overlayWidthSpin->setValue(settings.overlayWidth);
    hideDurationSpin->setValue(settings.hideDurationMs);
}

OverlaySettings SettingsDialog::overlaySettings() const {
    OverlaySettings settings;
    settings.backgroundColor = backgroundColorEdit->text().trimmed();
    settings.borderColor = borderColorEdit->text().trimmed();
    settings.accentColor = accentColorEdit->text().trimmed();
    settings.primaryTextColor = primaryTextColorEdit->text().trimmed();
    settings.secondaryTextColor = secondaryTextColorEdit->text().trimmed();
    settings.mutedTextColor = mutedTextColorEdit->text().trimmed();
    settings.progressBarColor = progressBarColorEdit->text().trimmed();
    settings.overlayWidth = overlayWidthSpin->value();
    settings.hideDurationMs = hideDurationSpin->value();
    return settings;
}

void SettingsDialog::setKeybindSettings(const KeybindSettings &settings) {
    coarseStepSpin->setValue(settings.coarseStep);
    fineStepSpin->setValue(settings.fineStep);
    mainKeyButton->setKeyHex(settings.mainKey);
    likeKeyButton->setKeyHex(settings.likeKey);
    lockKeyButton->setKeyHex(settings.lockKey);
    showKeyButton->setKeyHex(settings.showKey);
    useShiftFineAdjustCheck->setChecked(settings.useShiftForFineAdjust);
}

KeybindSettings SettingsDialog::keybindSettings() const {
    KeybindSettings settings;
    settings.coarseStep = coarseStepSpin->value();
    settings.fineStep = fineStepSpin->value();
    settings.mainKey = mainKeyButton->keyHex();
    settings.likeKey = likeKeyButton->keyHex();
    settings.lockKey = lockKeyButton->keyHex();
    settings.showKey = showKeyButton->keyHex();
    settings.useShiftForFineAdjust = useShiftFineAdjustCheck->isChecked();
    return settings;
}

void SettingsDialog::setQueueSettings(const QueueSettings &settings) {
    queueEnabledCheck->setChecked(settings.enabled);
    queueShowNowPlayingCheck->setChecked(settings.showNowPlaying);
    queueLockedCheck->setChecked(settings.locked);
    queueShowLockIconCheck->setChecked(settings.showLockIcon);
    queueMaxSongsSpin->setValue(settings.maxSongs);
    queueOpacitySpin->setValue(settings.opacityPercent);
    queueHoverColorEdit->setText(settings.hoverColor);
    updateColorPreview(queueHoverColorEdit, queueHoverColorPreview);
    queueWindowX = settings.windowX;
    queueWindowY = settings.windowY;
    queueWindowWidth = settings.windowWidth;
}

QueueSettings SettingsDialog::queueSettings() const {
    QueueSettings settings;
    settings.enabled = queueEnabledCheck->isChecked();
    settings.showNowPlaying = queueShowNowPlayingCheck->isChecked();
    settings.locked = queueLockedCheck->isChecked();
    settings.showLockIcon = queueShowLockIconCheck->isChecked();
    settings.maxSongs = queueMaxSongsSpin->value();
    settings.opacityPercent = queueOpacitySpin->value();
    settings.hoverColor = queueHoverColorEdit->text().trimmed();
    settings.windowX = queueWindowX;
    settings.windowY = queueWindowY;
    settings.windowWidth = queueWindowWidth;
    return settings;
}

void SettingsDialog::wireOverlayControls() {
    auto refreshTextContrastPreviews = [this]() {
        updateColorPreview(primaryTextColorEdit, primaryTextColorPreview, /*checkTextContrast=*/true);
        updateColorPreview(secondaryTextColorEdit, secondaryTextColorPreview, /*checkTextContrast=*/true);
        updateColorPreview(mutedTextColorEdit, mutedTextColorPreview, /*checkTextContrast=*/true);
    };
    // Changing the background re-scores every text color's contrast against it.
    connect(backgroundColorEdit, &QLineEdit::textChanged, this, [this, refreshTextContrastPreviews](const QString &) { updateColorPreview(backgroundColorEdit, backgroundColorPreview); refreshTextContrastPreviews(); emit overlaySettingsChanged(); });
    connect(borderColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(borderColorEdit, borderColorPreview); emit overlaySettingsChanged(); });
    connect(accentColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(accentColorEdit, accentColorPreview); emit overlaySettingsChanged(); });
    connect(primaryTextColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(primaryTextColorEdit, primaryTextColorPreview, true); emit overlaySettingsChanged(); });
    connect(secondaryTextColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(secondaryTextColorEdit, secondaryTextColorPreview, true); emit overlaySettingsChanged(); });
    connect(mutedTextColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(mutedTextColorEdit, mutedTextColorPreview, true); emit overlaySettingsChanged(); });
    connect(progressBarColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(progressBarColorEdit, progressBarColorPreview); emit overlaySettingsChanged(); });
    connect(overlayWidthSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit overlaySettingsChanged(); });
    connect(hideDurationSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit overlaySettingsChanged(); });
}

void SettingsDialog::wireKeybindControls() {
    connect(coarseStepSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit keybindSettingsChanged(); });
    connect(fineStepSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit keybindSettingsChanged(); });
    mainKeyButton->onChanged = [this]() { emit keybindSettingsChanged(); };
    likeKeyButton->onChanged = [this]() { emit keybindSettingsChanged(); };
    lockKeyButton->onChanged = [this]() { emit keybindSettingsChanged(); };
    showKeyButton->onChanged = [this]() { emit keybindSettingsChanged(); };
    connect(useShiftFineAdjustCheck, &QCheckBox::toggled, this, &SettingsDialog::keybindSettingsChanged);
}

void SettingsDialog::wireQueueControls() {
    connect(queueEnabledCheck, &QCheckBox::toggled, this, &SettingsDialog::queueSettingsChanged);
    connect(queueShowNowPlayingCheck, &QCheckBox::toggled, this, &SettingsDialog::queueSettingsChanged);
    connect(queueLockedCheck, &QCheckBox::toggled, this, &SettingsDialog::queueSettingsChanged);
    connect(queueShowLockIconCheck, &QCheckBox::toggled, this, &SettingsDialog::queueSettingsChanged);
    connect(queueMaxSongsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit queueSettingsChanged(); });
    connect(queueOpacitySpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit queueSettingsChanged(); });
    connect(queueHoverColorEdit, &QLineEdit::textChanged, this, [this](const QString &) {
        updateColorPreview(queueHoverColorEdit, queueHoverColorPreview);
        emit queueSettingsChanged();
    });
}

QWidget *SettingsDialog::createColorFieldRow(QLineEdit *edit, QLabel *preview, QWidget *parent) {
    QWidget *row = new QWidget(parent);
    QHBoxLayout *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(edit, 1);
    layout->addWidget(preview, 0);
    return row;
}

void SettingsDialog::updateColorPreview(QLineEdit *edit, QLabel *preview, bool checkTextContrast) {
    const QColor color(edit->text().trimmed());
    if (!color.isValid()) {
        preview->setStyleSheet("background-color: #202020; border: 1px solid #aa5555; border-radius: 4px;");
        preview->setToolTip("Not a valid color");
        return;
    }

    // Warn when a text color won't be readable on the chosen background.
    if (checkTextContrast) {
        const QColor bg(backgroundColorEdit->text().trimmed());
        if (bg.isValid()) {
            const double ratio = contrastRatio(color, bg);
            if (ratio < 4.5) {
                preview->setStyleSheet(QString("background-color: %1; border: 2px solid #e0a021; border-radius: 4px;").arg(color.name()));
                preview->setToolTip(QString("Low contrast on the background (%1:1). Aim for 4.5:1 or higher for readable text.")
                                        .arg(ratio, 0, 'f', 1));
                return;
            }
            preview->setToolTip(QString("Contrast on the background: %1:1 (good)").arg(ratio, 0, 'f', 1));
        }
    } else {
        preview->setToolTip(color.name());
    }
    preview->setStyleSheet(QString("background-color: %1; border: 1px solid #555555; border-radius: 4px;").arg(color.name()));
}

void SettingsDialog::refreshUi() {
    connectionValueLabel->setText(authenticated ? "Connected" : "Not connected");

    connectButton->setEnabled(true);
    connectButton->setText(authenticated ? "Reconnect Spotify" : "Connect Spotify");

    if (!authenticated) {
        helpTextLabel->setText(
            "Click <b>Connect Spotify</b> to authorize. Your browser will open a Spotify "
            "login page — approve the request, and playback state will start streaming in "
            "realtime. No password or cookie is ever handled by this app.");
        helpTextLabel->setOpenExternalLinks(true);
    }
}
