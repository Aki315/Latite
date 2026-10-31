#pragma once
// Keyboard input that does not depend on the game's window procedure being hooked correctly.
// A message hook on the game window's thread sees every key message. The result is only used while the normal
// hook (GenericHooks::MainWindow__windowProcCallback) has not delivered any key message, so keys are never
// dispatched twice.
#include <Windows.h>
#include <atomic>
#include "client/event/Eventing.h"
#include "client/event/events/KeyUpdateEvent.h"
#include "client/event/events/ClickEvent.h"
#include "util/Logger.h"
#include "util/Trace.h"
#include "util/WinCursor.h"

namespace input_fallback {
    inline std::atomic<bool> mainHookSawKey { false };
    inline std::atomic<bool> mainHookSawWheel { false };
    inline std::atomic<bool> mainHookSawButton { false };
    inline HHOOK hook = nullptr;
    inline HWND gameWindow = nullptr;

    inline LRESULT CALLBACK getMsgProc(int code, WPARAM wParam, LPARAM lParam) {
        if (code >= 0 && wParam == PM_REMOVE && lParam) {
            auto* m = reinterpret_cast<MSG*>(lParam);
            const bool sys = m->message == WM_SYSKEYDOWN || m->message == WM_SYSKEYUP;
            if (m->message == WM_MOUSEWHEEL) {
                // The game's own mouse input hook has not reported any wheel movement: scroll the menu from here.
                LATITE_TRACE_HOOK("input fallback: first mouse wheel message");
                if (!mainHookSawWheel.load()) {
                    const int notches = GET_WHEEL_DELTA_WPARAM(m->wParam) / WHEEL_DELTA;
                    if (notches != 0) {
                        ClickEvent ev { 4, static_cast<char>(notches > 0 ? 120 : -120) };
                        if (Eventing::get().dispatch(ev)) m->message = WM_NULL;
                    }
                }
            }
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

    // Mouse buttons, read from Windows once per game tick. Only used while the game's own mouse hook has not
    // reported a single button press (it does not fire on 1.26.60).
    inline void pollButtons() {
        static bool prev[3] = {};
        static const int vk[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
        if (mainHookSawButton.load()) return;
        HWND w = wincursor::gameWindow();
        if (!w || GetForegroundWindow() != w) {
            for (auto& p : prev) p = false;
            return;
        }
        for (int i = 0; i < 3; i++) {
            const bool down = (GetAsyncKeyState(vk[i]) & 0x8000) != 0;
            if (down == prev[i]) continue;
            prev[i] = down;
            LATITE_TRACE_HOOK("input fallback: first mouse button change");
            ClickEvent ev { i + 1, static_cast<char>(down ? 1 : 0) };
            Eventing::get().dispatch(ev);
        }
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
