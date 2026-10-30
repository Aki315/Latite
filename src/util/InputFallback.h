#pragma once
// Keyboard input that does not depend on the game's window procedure being hooked correctly.
// A message hook on the game window's thread sees every key message. The result is only used while the normal
// hook (GenericHooks::MainWindow__windowProcCallback) has not delivered any key message, so keys are never
// dispatched twice.
#include <Windows.h>
#include <atomic>
#include "client/event/Eventing.h"
#include "client/event/events/KeyUpdateEvent.h"
#include "util/Logger.h"
#include "util/Trace.h"

namespace input_fallback {
    inline std::atomic<bool> mainHookSawKey { false };
    inline HHOOK hook = nullptr;
    inline HWND gameWindow = nullptr;

    inline LRESULT CALLBACK getMsgProc(int code, WPARAM wParam, LPARAM lParam) {
        if (code >= 0 && wParam == PM_REMOVE && lParam) {
            auto* m = reinterpret_cast<MSG*>(lParam);
            const bool sys = m->message == WM_SYSKEYDOWN || m->message == WM_SYSKEYUP;
            if (m->message == WM_KEYDOWN || m->message == WM_KEYUP || sys) {
                LATITE_TRACE_HOOK("input fallback: first key message");
                if (!mainHookSawKey.load()) {
                    const bool down = m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN;
                    KeyUpdateEvent ev { static_cast<int>(m->wParam & 0xFF), down };
                    if (Eventing::get().dispatch(ev)) m->message = WM_NULL;
                }
            }
        }
        return CallNextHookEx(hook, code, wParam, lParam);
    }

    inline BOOL CALLBACK findWindowProc(HWND h, LPARAM param) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        RECT r = {};
        GetWindowRect(h, &r);
        const long area = (r.right - r.left) * (r.bottom - r.top);
        auto* best = reinterpret_cast<std::pair<HWND, long>*>(param);
        if (area > best->second) *best = { h, area };
        return TRUE;
    }

    inline void install() {
        if (hook) return;
        std::pair<HWND, long> best { nullptr, 0 };
        EnumWindows(findWindowProc, reinterpret_cast<LPARAM>(&best));
        gameWindow = best.first;
        if (!gameWindow) {
            Logger::Warn("[Input] could not find the game window for the key fallback");
            return;
        }
        const DWORD thread = GetWindowThreadProcessId(gameWindow, nullptr);
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&getMsgProc), &self);
        hook = SetWindowsHookExW(WH_GETMESSAGE, getMsgProc, self, thread);
        char title[128] = {};
        GetWindowTextA(gameWindow, title, sizeof(title));
        Logger::Info("[Input] key fallback on window 0x{:X} \"{}\" (thread {}): {}",
                     reinterpret_cast<uintptr_t>(gameWindow), title, thread, hook ? "installed" : "FAILED");
    }
}
