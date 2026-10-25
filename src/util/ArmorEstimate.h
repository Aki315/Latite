#pragma once
// Estimating how worn another player's armor is. Used by the Target HUD and the Armor Nametag, so both work the same.
//
// A hit wears every armor piece by max(1, floor(damage / 4)), Unbreaking III ignores about 30% of it. The client is
// not told that, so the tracker watches for health drops: when one lands shortly after you swung, the damage is your
// weapon (plus Sharpness V and Strength, as the server's Java PvP rules count them); for other hits it uses an assumed
// damage. How much of a hit gets through the armor (armor points, toughness, Protection IV) is used to tell weak
// swings and blocks from full hits.

#include "mc/Addresses.h"
#include "mc/common/entity/component/AttributesComponent.h"
#include "mc/common/world/Item.h"
#include "mc/common/world/ItemStack.h"
#include "mc/common/world/ItemStackBase.h"
#include "mc/common/world/actor/Actor.h"
#include "mc/common/world/actor/player/Player.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace ArmorEstimate {
    struct Vitals {
        float health = -1.f;
        float maxHealth = 20.f;
        float absorption = 0.f;
    };

    // Reads health, max health and golden hearts straight from the attribute list.
    inline Vitals readVitals(SDK::Actor* actor) {
        Vitals vitals;
        auto* attrs = actor->tryGetComponent<SDK::AttributesComponent>();
        if (!attrs) return vitals;

        auto& map = attrs->baseAttributes;
        auto sane = [](SDK::AttributeInstance* inst) {
            return inst && inst->maxValue > 0.f && inst->maxValue <= 2048.f && inst->value >= 0.f &&
                   inst->value <= inst->maxValue + 1024.f;
        };

        const int healthId = SDK::Attributes::Health.mIDValue;
        SDK::AttributeInstance* health = map.getInstance(healthId);
        if (!health) health = map.getInstanceByStride(healthId);
        if (!health) health = map.getInstanceDense(healthId);
        if (!health) health = map.getInstanceLenient(healthId);
        if (sane(health)) {
            vitals.health = health->value;
            vitals.maxHealth = health->maxValue;
        }

        const int absorptionId = SDK::Attributes::Absorption.mIDValue;
        SDK::AttributeInstance* absorption = map.getInstance(absorptionId);
        if (!absorption) absorption = map.getInstanceByStride(absorptionId);
        if (sane(absorption)) vitals.absorption = absorption->value;
        return vitals;
    }

    // The item name without its namespace: "minecraft:diamond_sword" and "bf:diamond_sword" both give "diamond_sword".
    inline std::string shortName(std::string id) {
        const auto colon = id.find(':');
        return colon == std::string::npos ? id : id.substr(colon + 1);
    }

    // Armor points and toughness of one piece. slot: 0 helmet, 1 chestplate, 2 leggings, 3 boots.
    inline void armorPoints(std::string const& id, int slot, float& defense, float& toughness) {
        struct Material {
            const char* name;
            int points[4];
            float toughness;
        };
        static constexpr Material kMaterials[] = {
            { "leather", { 1, 3, 2, 1 }, 0.f },   { "chainmail", { 2, 5, 4, 1 }, 0.f }, { "iron", { 2, 6, 5, 2 }, 0.f },
            { "golden", { 2, 5, 3, 1 }, 0.f },    { "diamond", { 3, 8, 6, 3 }, 2.f },   { "netherite", { 3, 8, 6, 3 }, 3.f },
        };
        const std::string name = shortName(id);
        for (auto const& m : kMaterials) {
            if (name.rfind(m.name, 0) == 0) {
                defense = static_cast<float>(m.points[slot]);
                toughness = m.toughness;
                return;
            }
        }
        defense = static_cast<float>(kMaterials[2].points[slot]); // Something unknown: treat it like iron.
        toughness = 0.f;
    }

    // Base damage of a held item the way the server's Java PvP rules count it. Returns -1 for items we don't know.
    // sharpable: swords and axes, the ones that get Sharpness.
    inline float weaponBase(std::string const& id, bool& sharpable) {
        struct Weapon {
            const char* name;
            float damage;
        };
        static constexpr Weapon kWeapons[] = {
            { "wooden_sword", 4.f },   { "golden_sword", 4.f },  { "stone_sword", 5.f },  { "iron_sword", 6.f },
            { "diamond_sword", 7.f },  { "netherite_sword", 8.f }, { "wooden_axe", 7.f },  { "golden_axe", 7.f },
            { "stone_axe", 9.f },      { "iron_axe", 9.f },      { "diamond_axe", 9.f },  { "netherite_axe", 10.f },
            { "trident", 8.f },
        };
        const std::string name = shortName(id);
        for (auto const& w : kWeapons) {
            if (name == w.name) {
                sharpable = name != "trident";
                return w.damage;
            }
        }
        sharpable = false;
        return -1.f;
    }

    struct Weapon {
        float base = -1.f; // -1 = unknown weapon
        bool sharpable = false;
    };

    struct Assumptions {
        bool protection = true; // They wear Protection IV on every piece.
        bool sharpness = true;  // Your sword or axe has Sharpness V (+3 damage).
        bool unbreaking = true; // Their armor has Unbreaking III (about 30% less wear).
        int strength = 0;       // Strength potion level, +3 damage per level.
    };

    // What you are holding, as a weapon. A bare hand does 1.
    inline Weapon heldWeapon(SDK::Player* me) {
        Weapon weapon { 1.f, false };
        if (me && me->supplies && me->supplies->inventory) {
            auto* held = me->supplies->inventory->getItem(me->supplies->selectedSlot);
            auto* item = held ? held->getItem() : nullptr;
            if (item) weapon.base = weaponBase(item->namespacedId.getString(), weapon.sharpable);
        }
        return weapon;
    }

    // One player's armor guess. It starts from what the server last sent for each piece and adds the wear from the hits
    // it sees. New pieces start over, and numbers the server sends later win over the guess.
    class Tracker {
    public:
        void reset() {
            for (auto& p : pieces) p = {};
            lastHealth = -1.f;
        }

        // The server sent this player's whole armor again: forget the wear, trust what it says.
        void resync() {
            for (auto& p : pieces) {
                p.baseDamage = p.lastServerDamage;
                p.wear = 0.f;
            }
        }

        // armor: the four armor stacks (null = empty slot). totalHealth: health + golden hearts, < 0 if unknown.
        // ownHit: you swung at them just now, so `weapon` is what hit. Otherwise a drop is counted only if
        // `otherHitDamage` is above 0 (an assumed damage for hits you can't see).
        void update(SDK::ItemStack* const* armor, float totalHealth, bool ownHit, Weapon const& weapon,
                    float otherHitDamage, Assumptions const& assume) {
            float defense = 0.f, toughness = 0.f;
            int count = 0;
            for (int i = 0; i < 4; ++i) {
                auto& piece = pieces[i];
                auto* item = armor[i] ? armor[i]->getItem() : nullptr;
                const int maxDamage = item ? item->getMaxDamage() : 0;
                if (maxDamage <= 0 || !Signatures::ItemStackBase_getDamageValue.result) {
                    piece = {};
                    continue;
                }

                const int server = std::clamp(static_cast<int>(armor[i]->getDamageValue()), 0, maxDamage);
                const std::string id = item->namespacedId.getString();
                if (!piece.known || piece.id != id || server < piece.lastServerDamage) {
                    // A new piece (or a repaired one): start over.
                    piece.known = true;
                    piece.id = id;
                    piece.baseDamage = server;
                    piece.wear = 0.f;
                } else if (server > piece.lastServerDamage) {
                    // The server sent newer numbers: trust them over the guess.
                    piece.baseDamage = server;
                    piece.wear = 0.f;
                }
                piece.lastServerDamage = server;
                piece.maxDamage = maxDamage;

                float d = 0.f, t = 0.f;
                armorPoints(id, i, d, t);
                defense += d;
                toughness += t;
                ++count;
            }

            const float previous = lastHealth;
            lastHealth = totalHealth;
            const float drop = previous - totalHealth;
            if (count == 0 || previous < 0.f || totalHealth < 0.f || drop < 0.3f) return;
            if (!ownHit && otherHitDamage <= 0.f) return;

            // How much of a hit gets through their armor: armor points, then Protection.
            const float protection = assume.protection ? 1.f - std::min(20.f, 4.f * count) * 0.04f : 1.f;
            auto getsThrough = [&](float raw) {
                const float reduction =
                    std::clamp(std::max(defense / 5.f, defense - raw / (2.f + toughness / 4.f)), 0.f, 20.f) / 25.f;
                return std::max(0.03f, (1.f - reduction) * protection);
            };

            float raw;
            if (ownHit && weapon.base >= 0.f) {
                // We know the weapon: its damage, Sharpness V = +3, Strength = +3 per level.
                raw = weapon.base + (weapon.sharpable && assume.sharpness ? 3.f : 0.f) + 3.f * static_cast<float>(assume.strength);
            } else if (!ownHit) {
                raw = otherHitDamage;
            } else {
                // Unknown weapon: work back from how much health it took.
                raw = drop * 3.f;
                for (int k = 0; k < 4; ++k) raw = std::min(40.f, drop / getsThrough(raw));
                raw = std::max(raw, 1.f);
            }
            if (ownHit ? weapon.base >= 0.f : true) {
                // A weak swing or a block shows up as a smaller drop than expected, so scale down by that. Never
                // scale up: the target may not have Protection at all.
                const float expected = raw * getsThrough(raw);
                raw *= std::clamp(drop / expected, 0.2f, 1.f);
            }

            const int perPiece = std::max(1, static_cast<int>(std::floor(raw / 4.f)));
            const float kept = assume.unbreaking ? 0.7f : 1.f; // Unbreaking III: 30% of the wear is ignored.
            for (auto& piece : pieces) {
                if (piece.known) piece.wear += static_cast<float>(perPiece) * kept;
            }
        }

        // Durability points lost on a slot according to the guess, -1 if nothing is equipped there.
        int estimatedDamage(int slot) const {
            auto const& piece = pieces[slot];
            if (!piece.known) return -1;
            return std::clamp(piece.baseDamage + static_cast<int>(std::lround(piece.wear)), 0, piece.maxDamage);
        }

    private:
        struct Piece {
            bool known = false;
            std::string id;
            int maxDamage = 0;
            int lastServerDamage = 0;
            int baseDamage = 0; // What the server last said, the guess builds on this.
            float wear = 0.f;   // Extra durability lost since then, from the hits.
        };
        Piece pieces[4];
        float lastHealth = -1.f;
    };
}
