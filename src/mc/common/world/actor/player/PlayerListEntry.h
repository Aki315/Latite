#pragma once
#include "mc/Util.h"
#include "mc/common/world/actor/player/SerializedSkinRef.h"
#include <cstdint>
#include <string>

namespace SDK {
    class PlayerListEntry {
        char pad[0xA8];

    public:
        // Layout, checked against a dump of a real entry: id at 0x00, uuid at 0x08, then the name, xuid and platform online
        // id as 0x20-byte strings, the build platform at 0x78, the skin at 0x80, a color (4 floats) at 0x90 and flag bytes
        // after it. Which flag byte is the host flag isn't settled: see the .playerinfo flags command.
        CLASS_FIELD(int64_t, uniqueId, 0x00);
        CLASS_FIELD(std::string, name, 0x18);
        CLASS_FIELD(std::string, xuid, 0x38);
        CLASS_FIELD(int32_t, buildPlatform, 0x78);
        CLASS_FIELD(bool, isHost, 0xA1);
        CLASS_FIELD(SerializedSkinRef, skin, 0x80);
    };
}
