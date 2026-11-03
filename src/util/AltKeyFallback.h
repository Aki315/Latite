#pragma once
// Alt reaches the game as a "system" key message, and on some versions or setups the game never lets it through to the
// window procedure hook, so Alt can't be bound to a module (Freelook, for example). Once per game update this checks
// whether Alt changed and the hook has not reported that change itself; if so, the key event is sent from here.
#include <Windows.h>
#include "client/event/Eventing.h"
#include "client/event/events/KeyUpdateEvent.h"

namespace alt_fallback {
    inline bool hookSawAlt = false; // Set by the window procedure hook whenever it sees an Alt message.
    inline bool wasDown = false;

    inline void poll() {
        const HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        if (down == wasDown) return;
        wasDown = down;
        if (hookSawAlt) { // The normal path already delivered this change.
            hookSawAlt = false;
            return;
        }
        KeyUpdateEvent ev { VK_MENU, down };
        Eventing::get().dispatch(ev);
    }
}
