#pragma once

#include "client/feature/module/Module.h"
#include "util/ArmorEstimate.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace ArmorNametagDetail {
constexpr int kSlotCount = 4; // helmet, chestplate, leggings, boots

struct ArmorDurability {
    int remaining;
    int maxDamage;
};

// One entry per armor slot; empty = nothing damageable equipped there.
using ArmorSnapshot = std::array<std::optional<ArmorDurability>, kSlotCount>;
// Estimated points lost per slot since the last durability value the server synced.
using SlotLosses = std::array<int, kSlotCount>;
} // namespace ArmorNametagDetail

// Appends a one-line armor durability readout beneath other players' nametags.
//
// Every line this module adds starts with a unique marker (see ArmorNametag.cpp),
// so it can find and strip its own suffix from any nametag at any time.
//
// Durability Source:
//  - Actual:    shows the last durability the server synced to this client.
//  - Estimated: works like the Target HUD (see util/ArmorEstimate.h). It watches every player's health, and when it
//               drops after you swung at them, it uses your weapon, its Sharpness level (read from the item) and your Strength to work out the wear.
//               Hits you didn't throw use the assumed Weapon Damage settings. Estimates are marked with "~", and
//               reset when the server sends the player's armor again.
class ArmorNametag final : public Module {
public:
    ArmorNametag();
    ~ArmorNametag() override = default;

    void onUpdate(Event& event);
    void onPacketReceive(Event& event);
    void onAttack(Event& event);
    void onDisable() override;

private:
    EnumData durabilityDisplayMode;
    EnumData durabilitySource;
    ValueType maxDistance = FloatValue(32.f); // Blocks; 0 = unlimited.

    // What a hit you did NOT throw is assumed to be (Estimated mode only), and what their armor is assumed to have.
    ValueType weaponDamage = FloatValue(7.f);      // Base damage of the weapon: 7 = diamond sword, 8 = netherite.
    ValueType assumeSharpness = BoolValue(true);   // Fallback: Sharpness V (+3 damage) when your weapon's level can't be read from the item.
    ValueType strengthLevel = FloatValue(0.f);     // Strength potion level, +3 damage per level (also for your hits).
    ValueType assumeProtection = BoolValue(true);  // Their armor is Protection IV.
    ValueType assumeUnbreaking = BoolValue(true);  // Their armor is Unbreaking III: about 30% less wear.
    ValueType wearScale = FloatValue(1.f);         // Multiplies the wear of every hit.

    // Estimated mode only: one guess per player, followed every tick (also for players out of nametag range).
    ArmorEstimate::Trackers trackers;
};
