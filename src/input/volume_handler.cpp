#include "volume_handler.h"
#include <QDebug>
#include <QSocketNotifier>

#ifdef __linux__
#include <QGuiApplication>
// Safe here (rather than in the header): nothing else includes this file, so
// Xlib's macros can't leak into unrelated translation units.
#include <X11/Xlib.h>
#include <X11/XF86keysym.h>

namespace {
constexpr unsigned int kLinuxGrabModifiers[] = {
    0,
    LockMask,
    Mod2Mask,
    static_cast<unsigned int>(LockMask | Mod2Mask)
};
}
#endif

VolumeHandler *VolumeHandler::instance = nullptr;

void VolumeHandler::applyKeybindSettings(const KeybindSettings &settings) {
    keybindSettings = settings;
}

#if defined(__linux__)
VolumeHandler::VolumeHandler(QObject *parent) : QObject(parent) {
    instance = this;

    const QString platformName = QGuiApplication::platformName().toLower();
    if (platformName.contains("wayland")) {
        qDebug() << "Linux global volume interception is not available on Wayland sessions.";
        return;
    }

    x11Display = XOpenDisplay(nullptr);
    if (!x11Display) {
        qDebug() << "Failed to open X11 display for global volume interception.";
        return;
    }

    x11RootWindow = DefaultRootWindow(x11Display);
    volumeUpKeyCode = XKeysymToKeycode(x11Display, XF86XK_AudioRaiseVolume);
    volumeDownKeyCode = XKeysymToKeycode(x11Display, XF86XK_AudioLowerVolume);

    if (volumeUpKeyCode == 0 || volumeDownKeyCode == 0) {
        qDebug() << "Failed to resolve X11 volume key keycodes.";
        XCloseDisplay(x11Display);
        x11Display = nullptr;
        return;
    }

    for (const unsigned int modifiers : kLinuxGrabModifiers) {
        XGrabKey(x11Display, volumeUpKeyCode, modifiers, x11RootWindow, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(x11Display, volumeDownKeyCode, modifiers, x11RootWindow, True, GrabModeAsync, GrabModeAsync);
    }

    XSelectInput(x11Display, x11RootWindow, KeyPressMask);
    XSync(x11Display, False);

    x11Notifier = new QSocketNotifier(ConnectionNumber(x11Display), QSocketNotifier::Read, this);
    connect(x11Notifier, &QSocketNotifier::activated, this, [this]() { processLinuxX11Events(); });
}

VolumeHandler::~VolumeHandler() {
    if (x11Notifier) {
        x11Notifier->setEnabled(false);
        x11Notifier->deleteLater();
        x11Notifier = nullptr;
    }

    releaseLinuxGrabs();

    if (x11Display) {
        XCloseDisplay(x11Display);
        x11Display = nullptr;
    }

    if (instance == this) {
        instance = nullptr;
    }
}

void VolumeHandler::processLinuxX11Events() {
    if (!x11Display) {
        return;
    }

    while (XPending(x11Display) > 0) {
        XEvent event;
        XNextEvent(x11Display, &event);

        if (event.type != KeyPress) {
            continue;
        }

        const int keyCode = event.xkey.keycode;
        if (keyCode != volumeUpKeyCode && keyCode != volumeDownKeyCode) {
            continue;
        }

        const bool isShift = (event.xkey.state & ShiftMask) != 0;
        const int delta = (keyCode == volumeUpKeyCode) ? (isShift ? 1 : 5) : (isShift ? -1 : -5);
        emit volumeChanged(delta);
    }
}

void VolumeHandler::releaseLinuxGrabs() {
    if (!x11Display || x11RootWindow == 0) {
        return;
    }

    for (const unsigned int modifiers : kLinuxGrabModifiers) {
        if (volumeUpKeyCode != 0) {
            XUngrabKey(x11Display, volumeUpKeyCode, modifiers, x11RootWindow);
        }
        if (volumeDownKeyCode != 0) {
            XUngrabKey(x11Display, volumeDownKeyCode, modifiers, x11RootWindow);
        }
    }

    XSync(x11Display, False);
}

#elif !defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__)
VolumeHandler::VolumeHandler(QObject *parent) : QObject(parent) {
    instance = this;
    qDebug() << "Global volume key interception is disabled on this platform.";
}

VolumeHandler::~VolumeHandler() {
    if (instance == this) {
        instance = nullptr;
    }
}
#endif
