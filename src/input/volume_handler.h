#ifndef VOLUME_HANDLER_H
#define VOLUME_HANDLER_H

#include <QObject>
#include "settings/app_settings.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __linux__
// Deliberately NOT <X11/Xlib.h>. That header #defines None, Bool, Status,
// KeyPress, KeyRelease, FocusIn, FocusOut, Expose and more as bare macros,
// which then rewrite ordinary identifiers in every file that includes this one
// — queue_window.h's `enum class EdgeHit { None, ... }` becomes
// `{ 0L, ... }` and fails to compile. These two typedefs are identical to
// Xlib's own, so the real header can still be included alongside them in the
// implementation file.
struct _XDisplay;
using Display = _XDisplay;
using Window = unsigned long;
#endif

#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/hid/IOHIDManager.h>
#endif

class QSocketNotifier;

class VolumeHandler : public QObject {
    Q_OBJECT
public:
    explicit VolumeHandler(QObject *parent = nullptr);
    ~VolumeHandler() override;
    void applyKeybindSettings(const KeybindSettings &settings);

signals:
    void volumeChanged(int delta);
    void toggleMusic();
    void nextTrack();
    void prevTrack();
    void toggleQueueLockRequested();
    void toggleQueueShowRequested();
    void likeSongRequested();

private:
#ifdef _WIN32
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static HHOOK hHook;
    static bool lockChordDown;
    static bool showChordDown;
    static bool likeChordDown;
#endif
#ifdef __APPLE__
    static CGEventRef MacEventTapCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon);
    static void MacHIDInputCallback(void *context, IOReturn result, void *sender, IOHIDValueRef value);
    void setupCapsLockMonitor();
    CFMachPortRef eventTap = nullptr;
    CFRunLoopSourceRef runLoopSource = nullptr;
    IOHIDManagerRef hidManager = nullptr;
#endif
#ifdef __linux__
    void processLinuxX11Events();
    void releaseLinuxGrabs();
    Display *x11Display = nullptr;
    Window x11RootWindow = 0;
    QSocketNotifier *x11Notifier = nullptr;
    int volumeUpKeyCode = 0;
    int volumeDownKeyCode = 0;
#endif
    static VolumeHandler *instance;
    KeybindSettings keybindSettings;
};

#endif // VOLUME_HANDLER_H
