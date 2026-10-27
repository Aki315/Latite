#pragma once
#include "mc/Util.h"
#include "mc/common/world/actor/player/SerializedSkinRef.h"
#include <cstdint>
#include <string>

namespace SDK {
    class PlayerListEntry {
        char pad[0xA8];

    public:
        // The unique id, the name and the skin were already used. The platform and host flag follow the entry's layout
        // (id 8, uuid 16, name, xuid and platform online id as 0x20-byte strings, build platform, then the skin at 0x80,
        // then the teacher and host flags); the .playerinfo command can dump an entry to check them.
        CLASS_FIELD(int64_t, uniqueId, 0x00);
        CLASS_FIELD(std::string, name, 0x18);
        CLASS_FIELD(std::string, xuid, 0x38);
        CLASS_FIELD(int32_t, buildPlatform, 0x78);
        CLASS_FIELD(bool, isHost, 0x91);
        CLASS_FIELD(SerializedSkinRef, skin, 0x80);
    };
}
