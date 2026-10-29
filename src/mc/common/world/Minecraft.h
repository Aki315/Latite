#pragma once
#include "level/Level.h"
#include "mc/Util.h"
#include "Timer.h"

namespace SDK {
    class Minecraft {
    public:
        Level* getLevel();

        // Re-found from RTTI by calibrate() when the game moves them.
        static inline size_t offGameSession = 0xC8;
        static inline size_t offTimer = 0xE0;
        static void calibrate(Minecraft* mc); // finds timer by its contents, gameSession once a world exists

        CLASS_FIELD_DYN(class GameSession*, gameSession, offGameSession);
        CLASS_FIELD_DYN(Timer*, timer, offTimer);
    };

    class GameSession {
    public:
        Level* getLevel();

        static inline size_t offLevel = 0x40;
    };
}
