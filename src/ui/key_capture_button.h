#ifndef KEY_CAPTURE_BUTTON_H
#define KEY_CAPTURE_BUTTON_H

#include <QPushButton>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QInputDialog>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QVariantAnimation>
#include <functional>
#include "theme.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// A chip that captures a single physical key press and stores its virtual key
// code (as a "0xNN" hex string, matching the format the keybind system
// expects). It shows a friendly name — "Caps Lock", "A" — instead of forcing
// the user to know raw VK codes. Click it, then press any key; while armed the
// chip reads "Press a key…" in the accent and its border pulses. Right-click to
// type a code by hand, the escape hatch for keys the OS won't report normally
// (e.g. a registry-disabled Caps Lock, which surfaces as 0xFF but still fires
// 0x14 in the global hook). Painted by hand so the chip and its capture pulse
// match the Nocturne design rather than a platform button.
class KeyCaptureButton : public QPushButton {
public:
    explicit KeyCaptureButton(QWidget *parent = nullptr) : QPushButton(parent) {
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setToolTip("Click, then press a key. Right-click to type a VK code by hand.");
        setFont(theme::uiFont(13));
        pulse = new QVariantAnimation(this);
        pulse->setStartValue(0.0);
        pulse->setEndValue(1.0);
        pulse->setDuration(theme::animMs(1600));
        pulse->setEasingCurve(QEasingCurve::InOutSine);
        pulse->setLoopCount(-1);
        connect(pulse, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            pulseValue = v.toReal();
            update();
        });
        connect(this, &QPushButton::clicked, this, [this](bool checked) {
            capturing = checked;
            refreshText();
            if (capturing) {
                grabKeyboard();
                if (theme::animMs(1600) > 0) pulse->start();
            } else {
                releaseKeyboard();
                pulse->stop();
                pulseValue = 0.0;
                update();
            }
        });
        refreshText();
    }

    // Stored value as a "0xNN" hex VK string (the on-disk keybind format).
    QString keyHex() const { return vkHex; }

    void setKeyHex(const QString &hex) {
        vkHex = hex.trimmed();
        endCapture();
        refreshText();
    }

    // Notified whenever the captured key changes (avoids needing moc/signals).
    std::function<void()> onChanged;

    QSize sizeHint() const override {
        const int w = fontMetrics().horizontalAdvance(text()) + 26;
        return QSize(qMax(w, 44), qMax(30, fontMetrics().height() + 12));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = rect().adjusted(1, 1, -1, -1);
        const qreal radius = theme::kRadiusMd;

        QColor ground(theme::kSurface);
        QColor border(233, 233, 237, 41); // divider
        QColor textColor(theme::kText);
        if (capturing) {
            ground = QColor(theme::kAccent); ground.setAlphaF(0.10);
            textColor = QColor(theme::kAccent300);
            border = QColor(theme::kAccent);
        }

        // Armed pulse: an accent glow that breathes around the chip.
        if (capturing && pulseValue > 0.0) {
            QColor glow(theme::kAccent);
            glow.setAlphaF(0.10 + 0.30 * pulseValue);
            QPainterPath gp;
            gp.addRoundedRect(rect().adjusted(0, 0, -1, -1), radius, radius);
            QPen gpen(glow, 2.5);
            p.setPen(gpen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(gp);
        }

        QPainterPath path;
        path.addRoundedRect(r, radius, radius);
        p.fillPath(path, ground);
        p.setPen(QPen(border, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);

        p.setPen(textColor);
        p.drawText(r, Qt::AlignCenter, text());
    }

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

        quint32 vk = event->nativeVirtualKey();
#ifdef _WIN32
        // Some keys report 0 or 0xFF (a "no single virtual key" sentinel) as
        // their virtual key; resolve those from the hardware scan code so the
        // global hook, which matches on the real VK, can actually see them.
        if (vk == 0 || vk == 0xFF) {
            const quint32 scanCode = event->nativeScanCode();
            if (scanCode != 0) {
                const UINT mapped = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX);
                if (mapped != 0) {
                    vk = mapped;
                }
            }
        }
#endif
        if (vk == 0 || vk == 0xFF) {
            // The OS won't report a usable virtual key (e.g. a registry-
            // disabled Caps Lock). Point the user at manual entry.
            setText(QStringLiteral("Not detected — right-click to type a code"));
            event->accept();
            return; // stay in capture mode for another attempt
        }

        vkHex = QString("0x%1").arg(vk, 2, 16, QChar('0')).toUpper().replace("0X", "0x");
        endCapture();
        refreshText();
        if (onChanged) {
            onChanged();
        }
        event->accept();
    }

    void focusOutEvent(QFocusEvent *event) override {
        if (capturing) {
            endCapture();
            refreshText();
        }
        QPushButton::focusOutEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override {
        // Manual entry escape hatch: type the VK code in hex.
        if (capturing) {
            endCapture();
        }
        bool ok = false;
        const QString current = vkHex.isEmpty() ? QStringLiteral("0x14") : vkHex;
        const QString text = QInputDialog::getText(
            this, QStringLiteral("Enter key code"),
            QStringLiteral("Virtual-key code in hex (e.g. 0x14 = Caps Lock, 0x53 = S):"),
            QLineEdit::Normal, current, &ok);
        if (ok) {
            bool parsed = false;
            const uint v = text.trimmed().toUInt(&parsed, 16);
            if (parsed && v > 0 && v <= 0xFF) {
                vkHex = QString("0x%1").arg(v, 2, 16, QChar('0')).toUpper().replace("0X", "0x");
                if (onChanged) {
                    onChanged();
                }
            }
        }
        refreshText();
        event->accept();
    }

private:
    void endCapture() {
        capturing = false;
        setChecked(false);
        releaseKeyboard();
        pulse->stop();
        pulseValue = 0.0;
        update();
    }

    void refreshText() {
        setText(capturing ? QStringLiteral("Press a key…") : friendlyName(vkHex));
        updateGeometry();
        update();
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
    QVariantAnimation *pulse = nullptr;
    qreal pulseValue = 0.0;
};

#endif // KEY_CAPTURE_BUTTON_H
