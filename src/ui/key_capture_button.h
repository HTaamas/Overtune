#ifndef KEY_CAPTURE_BUTTON_H
#define KEY_CAPTURE_BUTTON_H

#include <QPushButton>
#include <QKeyEvent>
#include <functional>

// A button that captures a single physical key press and stores its virtual
// key code (as a "0xNN" hex string, matching the format the keybind system
// expects). It shows a friendly name — "Caps Lock", "A" — instead of forcing
// the user to know raw VK codes. Click it, then press any key.
class KeyCaptureButton : public QPushButton {
public:
    explicit KeyCaptureButton(QWidget *parent = nullptr) : QPushButton(parent) {
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        connect(this, &QPushButton::clicked, this, [this](bool checked) {
            capturing = checked;
            refreshText();
            if (capturing) {
                grabKeyboard();
            } else {
                releaseKeyboard();
            }
        });
        refreshText();
    }

    // Stored value as a "0xNN" hex VK string (the on-disk keybind format).
    QString keyHex() const { return vkHex; }

    void setKeyHex(const QString &hex) {
        vkHex = hex.trimmed();
        capturing = false;
        setChecked(false);
        releaseKeyboard();
        refreshText();
    }

    // Notified whenever the captured key changes (avoids needing moc/signals).
    std::function<void()> onChanged;

protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (!capturing) {
            QPushButton::keyPressEvent(event);
            return;
        }
        // Ignore lone modifier presses so a chord's modifier can't be captured
        // as the key itself.
        const int key = event->key();
        if (key == Qt::Key_Control || key == Qt::Key_Shift ||
            key == Qt::Key_Alt || key == Qt::Key_Meta) {
            return;
        }

        const quint32 vk = event->nativeVirtualKey();
        if (vk != 0) {
            vkHex = QString("0x%1").arg(vk, 2, 16, QChar('0')).toUpper().replace("0X", "0x");
            capturing = false;
            setChecked(false);
            releaseKeyboard();
            refreshText();
            if (onChanged) {
                onChanged();
            }
        }
        event->accept();
    }

    void focusOutEvent(QFocusEvent *event) override {
        if (capturing) {
            capturing = false;
            setChecked(false);
            releaseKeyboard();
            refreshText();
        }
        QPushButton::focusOutEvent(event);
    }

private:
    void refreshText() {
        setText(capturing ? QStringLiteral("Press a key…") : friendlyName(vkHex));
    }

    static QString friendlyName(const QString &hex) {
        bool ok = false;
        const uint vk = hex.toUInt(&ok, 16);
        if (!ok) {
            return hex.isEmpty() ? QStringLiteral("Unset") : hex;
        }
        if (vk >= 'A' && vk <= 'Z') {
            return QString(QChar(vk));
        }
        if (vk >= '0' && vk <= '9') {
            return QString(QChar(vk));
        }
        if (vk >= 0x70 && vk <= 0x7B) { // VK_F1..VK_F12
            return QString("F%1").arg(vk - 0x70 + 1);
        }
        switch (vk) {
        case 0x14: return QStringLiteral("Caps Lock");
        case 0x20: return QStringLiteral("Space");
        case 0x09: return QStringLiteral("Tab");
        case 0x0D: return QStringLiteral("Enter");
        case 0x1B: return QStringLiteral("Esc");
        case 0x2D: return QStringLiteral("Insert");
        case 0x2E: return QStringLiteral("Delete");
        case 0x24: return QStringLiteral("Home");
        case 0x23: return QStringLiteral("End");
        case 0x21: return QStringLiteral("Page Up");
        case 0x22: return QStringLiteral("Page Down");
        case 0xC0: return QStringLiteral("` (backtick)");
        case 0xBC: return QStringLiteral(", (comma)");
        case 0xBE: return QStringLiteral(". (period)");
        case 0x25: return QStringLiteral("Left");
        case 0x26: return QStringLiteral("Up");
        case 0x27: return QStringLiteral("Right");
        case 0x28: return QStringLiteral("Down");
        default: break;
        }
        return QString("Key %1").arg(hex);
    }

    QString vkHex;
    bool capturing = false;
};

#endif // KEY_CAPTURE_BUTTON_H
