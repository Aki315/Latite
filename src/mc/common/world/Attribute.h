#pragma once
#include "mc/Util.h"
#include "mc/deps/core/StringUtils.h"
#include "util/Crypto.h"

namespace SDK {
    class AttributeInstance {
        char pad[0x88];

    public:
        // AttributeInstance::getCurrentValue
        CLASS_FIELD(float, maxValue, 0x78);
        CLASS_FIELD(float, value, 0x7C);
    };

    class Attribute {
    public:
        int8_t mRedefinitionNode = 0;
        bool mSyncable = true;
        short unk = 0;
        int mIDValue;
        HashedString mName;

        Attribute(int id, std::string const& name)
            : mIDValue(id)
            , mName(name) {}
    };

    class Attributes {
    public:
        inline static Attribute Hunger = Attribute(1, "minecraft:player.hunger");
        inline static Attribute Saturation = Attribute(2, "minecraft:player.saturation");
        inline static Attribute Health = Attribute(6, "minecraft:health");
        // Golden hearts. Id 14 was found with the Health Indicator's "Debug: Attributes" on 1.26.50 (it reads 4 after a
        // golden apple and 16 after an enchanted one, max 16). In 1.26.60 hunger, saturation and health all moved down
        // by one, so this is 13 by the same shift - NOT checked in the game yet; if golden hearts don't show, run the
        // Debug: Attributes option again and use the id it reports.
        inline static Attribute Absorption = Attribute(13, "minecraft:absorption");
    };
}
