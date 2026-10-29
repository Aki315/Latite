#pragma once
#include "mc/Util.h"
#include "util/memory.h"
#include <string>

#include "mc/common/client/renderer/GameRenderer.h"

namespace SDK {
    class MinecraftGame {
    public:
        bool isCursorGrabbed();

        // Re-found from RTTI by calibrate(); false until the game has filled the field in.
        static inline size_t offGameRenderer = 0x1440;
        static bool calibrate(MinecraftGame* game);

        CLASS_FIELD_DYN(GameRenderer*, gameRenderer, offGameRenderer);

        class FontRepository* getFontRepository();

        class ClientInstance* getPrimaryClientInstance();
    };
}
