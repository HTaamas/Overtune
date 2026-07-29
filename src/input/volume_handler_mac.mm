#include "volume_handler.h"
#include <QDebug>
#include <QMessageBox>
#include <QMetaObject>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>

#ifdef __APPLE__
#import <AppKit/AppKit.h>
#include <IOKit/hidsystem/ev_keymap.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/IOKitLib.h>
#include <ApplicationServices/ApplicationServices.h>

namespace {
constexpr int kKeyStateDown = 0x0A;
constexpr int kVkCapital = 0x14; // Windows VK_CAPITAL — the value the main-key setting stores for Caps Lock

bool isVolumeKey(int keyCode) {
    return keyCode == NX_KEYTYPE_SOUND_UP || keyCode == NX_KEYTYPE_SOUND_DOWN;
}

// Double-tap state for the main-key modifier chords, mirroring the Windows
// behaviour: a single tap (with the modifier held) falls back to play/pause
// after the double-tap window closes, while a second tap inside the window
// fires the double-tap action (skip / previous).
struct ModifierTapState {
    QElapsedTimer timer;
    bool pending = false;
    quint64 generation = 0;
};
ModifierTapState g_nextTapState;
ModifierTapState g_prevTapState;

void handleModifierTap(ModifierTapState &state,
                       const std::function<void()> &singleTapAction,
                       const std::function<void()> &doubleTapAction) {
    constexpr int doubleTapTimeoutMs = 400;

    if (state.pending && state.timer.isValid() && state.timer.elapsed() > doubleTapTimeoutMs) {
        state.pending = false;
        ++state.generation;
    }

    if (!state.pending) {
        state.pending = true;
        state.timer.start();
        const quint64 generation = ++state.generation;
        QTimer::singleShot(doubleTapTimeoutMs, [generation, &state, singleTapAction]() {
            if (!state.pending || state.generation != generation) {
                return;
            }
            if (state.timer.isValid() && state.timer.elapsed() >= doubleTapTimeoutMs) {
                state.pending = false;
                singleTapAction();
            }
        });
        return;
    }

    if (state.timer.isValid() && state.timer.elapsed() <= doubleTapTimeoutMs) {
        state.pending = false;
        ++state.generation;
        doubleTapAction();
    }
}

// Force the Caps Lock state (and its LED) off. Used so that Caps Lock, when it
// acts as a hotkey, never actually engages caps typing or lights the LED.
void forceCapsLockOff() {
    static io_connect_t connect = MACH_PORT_NULL;
    if (connect == MACH_PORT_NULL) {
        io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOHIDSystem"));
        if (service) {
            IOServiceOpen(service, mach_task_self(), kIOHIDParamConnectType, &connect);
            IOObjectRelease(service);
        }
    }
    if (connect != MACH_PORT_NULL) {
        IOHIDSetModifierLockState(connect, kIOHIDCapsLockState, false);
    }
}

bool ensureMacInputAccess() {
    bool hasListenAccess = true;
    if (@available(macOS 10.15, *)) {
        hasListenAccess = CGPreflightListenEventAccess();
        if (!hasListenAccess) {
            CGRequestListenEventAccess();
            hasListenAccess = CGPreflightListenEventAccess();
        }
    }

    const NSDictionary *options = @{
        (__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES
    };
    const bool hasAccessibilityAccess = AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);

    return hasListenAccess && hasAccessibilityAccess;
}
}

VolumeHandler::VolumeHandler(QObject *parent) : QObject(parent) {
    instance = this;

    if (!ensureMacInputAccess()) {
        QMessageBox::information(
            nullptr,
            "Overtune Permission Needed",
            "To intercept the volume keys as a bundled app, allow Overtune in System Settings under Privacy & Security > Input Monitoring and Accessibility, then reopen the app."
        );
        qDebug() << "macOS global volume interception requires Input Monitoring and Accessibility access.";
        return;
    }

    eventTap = CGEventTapCreate(
        kCGSessionEventTap,
        kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        CGEventMaskBit(NX_SYSDEFINED),
        MacEventTapCallback,
        this
    );

    if (!eventTap) {
        qDebug() << "Failed to install macOS event tap. Input Monitoring permission may be required.";
        return;
    }

    runLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, eventTap, 0);
    if (!runLoopSource) {
        qDebug() << "Failed to create macOS event tap run loop source.";
        CFRelease(eventTap);
        eventTap = nullptr;
        return;
    }

    CFRunLoopAddSource(CFRunLoopGetMain(), runLoopSource, kCFRunLoopCommonModes);
    CGEventTapEnable(eventTap, true);

    setupCapsLockMonitor();
}

VolumeHandler::~VolumeHandler() {
    if (hidManager) {
        IOHIDManagerUnscheduleFromRunLoop(hidManager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
        CFRelease(hidManager);
        hidManager = nullptr;
    }

    if (runLoopSource) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), runLoopSource, kCFRunLoopCommonModes);
        CFRelease(runLoopSource);
        runLoopSource = nullptr;
    }

    if (eventTap) {
        CFMachPortInvalidate(eventTap);
        CFRelease(eventTap);
        eventTap = nullptr;
    }

    if (instance == this) {
        instance = nullptr;
    }
}

// Caps Lock never produces a normal key event, and the system applies a
// deliberate activation delay to its toggle (the reason a quick tap "does
// nothing"). We therefore read the RAW Caps Lock key through IOHIDManager,
// which fires instantly on press regardless of that delay, and force the caps
// state back off so it neither latches nor lights the LED.
void VolumeHandler::setupCapsLockMonitor() {
    hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!hidManager) {
        qDebug() << "Failed to create IOHIDManager for Caps Lock.";
        return;
    }

    NSArray *matches = @[
        @{ @(kIOHIDDeviceUsagePageKey): @(kHIDPage_GenericDesktop),
           @(kIOHIDDeviceUsageKey): @(kHIDUsage_GD_Keyboard) },
        @{ @(kIOHIDDeviceUsagePageKey): @(kHIDPage_GenericDesktop),
           @(kIOHIDDeviceUsageKey): @(kHIDUsage_GD_Keypad) },
    ];
    IOHIDManagerSetDeviceMatchingMultiple(hidManager, (__bridge CFArrayRef)matches);
    IOHIDManagerRegisterInputValueCallback(hidManager, MacHIDInputCallback, this);
    IOHIDManagerScheduleWithRunLoop(hidManager, CFRunLoopGetMain(), kCFRunLoopCommonModes);

    if (IOHIDManagerOpen(hidManager, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        qDebug() << "Failed to open IOHIDManager for Caps Lock (Input Monitoring permission may be required).";
    }
}

void VolumeHandler::MacHIDInputCallback(void *, IOReturn, void *, IOHIDValueRef value) {
    if (!instance) {
        return;
    }

    IOHIDElementRef element = IOHIDValueGetElement(value);
    if (IOHIDElementGetUsagePage(element) != kHIDPage_KeyboardOrKeypad ||
        IOHIDElementGetUsage(element) != kHIDUsage_KeyboardCapsLock) {
        return;
    }

    // Cancel any latch/LED on both press and release (the system may toggle it
    // slightly after the press, so we clear it on release too).
    forceCapsLockOff();

    const CFIndex pressed = IOHIDValueGetIntegerValue(value);
    if (pressed != 1) {
        return; // act on key-down only
    }

    // Only when the user has mapped Caps Lock (VK_CAPITAL) as the main key.
    bool ok = false;
    if (instance->keybindSettings.mainKey.toInt(&ok, 16) != kVkCapital || !ok) {
        return;
    }

    const NSEventModifierFlags mods = [NSEvent modifierFlags];
    const bool isShift = (mods & NSEventModifierFlagShift) != 0;
    const bool isCtrl = (mods & NSEventModifierFlagControl) != 0;

    if (isShift && isCtrl) {
        return; // Ignore Shift+Ctrl+MainKey
    }

    auto singleTap = [handler = instance] { if (handler) emit handler->toggleMusic(); };

    if (isShift) {
        auto doubleTap = [handler = instance] { if (handler) emit handler->nextTrack(); };
        QMetaObject::invokeMethod(instance, [=]() { handleModifierTap(g_nextTapState, singleTap, doubleTap); }, Qt::QueuedConnection);
    } else if (isCtrl) {
        auto doubleTap = [handler = instance] { if (handler) emit handler->prevTrack(); };
        QMetaObject::invokeMethod(instance, [=]() { handleModifierTap(g_prevTapState, singleTap, doubleTap); }, Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(instance, [=]() {
            singleTap();
        }, Qt::QueuedConnection);
    }
}

CGEventRef VolumeHandler::MacEventTapCallback(CGEventTapProxy, CGEventType type, CGEventRef event, void *refcon) {
    auto *handler = static_cast<VolumeHandler *>(refcon);
    if (!handler) {
        return event;
    }

    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (handler->eventTap) {
            CGEventTapEnable(handler->eventTap, true);
        }
        return event;
    }

    if (type != static_cast<CGEventType>(NX_SYSDEFINED)) {
        return event;
    }

    NSEvent *nsEvent = [NSEvent eventWithCGEvent:event];
    if (!nsEvent || nsEvent.subtype != NX_SUBTYPE_AUX_CONTROL_BUTTONS) {
        return event;
    }

    const NSInteger data = nsEvent.data1;
    const int keyCode = static_cast<int>((data & 0xFFFF0000) >> 16);
    const int keyState = static_cast<int>((data & 0x0000FF00) >> 8);

    if (!isVolumeKey(keyCode)) {
        return event;
    }

    if (keyState == kKeyStateDown && instance) {
        const bool isShift = (nsEvent.modifierFlags & NSEventModifierFlagShift) != 0;
        const int delta = (keyCode == NX_KEYTYPE_SOUND_UP) ? (isShift ? 1 : 5) : (isShift ? -1 : -5);
        QMetaObject::invokeMethod(instance, [delta]() {
            if (instance) {
                emit instance->volumeChanged(delta);
            }
        }, Qt::QueuedConnection);
    }

    return nullptr;
}
#endif
