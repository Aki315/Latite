#pragma once
#include "mc/Util.h"
#include "mc/common/world/actor/player/SerializedSkinRef.h"
#include <cstdint>
#include <string>

namespace SDK {
    class PlayerListEntry {
        char pad[0xD0];

    public:
        // 1.26.60 layout, read from a dump of a real entry (.playerinfo dump): id at 0x00, uuid at 0x08, then the name,
        // xuid and platform online id as 0x20-byte strings (0x18, 0x38, 0x58), a new empty 0x20-byte string at 0x78 (this
        // is what moved everything after it by 0x20), the build platform at 0x98, the skin at 0xA0, a color (4 floats) at
        // 0xB0 and the flag bytes at 0xC0. Before 1.26.60 the platform was at 0x78, the skin at 0x80 and the flags at 0xA0.
        // Verified on one entry (platform 8, host 1); the host flag position is the old 0xA1 plus 0x20.
        CLASS_FIELD(int64_t, uniqueId, 0x00);
        CLASS_FIELD(std::string, name, 0x18);
        CLASS_FIELD(std::string, xuid, 0x38);
        CLASS_FIELD(int32_t, buildPlatform, 0x98);
        CLASS_FIELD(SerializedSkinRef, skin, 0xA0);
        CLASS_FIELD(bool, isHost, 0xC1);
    };
}
