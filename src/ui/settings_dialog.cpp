#include "settings_dialog.h"
#include "spotify/spotify_client.h"

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

namespace {
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
    setWindowTitle("SpotifyVol Settings");
    setModal(false);
    resize(520, 470);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);

    QLabel *titleLabel = new QLabel("SpotifyVol Settings", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    tabs = new QTabWidget(this);

    QWidget *spotifyTab = new QWidget(this);
    QVBoxLayout *spotifyLayout = new QVBoxLayout(spotifyTab);
    spotifyLayout->setContentsMargins(12, 12, 12, 12);
    spotifyLayout->setSpacing(12);

    QLabel *spotifyTitle = new QLabel("Spotify Connection", spotifyTab);
    spotifyTitle->setFont(titleFont);
    spotifyLayout->addWidget(spotifyTitle);

    connectionValueLabel = new QLabel(this);
    helpTextLabel = new QLabel(this);
    helpTextLabel->setWordWrap(true);

    spotifyLayout->addWidget(createRow("Status", connectionValueLabel, spotifyTab));
    spotifyLayout->addWidget(helpTextLabel);

    connectButton = new QPushButton(this);
    connect(connectButton, &QPushButton::clicked, this, &SettingsDialog::connectSpotifyRequested);
    spotifyLayout->addWidget(connectButton, 0, Qt::AlignLeft);

    logViewer = new QPlainTextEdit(spotifyTab);
    logViewer->setReadOnly(true);
    logViewer->setFixedHeight(140);
    logViewer->setStyleSheet("QPlainTextEdit { background-color: #2b2b2b; color: #dcdcdc; font-family: monospace; font-size: 10px; border: 1px solid #3c3c3c; border-radius: 4px; padding: 4px; }");
    spotifyLayout->addWidget(new QLabel("WebSocket Connection Logs:", spotifyTab));
    spotifyLayout->addWidget(logViewer);
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
    QLabel *queueHint = new QLabel("The Up Next window stays on top and never auto-hides. Drag it anywhere with the mouse, resize it from the left/right edge — position and size are remembered. Locking makes it click-through so it can't be moved or block clicks; toggle the lock any time with Alt+U.", this);
    queueHint->setWordWrap(true);

    queueLayout->addRow(QString(), queueEnabledCheck);
    queueLayout->addRow(QString(), queueShowNowPlayingCheck);
    queueLayout->addRow(QString(), queueLockedCheck);
    queueLayout->addRow(QString(), queueShowLockIconCheck);
    queueLayout->addRow("Songs shown", queueMaxSongsSpin);
    queueLayout->addRow("Opacity", queueOpacitySpin);
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
    mainKeyEdit = new QLineEdit(this);
    useShiftFineAdjustCheck = new QCheckBox("Use Shift for fine adjustment", this);
    QLabel *mainKeyHint = new QLabel("Main key is a virtual key code (VK) value, e.g. 0x14 for Caps Lock (default) or 0x41 for 'A'. On macOS it is translated to the matching mac key (Caps Lock is supported).\nHold Shift with the main key to Skip, or Ctrl for Previous.", this);
    mainKeyHint->setWordWrap(true);

    keybindsLayout->addRow("Coarse step", coarseStepSpin);
    keybindsLayout->addRow("Fine step", fineStepSpin);
    keybindsLayout->addRow("Main key (VK)", mainKeyEdit);
    keybindsLayout->addRow(QString(), useShiftFineAdjustCheck);
    keybindsLayout->addRow(QString(), mainKeyHint);
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
    helpTextLabel->setText(
        QString("<b>Authorize SpotifyVol</b><br>"
                "A browser window was opened to <a href=\"%1\">%1</a>.<br>"
                "If it didn't open, visit that link and confirm the code <b>%2</b>.")
            .arg(url, code));
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
    updateColorPreview(primaryTextColorEdit, primaryTextColorPreview);
    secondaryTextColorEdit->setText(settings.secondaryTextColor);
    updateColorPreview(secondaryTextColorEdit, secondaryTextColorPreview);
    mutedTextColorEdit->setText(settings.mutedTextColor);
    updateColorPreview(mutedTextColorEdit, mutedTextColorPreview);
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
    mainKeyEdit->setText(settings.mainKey);
    useShiftFineAdjustCheck->setChecked(settings.useShiftForFineAdjust);
}

KeybindSettings SettingsDialog::keybindSettings() const {
    KeybindSettings settings;
    settings.coarseStep = coarseStepSpin->value();
    settings.fineStep = fineStepSpin->value();
    settings.mainKey = mainKeyEdit->text().trimmed();
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
    settings.windowX = queueWindowX;
    settings.windowY = queueWindowY;
    settings.windowWidth = queueWindowWidth;
    return settings;
}

void SettingsDialog::wireOverlayControls() {
    connect(backgroundColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(backgroundColorEdit, backgroundColorPreview); emit overlaySettingsChanged(); });
    connect(borderColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(borderColorEdit, borderColorPreview); emit overlaySettingsChanged(); });
    connect(accentColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(accentColorEdit, accentColorPreview); emit overlaySettingsChanged(); });
    connect(primaryTextColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(primaryTextColorEdit, primaryTextColorPreview); emit overlaySettingsChanged(); });
    connect(secondaryTextColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(secondaryTextColorEdit, secondaryTextColorPreview); emit overlaySettingsChanged(); });
    connect(mutedTextColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(mutedTextColorEdit, mutedTextColorPreview); emit overlaySettingsChanged(); });
    connect(progressBarColorEdit, &QLineEdit::textChanged, this, [this](const QString &) { updateColorPreview(progressBarColorEdit, progressBarColorPreview); emit overlaySettingsChanged(); });
    connect(overlayWidthSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit overlaySettingsChanged(); });
    connect(hideDurationSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit overlaySettingsChanged(); });
}

void SettingsDialog::wireKeybindControls() {
    connect(coarseStepSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit keybindSettingsChanged(); });
    connect(fineStepSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit keybindSettingsChanged(); });
    connect(mainKeyEdit, &QLineEdit::textChanged, this, &SettingsDialog::keybindSettingsChanged);
    connect(useShiftFineAdjustCheck, &QCheckBox::toggled, this, &SettingsDialog::keybindSettingsChanged);
}

void SettingsDialog::wireQueueControls() {
    connect(queueEnabledCheck, &QCheckBox::toggled, this, &SettingsDialog::queueSettingsChanged);
    connect(queueShowNowPlayingCheck, &QCheckBox::toggled, this, &SettingsDialog::queueSettingsChanged);
    connect(queueLockedCheck, &QCheckBox::toggled, this, &SettingsDialog::queueSettingsChanged);
    connect(queueShowLockIconCheck, &QCheckBox::toggled, this, &SettingsDialog::queueSettingsChanged);
    connect(queueMaxSongsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit queueSettingsChanged(); });
    connect(queueOpacitySpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit queueSettingsChanged(); });
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

void SettingsDialog::updateColorPreview(QLineEdit *edit, QLabel *preview) {
    const QColor color(edit->text().trimmed());
    if (color.isValid()) {
        preview->setStyleSheet(QString("background-color: %1; border: 1px solid #555555; border-radius: 4px;").arg(color.name()));
    } else {
        preview->setStyleSheet("background-color: #202020; border: 1px solid #aa5555; border-radius: 4px;");
    }
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
