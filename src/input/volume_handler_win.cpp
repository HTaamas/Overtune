#include "volume_handler.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QTimer>

#ifdef _WIN32
HHOOK VolumeHandler::hHook = nullptr;
bool VolumeHandler::queueLockChordDown = false;
bool VolumeHandler::likeChordDown = false;

VolumeHandler::VolumeHandler(QObject *parent) : QObject(parent) {
    instance = this;
    keybindSettings = AppSettings::loadKeybindSettings();

    hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    if (!hHook) {
        qDebug() << "Failed to install keyboard hook!";
    }
}

VolumeHandler::~VolumeHandler() {
    if (hHook) {
        UnhookWindowsHookEx(hHook);
        hHook = nullptr;
    }

    if (instance == this) {
        instance = nullptr;
    }
}

LRESULT CALLBACK VolumeHandler::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    const bool isShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool isCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const int doubleTapTimeout = 400;

    struct ModifierTapState {
        QElapsedTimer timer;
        bool pending = false;
        quint64 generation = 0;
    };

    static ModifierTapState nextTrackTapState;
    static ModifierTapState prevTrackTapState;

    auto handleModifierTap = [&](ModifierTapState &state, const auto &doubleTapAction) {
        if (!instance) {
            return;
        }

        if (state.pending && state.timer.isValid() && state.timer.elapsed() > doubleTapTimeout) {
            state.pending = false;
            ++state.generation;
        }

        if (!state.pending) {
            state.pending = true;
            state.timer.start();
            const quint64 generation = ++state.generation;

            const int timeoutMs = doubleTapTimeout;
            QTimer::singleShot(timeoutMs, instance, [generation, timeoutMs, &state]() {
                if (!instance) {
                    return;
                }

                if (!state.pending || state.generation != generation) {
                    return;
                }

                if (state.timer.isValid() && state.timer.elapsed() >= timeoutMs) {
                    state.pending = false;
                    emit instance->toggleMusic();
                }
            });

            return;
        }

        if (state.timer.isValid() && state.timer.elapsed() <= doubleTapTimeout) {
            state.pending = false;
            ++state.generation;
            doubleTapAction();
        }
    };

    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *pKey = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        const DWORD likeVk = instance ? DWORD(instance->keybindSettings.likeKey.toUInt(nullptr, 16)) : 0;

        if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            if (pKey->vkCode == 'U') {
                queueLockChordDown = false;
            }
            if (likeVk != 0 && pKey->vkCode == likeVk) {
                likeChordDown = false;
            }
        }

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            if (pKey->vkCode == 'U' && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
                if (!queueLockChordDown && instance) {
                    queueLockChordDown = true;
                    emit instance->toggleQueueLockRequested();
                }
                return 1;
            }

            if (likeVk != 0 && pKey->vkCode == likeVk && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
                if (!likeChordDown && instance) {
                    likeChordDown = true;
                    emit instance->likeSongRequested();
                }
                return 1;
            }

            if (pKey->vkCode == instance->keybindSettings.mainKey.toInt(nullptr, 16)) {
                if (instance) {
                    if (isShift && isCtrl) {
                        return 0;
                    } else if (isShift) {
                        handleModifierTap(nextTrackTapState, [&]() {
                            emit instance->nextTrack();
                        });
                    } else if (isCtrl) {
                        handleModifierTap(prevTrackTapState, [&]() {
                            emit instance->prevTrack();
                        });
                    } else {
                        emit instance->toggleMusic();
                    }
                }

                return 1;
            }

            if (pKey->vkCode == VK_VOLUME_UP || pKey->vkCode == VK_VOLUME_DOWN) {
                const bool useFineStep = instance && instance->keybindSettings.useShiftForFineAdjust && isShift;
                const int step = instance ? (useFineStep ? instance->keybindSettings.fineStep : instance->keybindSettings.coarseStep) : 5;
                const int delta = (pKey->vkCode == VK_VOLUME_UP) ? step : -step;

                if (instance) {
                    emit instance->volumeChanged(delta);
                }
                return 1;
            }
        }
    }

    return CallNextHookEx(hHook, nCode, wParam, lParam);
}
#endif
