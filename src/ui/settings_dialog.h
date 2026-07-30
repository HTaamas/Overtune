#ifndef SETTINGS_DIALOG_H
#define SETTINGS_DIALOG_H

#include <QDialog>
#include <QList>
#include "settings/app_settings.h"

class QLabel;
class QPushButton;
class QCheckBox;
class QLineEdit;
class QSpinBox;
class QSlider;
class QStackedWidget;
class QPlainTextEdit;
class KeyCaptureButton;
class OverlayPreview;

// One editable overlay colour: the hex field plus its swatch row and (for text
// roles) the WCAG contrast tag.
struct ColorRole;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    void setAuthenticated(bool authenticated);
    void appendLog(const QString &text);
    void setOverlaySettings(const OverlaySettings &settings);
    OverlaySettings overlaySettings() const;
    void setKeybindSettings(const KeybindSettings &settings);
    KeybindSettings keybindSettings() const;
    void setQueueSettings(const QueueSettings &settings);
    QueueSettings queueSettings() const;

    void showAuthorizationPrompt(const QString &url, const QString &code);

signals:
    void connectSpotifyRequested();
    void overlaySettingsChanged();
    void keybindSettingsChanged();
    void queueSettingsChanged();

private:
    QWidget *buildSidebar();
    QWidget *buildConnectionPage();
    QWidget *buildOverlayPage();
    QWidget *buildUpNextPage();
    QWidget *buildKeybindsPage();
    void selectSection(int index);
    void refreshUi();
    void wireOverlayControls();
    void wireKeybindControls();
    void wireQueueControls();

    // Overlay editing helpers
    QWidget *makeColorRow(const QString &label, QLineEdit *edit, const QStringList &ramp, bool textRole);
    void applyPreset(int index);
    void refreshPresetSelection();
    void updateColorUi();          // swatches + contrast tags after any change
    void setDisclosureExpanded(bool expanded);

    QLineEdit *colorEditFor(const QString &role) const;

    // --- sidebar ---
    QList<QPushButton *> navButtons;
    QStackedWidget *stack = nullptr;
    QLabel *sidebarDot = nullptr;
    QLabel *sidebarStatus = nullptr;

    // --- connection page ---
    QLabel *connectionValueLabel = nullptr;
    QLabel *helpTextLabel = nullptr;
    QPushButton *connectButton = nullptr;
    QPlainTextEdit *logViewer = nullptr;

    // --- overlay page ---
    QList<QWidget *> presetCards;
    QList<QLabel *> presetTags;
    QPushButton *disclosureButton = nullptr;
    QWidget *customiseBox = nullptr;
    bool disclosureExpanded = false;
    QLineEdit *backgroundColorEdit = nullptr;
    QLineEdit *surfaceColorEdit = nullptr;
    QLineEdit *borderColorEdit = nullptr;
    QLineEdit *accentColorEdit = nullptr;
    QLineEdit *primaryTextColorEdit = nullptr;
    QLineEdit *secondaryTextColorEdit = nullptr;
    QLineEdit *mutedTextColorEdit = nullptr;
    QLineEdit *progressBarColorEdit = nullptr;
    QList<QList<QWidget *>> swatchGroups; // swatches per role row, for selection ring
    QList<QLineEdit *> swatchRoleEdits;   // the edit each swatch group drives (parallel)
    QLabel *primaryContrastTag = nullptr;
    QLabel *secondaryContrastTag = nullptr;
    QLabel *mutedContrastTag = nullptr;
    QSlider *overlayWidthSlider = nullptr;
    QLabel *overlayWidthValue = nullptr;
    QSlider *hideDurationSlider = nullptr;
    QLabel *hideDurationValue = nullptr;
    OverlayPreview *preview = nullptr;

    // --- up next page ---
    QSpinBox *queueMaxSongsSpin = nullptr;
    QSpinBox *queueOpacitySpin = nullptr;
    QCheckBox *queueShowNowPlayingCheck = nullptr;
    QCheckBox *queueShowLockIconCheck = nullptr;

    // --- keybinds page ---
    KeyCaptureButton *mainKeyButton = nullptr;
    KeyCaptureButton *likeKeyButton = nullptr;
    KeyCaptureButton *lockKeyButton = nullptr;
    KeyCaptureButton *showKeyButton = nullptr;
    QList<QPushButton *> volumeStepButtons; // segmented: 5 / 2 / 10
#ifdef _WIN32
    QCheckBox *runAsAdminCheck = nullptr;
#endif

    bool authenticated = false;
    QString presetName = "Nocturne";
    // Carried through unchanged (not edited on this screen): the queue window's
    // position/size, its enabled/locked state (toggled in the tray) and hover
    // colour, so a settings round-trip doesn't wipe them.
    bool queueEnabledState = false;
    bool queueLockedState = false;
    QString queueHoverColorState = "#e9e9ed";
    int queueWindowX = INT_MIN;
    int queueWindowY = INT_MIN;
    int queueWindowWidth = -1;
    int keybindFineStep = 1;
    bool keybindUseShift = true;
};

#endif // SETTINGS_DIALOG_H
