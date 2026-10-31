#pragma once
#include <atomic>

// The game's own drawing calls (tessellator, text flush, images) depend on signatures and vtable slots that move with
// every Minecraft update. When one of them is missing or crashes, it is switched off here so the rest of Latite keeps
// running instead of taking the game down.
namespace SDK::uidraw {
    inline std::atomic<bool> meshBroken { false };
    inline std::atomic<bool> textFlushBroken { false };
    inline std::atomic<bool> textDrawBroken { false };
    inline std::atomic<bool> imageBroken { false };

    // true when tessellator drawing is possible (all signatures found and none of the calls has crashed)
    bool meshReady();
}
