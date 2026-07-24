#ifndef SETTINGS_DIALOG_H
#define SETTINGS_DIALOG_H

#include <QDialog>
#include "settings/app_settings.h"

class QLabel;
class QPushButton;
class QCheckBox;
class QLineEdit;
class QSpinBox;
class QTabWidget;

class QPlainTextEdit;

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

    // Show the device-flow verification URL and user code while auth is pending.
    void showAuthorizationPrompt(const QString &url, const QString &code);

signals:
    void connectSpotifyRequested();
    void overlaySettingsChanged();
    void keybindSettingsChanged();
    void queueSettingsChanged();

private:
    void refreshUi();
    void wireOverlayControls();
    void wireKeybindControls();
    void wireQueueControls();
    QWidget *createColorFieldRow(QLineEdit *edit, QLabel *preview, QWidget *parent = nullptr);
    void updateColorPreview(QLineEdit *edit, QLabel *preview);

    QLabel *connectionValueLabel;
    QLabel *helpTextLabel;
    QPushButton *connectButton;
    QTabWidget *tabs;
    QLineEdit *backgroundColorEdit;
    QLabel *backgroundColorPreview;
    QLineEdit *borderColorEdit;
    QLabel *borderColorPreview;
    QLineEdit *accentColorEdit;
    QLabel *accentColorPreview;
    QLineEdit *primaryTextColorEdit;
    QLabel *primaryTextColorPreview;
    QLineEdit *secondaryTextColorEdit;
    QLabel *secondaryTextColorPreview;
    QLineEdit *mutedTextColorEdit;
    QLabel *mutedTextColorPreview;
    QLineEdit *progressBarColorEdit;
    QLabel *progressBarColorPreview;
    QSpinBox *overlayWidthSpin;
    QSpinBox *hideDurationSpin;
    QSpinBox *coarseStepSpin;
    QSpinBox *fineStepSpin;
    QCheckBox *useShiftFineAdjustCheck;
    QLineEdit *mainKeyEdit;
    QCheckBox *queueEnabledCheck;
    QCheckBox *queueShowNowPlayingCheck;
    QCheckBox *queueLockedCheck;
    QCheckBox *queueShowLockIconCheck;
    QSpinBox *queueMaxSongsSpin;
    QSpinBox *queueOpacitySpin;
    QLineEdit *queueHoverColorEdit;
    QLabel *queueHoverColorPreview;

    bool authenticated = false;
    // Position/size aren't edited in the dialog; carried through so a settings
    // round-trip doesn't wipe the remembered window spot.
    int queueWindowX = INT_MIN;
    int queueWindowY = INT_MIN;
    int queueWindowWidth = -1;

    QPlainTextEdit *logViewer = nullptr;
};

#endif // SETTINGS_DIALOG_H
