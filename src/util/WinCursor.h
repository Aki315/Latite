#pragma once
// Mouse state read from Windows instead of from game memory, so it keeps working when a game update moves the
// game's own fields.
#include <Windows.h>
#include <atomic>
#include "util/LMath.h"

namespace wincursor {
    // The game's main window (largest visible top-level window of this process); found again if it goes away.
    inline HWND gameWindow() {
        static HWND cached = nullptr;
        if (cached && IsWindow(cached)) return cached;
        struct Best {
            HWND wnd = nullptr;
            long area = 0;
        } best;
        EnumWindows(
            [](HWND h, LPARAM param) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
                RECT r = {};
                GetWindowRect(h, &r);
                const long area = (r.right - r.left) * (r.bottom - r.top);
                auto* b = reinterpret_cast<Best*>(param);
                if (area > b->area) {
                    b->wnd = h;
                    b->area = area;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&best));
        cached = best.wnd;
        return cached;
    }

    // true while the pointer is hidden, which is what the game does when it has captured the mouse (playing).
    // Menus, chat and inventories show the pointer.
    inline bool pointerHidden() {
        CURSORINFO ci { sizeof(CURSORINFO) };
        if (!GetCursorInfo(&ci)) return false;
        return !(ci.flags & CURSOR_SHOWING) || !ci.hCursor;
    }

    // Pointer position in client pixels of the game window.
    inline bool clientPos(float& x, float& y, float* clientW = nullptr, float* clientH = nullptr) {
        HWND w = gameWindow();
        if (!w) return false;
        POINT p {};
        RECT rc {};
        if (!GetCursorPos(&p) || !ScreenToClient(w, &p) || !GetClientRect(w, &rc)) return false;
        x = static_cast<float>(p.x);
        y = static_cast<float>(p.y);
        if (clientW) *clientW = static_cast<float>(rc.right - rc.left);
        if (clientH) *clientH = static_cast<float>(rc.bottom - rc.top);
        return true;
    }
}
