#pragma once
// Estimating how worn another player's armor is. Used by the Target HUD and the Armor Nametag, so both work the same.
//
// A hit wears every armor piece by max(1, floor(damage / 4)), Unbreaking III ignores about 30% of it. The client is
// not told that, so the tracker watches for health drops: when one lands shortly after you swung, the damage is your
// weapon (plus Sharpness V and Strength, as the server's Java PvP rules count them); for other hits it uses an assumed
// damage. How much of a hit gets through the armor (armor points, toughness, Protection IV) is used to tell weak
// swings and blocks from full hits.
//
// Critical hits are counted on their own: the server's Java PvP rules make a swing a crit when you are falling and
// not on the ground, and a crit multiplies the weapon's base damage by 1.5 (Sharpness and Strength are added after).
// The client sees the same thing as your height dropping from one tick to the next, so no setting is needed.
//
// What it reads about YOUR swing on its own: the item in your hand (swords, axes, trident, pickaxes, shovels, hoes and
// the mace; anything else is worked out from how much health it took), the Sharpness level of that item read from its
// enchantment data (util/EnchantNbt.h), and your own Strength and Weakness potions. If the enchantment data can't be
// read (the layout of the game's tag is not verified, so this can fail on some game versions), an enchanted (glowing)
// sword is taken as Sharpness V as far as the Assume Sharpness setting allows, like before.
// For THEIR armor it checks which pieces are enchanted (they glow); the levels of THEIR armor are not read, so enchanted
// armor is taken as Protection IV, and Unbreaking is only counted on pieces that glow, since a plain piece can't have it.
//
// When the estimate starts over (so it never shows a stale number): the target dies (health reaches 0) or comes back
// to life (health jumps up by 10 or more), a piece disappears from its slot, a different item or enchanted state
// shows up in a slot, the server's durability for a piece goes down (repaired / fresh piece), the server sends the
// whole armor again (see syncArmorPacket), and the owner of the tracker calls reset() on a new target.
//
// Limits, since the client can't see everything:
//  - The enchantment levels of THEIR armor are not read, only "enchanted or not", so Protection IV and Unbreaking III
//    are assumed. Your own weapon's Sharpness level is read when the item's enchantment data can be understood.
//  - Other players' potions (Resistance, Weakness on them) and their shield blocks are not visible. A block or a weaker
//    hit than expected shows up as a smaller health drop and scales the wear down, but a hit harder than expected is
//    never scaled up.
//  - Swapping to an identical fresh piece with no gap, no packet and no enchant change can't be told apart from the
//    same piece; the next durability number the server sends corrects it.
//  - A hit is only recognised from a health drop: 0.05 or more right after you swung, 0.3 or more for hits you didn't
//    throw. A hit that takes no health at all (a block, a swing the server cancelled) isn't counted.
//  - The damage numbers follow Blurry's Java PvP addon (item damage, crit x1.5, Sharpness 0.5 x level + 0.5, Strength
//    +3, Weakness -4). The addon also scales damage by your attack cooldown; the client can't see that, so a weak
//    swing is only noticed through the smaller health drop.
//  - Armor reduction and wear are the game's own (the addon deals damage with applyDamage), so they use Bedrock's armor
//    points and toughness, and the usual wear of max(1, floor(damage / 4)) per piece.
//  - The result is an estimate, and the displays mark it with ~.

#include "mc/Addresses.h"
#include "util/EnchantNbt.h"
#include "util/Logger.h"
#include "mc/common/entity/component/AttributesComponent.h"
#include "mc/common/entity/IEntityComponent.h"
#include "mc/common/entity/component/MobEffectsComponent.h"
#include "mc/common/network/Packet.h"
#include "mc/common/world/Item.h"
#include "mc/common/world/ItemStack.h"
#include "mc/common/world/ItemStackBase.h"
#include "mc/common/entity/component/ActorEquipmentComponent.h"
#include "mc/common/world/actor/Actor.h"
#include "mc/common/world/level/Level.h"
#include "mc/common/world/actor/player/Player.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstring>
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
            { "trident", 8.f },        { "mace", 5.f },
            { "copper_sword", 4.f },   { "copper_axe", 7.f }, // In Blurry's addon too (minecraft:damage in its item files).
            // Tools the server's damage table knows too.
            { "wooden_pickaxe", 2.f }, { "stone_pickaxe", 3.f }, { "iron_pickaxe", 4.f }, { "diamond_pickaxe", 5.f },
            { "netherite_pickaxe", 6.f },
            { "wooden_shovel", 2.5f }, { "stone_shovel", 3.5f }, { "iron_shovel", 4.5f }, { "diamond_shovel", 5.5f },
            { "netherite_shovel", 6.5f },
            { "wooden_hoe", 1.f },     { "stone_hoe", 1.f },     { "iron_hoe", 1.f },     { "diamond_hoe", 1.f },
            { "netherite_hoe", 1.f },
        };
        const std::string name = shortName(id);
        for (auto const& w : kWeapons) {
            if (name == w.name) {
                // Sharpness only goes on swords and axes.
                sharpable = name.find("_sword") != std::string::npos || name.find("_axe") != std::string::npos;
                return w.damage;
            }
        }
        sharpable = false;
        return -1.f;
    }

    struct Weapon {
        float base = -1.f; // -1 = unknown weapon
        bool sharpable = false;
        bool crit = false; // The swing was a critical hit: you were falling.
        bool enchanted = false; // The item in your hand glows, so it has enchantments.
        int sharpness = -1;     // Sharpness level read from the item (0 = it has none), -1 = could not be read.
        int strength = 0;       // Your own Strength level when you swung (0 = none).
        int weakness = 0;       // Your own Weakness level when you swung (0 = none).
    };

    // MobArmorEquipmentPacket (protocol 1.26.51, packet id 32): the server sends a player's whole armor again, when
    // they put armor on, take it off or it changes. Its first field is the actor's runtime id. Latite has no typed
    // wrapper for it, so this is a small view of just that field; check it against the game version before relying on it.
    struct MobArmorEquipmentPacketView : SDK::Packet {
        uint64_t runtimeID;
    };
    constexpr auto kMobArmorEquipmentPacketId = static_cast<SDK::PacketID>(0x20);

    // If the packet is a whole-armor update, the runtime id of the player it is for, otherwise 0. Packet thread: only
    // read, remember the id, and act on it later from the game thread.
    inline uint64_t syncArmorPacket(SDK::Packet* packet) {
        if (!packet || packet->getID() != kMobArmorEquipmentPacketId) return 0;
        return reinterpret_cast<MobArmorEquipmentPacketView*>(packet)->runtimeID;
    }

    struct Assumptions {
        bool protection = true; // They wear Protection IV on every piece.
        bool sharpness = true;  // Your sword or axe has Sharpness V (+3 damage).
        bool unbreaking = true; // Their armor has Unbreaking III (about 30% less wear).
        int strength = 0;       // Strength potion level, +3 damage per level.
        float wearScale = 1.f;  // Multiplies the wear of every hit; raise it if the guess keeps showing too much durability.
    };

    // Is a swing made right now a critical hit? The server counts it as one when you are falling and not on the ground.
    // Going down faster than a slow sink in water, from one tick to the next, is the same thing.
    inline bool isCriticalSwing(SDK::Actor* me) {
        if (!me || !me->stateVector) return false;
        return me->getPosOld().y - me->getPos().y > 0.035f;
    }

    // Your own Strength and Weakness levels (0 = none), read from the effects the game keeps on you.
    // Effect ids: 5 is Strength, 18 is Weakness. The amplifier starts at 0 for level I.
    inline void readOwnEffects(SDK::Actor* me, int& strength, int& weakness) {
        strength = 0;
        weakness = 0;
        if (!me) return;
        auto* effects = me->tryGetComponent<SDK::MobEffectsComponent>();
        if (!effects) return;
        for (auto const& effect : effects->read()) {
            const int level = std::clamp(effect.amplifier + 1, 0, 10);
            if (effect.id == 5) strength = std::max(strength, level);
            else if (effect.id == 18) weakness = std::max(weakness, level);
        }
    }

    // Reads game memory without crashing: false instead of an access violation when the address is not mapped. No objects
    // with destructors in here, which __try requires.
    inline bool readGameMemory(const void* from, void* to, size_t bytes) {
        __try {
            std::memcpy(to, from, bytes);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // What the weapon in your hand has, logged once each time it changes so a wrong reading is easy to spot in the log.
    inline void logWeaponRead(std::string const& item, EnchantNbt::Enchants const& enchants) {
        static std::string last;
        std::string now = item + (enchants.status == EnchantNbt::Status::Parsed
                                      ? " sharpness " + std::to_string(enchants.levelOf(EnchantNbt::kSharpness))
                                      : " unreadable");
        if (now == last) return;
        last = std::move(now);
        if (enchants.status == EnchantNbt::Status::Parsed) {
            std::string all;
            for (auto const& e : enchants.list) all += " [" + std::to_string(e.first) + ":" + std::to_string(e.second) + "]";
            Logger::Info("[ArmorEstimate] your {}: Sharpness {} read from the item (enchantments {})", item,
                         enchants.levelOf(EnchantNbt::kSharpness), all.empty() ? " none" : all);
        } else {
            Logger::Info("[ArmorEstimate] your {} glows but its enchantments could not be read: assuming Sharpness V "
                         "(if Assume Sharpness is on)",
                         item);
        }
    }

    // What you are swinging, as a weapon: the item in your hand (a bare hand does 1), whether it is enchanted, whether
    // the swing is a crit, and your own potions. Call it when you hit.
    inline Weapon heldWeapon(SDK::Player* me) {
        Weapon weapon;
        weapon.base = 1.f;
        weapon.crit = isCriticalSwing(me);
        readOwnEffects(me, weapon.strength, weapon.weakness);
        if (me && me->supplies && me->supplies->inventory) {
            auto* held = me->supplies->inventory->getItem(me->supplies->selectedSlot);
            auto* item = held ? held->getItem() : nullptr;
            if (item) {
                weapon.base = weaponBase(item->namespacedId.getString(), weapon.sharpable);
                weapon.enchanted = item->isGlint(held);
                if (weapon.enchanted && weapon.sharpable) {
                    const auto enchants = EnchantNbt::readEnchants(held->tag.get(), readGameMemory);
                    if (enchants.status == EnchantNbt::Status::Parsed) {
                        weapon.sharpness = enchants.levelOf(EnchantNbt::kSharpness);
                    }
                    logWeaponRead(shortName(item->namespacedId.getString()), enchants);
                }
            }
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
            lastLife = -1.f;
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
        //
        // health: their health without golden hearts, or < 0 if unknown. Used to notice a death or a respawn.
        void update(SDK::ItemStack* const* armor, float totalHealth, bool ownHit, Weapon const& weapon,
                    float otherHitDamage, Assumptions const& assume, float health = -1.f) {
            // Dead (health 0) or alive again (a jump of 10 or more, which healing can't do but a respawn does): the
            // wear belongs to the last life, so start over from what the server says now.
            if (health >= 0.f) {
                const bool died = health <= 0.05f;
                const bool revived = lastLife >= 0.f && health - lastLife >= 10.f;
                if (died || revived) reset();
                lastLife = health;
            } else {
                lastLife = -1.f;
            }

            float defense = 0.f, toughness = 0.f;
            int count = 0;
            int enchantedPieces = 0; // Armor pieces that glow: taken as Protection IV.
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
                const bool glint = item->isGlint(armor[i]);
                if (!piece.known || piece.id != id || piece.glint != glint || server < piece.lastServerDamage) {
                    // A new piece (or a repaired one): start over.
                    piece.known = true;
                    piece.id = id;
                    piece.glint = glint;
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
                if (glint) ++enchantedPieces;
            }

            const float previous = lastHealth;
            lastHealth = totalHealth;
            const float drop = previous - totalHealth;
            if (count == 0 || previous < 0.f || totalHealth < 0.f) return;
            if (!ownHit && otherHitDamage <= 0.f) return;
            // Armor like full netherite with Protection lets very little of a hit through, so a hit you just threw can
            // be a drop of only a tenth of a heart or so. Hits you didn't throw need a clearer drop to be told apart
            // from anything else (poison, fire, hunger).
            if (drop < (ownHit ? 0.05f : 0.3f)) return;

            // How much of a hit gets through their armor: armor points, then Protection (IV on every enchanted piece).
            const float protection = assume.protection ? 1.f - std::min(20.f, 4.f * enchantedPieces) * 0.04f : 1.f;
            auto getsThrough = [&](float raw) {
                const float reduction =
                    std::clamp(std::max(defense / 5.f, defense - raw / (2.f + toughness / 4.f)), 0.f, 20.f) / 25.f;
                return std::max(0.03f, (1.f - reduction) * protection);
            };

            float raw;
            if (ownHit && weapon.base >= 0.f) {
                // We know the weapon: its damage (x1.5 on a crit), Sharpness on a sword or axe (the level read from the
                // item, or V = +3 when an enchanted one can't be read), Strength = +3 per level (your potion, or the
                // extra level from the settings), Weakness = -4 per level.
                const int strength = std::max(weapon.strength, assume.strength);
                float sharpness = 0.f;
                if (weapon.sharpable && weapon.enchanted) {
                    if (weapon.sharpness >= 0) sharpness = EnchantNbt::sharpnessBonus(weapon.sharpness);
                    else if (assume.sharpness) sharpness = 3.f;
                }
                raw = (weapon.crit ? weapon.base * 1.5f : weapon.base) + sharpness +
                      3.f * static_cast<float>(strength) - 4.f * static_cast<float>(weapon.weakness);
                raw = std::max(raw, 1.f);
            } else if (!ownHit) {
                raw = otherHitDamage;
            } else {
                // Unknown weapon: work back from how much health it took.
                raw = drop * 3.f;
                for (int k = 0; k < 4; ++k) raw = std::min(40.f, drop / getsThrough(raw));
                raw = std::max(raw, 1.f);
            }
            if (ownHit ? weapon.base >= 0.f : true) {
                // A weak swing or a block shows up as a smaller drop than expected, so scale down by that. This also
                // corrects a crit that was guessed wrong. Never scale up: the target may not have Protection at all.
                const float expected = raw * getsThrough(raw);
                raw *= std::clamp(drop / expected, 0.2f, 1.f);
            }

            // The small extra keeps a damage of exactly 12 (3 wear) from reading as 11.9999 (2 wear) after the scaling.
            const int perPiece = std::max(1, static_cast<int>(std::floor(raw / 4.f + 0.01f)));
            for (auto& piece : pieces) {
                if (!piece.known) continue;
                // Unbreaking III ignores 30% of the wear, and only an enchanted (glowing) piece can have it.
                const float kept = assume.unbreaking && piece.glint ? 0.7f : 1.f;
                piece.wear += static_cast<float>(perPiece) * kept * assume.wearScale;
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
            bool glint = false; // It was enchanted (glowing) when we saw it; a change means a different piece.
            int lastServerDamage = 0;
            int baseDamage = 0; // What the server last said, the guess builds on this.
            float wear = 0.f;   // Extra durability lost since then, from the hits.
        };
        Piece pieces[4];
        float lastHealth = -1.f; // Health + golden hearts at the last update.
        float lastLife = -1.f;   // Health alone at the last update, for spotting a death or a respawn.
    };
    // The four armor stacks of a player (null = empty slot or nothing that wears out).
    inline void readArmorStacks(SDK::Actor* actor, SDK::ItemStack* (&out)[4]) {
        for (auto*& stack : out) stack = nullptr;
        auto* equipment = actor->tryGetComponent<SDK::ActorEquipmentComponent>();
        if (!equipment || !equipment->armorContainer) return;
        for (int slot = 0; slot < 4; ++slot) {
            auto* stack = actor->getArmor(slot);
            if (stack && stack->getItem()) out[slot] = stack;
        }
    }

    // One Tracker for every player the game has loaded, kept up to date every tick whether or not anything is shown.
    // That way no hit is missed while a player is out of the Target HUD, out of nametag range, or you switched to
    // fighting someone else: their wear keeps adding up and is still there when you look at them again.
    // A tracker is only dropped after the player has been gone for a couple of minutes.
    class Trackers {
    public:
        // Call once per game tick. otherHitDamage: assumed damage for hits you did not throw (0 = ignore them).
        void tick(SDK::Level* level, SDK::Actor* me, Assumptions const& assume, float otherHitDamage) {
            if (!level || !me) return;
            ++counter;

            std::vector<uint64_t> syncs;
            {
                std::lock_guard lock(mutex);
                syncs.swap(pendingSyncs);
            }

            const auto now = std::chrono::steady_clock::now();
            for (auto it = ownHits.begin(); it != ownHits.end();) {
                it = now - it->second.when > std::chrono::seconds(5) ? ownHits.erase(it) : std::next(it);
            }

            for (SDK::Actor* actor : level->getRuntimeActorList()) {
                if (!actor || actor == me || !actor->isPlayer()) continue;

                const uint64_t id = actor->getRuntimeID();
                auto& entry = entries[id];
                entry.seen = counter;
                if (std::find(syncs.begin(), syncs.end(), id) != syncs.end()) entry.tracker.resync();

                const auto vitals = readVitals(actor);
                const float total = vitals.health < 0.f ? -1.f : vitals.health + vitals.absorption;
                const auto swing = ownHits.find(id);
                const bool own = swing != ownHits.end() && now - swing->second.when <= std::chrono::seconds(1);

                SDK::ItemStack* stacks[4];
                readArmorStacks(actor, stacks);
                entry.tracker.update(stacks, total, own, own ? swing->second.weapon : Weapon {}, otherHitDamage, assume,
                                     vitals.health);
            }

            // Drop players that have been gone for about two minutes.
            for (auto it = entries.begin(); it != entries.end();) {
                it = counter - it->second.seen > 2400 ? entries.erase(it) : std::next(it);
            }
        }

        // You just swung at this player with this weapon.
        void noteOwnHit(uint64_t id, Weapon const& weapon) { ownHits[id] = { std::chrono::steady_clock::now(), weapon }; }

        // The server sent this player's whole armor again. Safe to call from the packet thread.
        void noteArmorSync(uint64_t id) {
            std::lock_guard lock(mutex);
            if (pendingSyncs.size() < 64) pendingSyncs.push_back(id);
        }

        // The guess for a player, or null if they have not been seen.
        Tracker const* find(uint64_t id) const {
            auto it = entries.find(id);
            return it == entries.end() ? nullptr : &it->second.tracker;
        }

        void forget() {
            entries.clear();
            ownHits.clear();
            std::lock_guard lock(mutex);
            pendingSyncs.clear();
        }

    private:
        struct Entry {
            Tracker tracker;
            uint64_t seen = 0;
        };
        struct OwnHit {
            std::chrono::steady_clock::time_point when;
            Weapon weapon;
        };
        std::unordered_map<uint64_t, Entry> entries;
        std::unordered_map<uint64_t, OwnHit> ownHits;
        std::mutex mutex;
        std::vector<uint64_t> pendingSyncs;
        uint64_t counter = 0;
    };
}
