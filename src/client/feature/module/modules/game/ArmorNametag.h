#pragma once

#include "client/feature/module/Module.h"

#include <array>
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
//  - Estimated: additionally subtracts 1 point per observed hit, marked with "~",
//               and resets its baseline when a MobArmorEquipmentPacket is received.
//               Remote-player durability still cannot be exact if the server sends no update.
class ArmorNametag final : public Module {
public:
    ArmorNametag();
    ~ArmorNametag() override = default;

    void onUpdate(Event& event);
    void onPacketReceive(Event& event);
    void onDisable() override;

private:
    struct SlotEstimate {
        int lastActual = -1; // Last synced remaining durability we saw (-1 = none yet).
        int loss = 0;        // Estimated points lost since that sync.
    };
    struct PlayerEstimate {
        std::array<SlotEstimate, ArmorNametagDetail::kSlotCount> slots{};
        uint64_t lastSeen = 0; // updateCounter value of the last update that touched this player.
    };

    ArmorNametagDetail::SlotLosses estimateLosses(uint64_t runtimeId, ArmorNametagDetail::ArmorSnapshot const& armor,
                                                  int newHits, bool authoritativeSync);

    EnumData durabilityDisplayMode;
    EnumData durabilitySource;
    ValueType maxDistance = FloatValue(32.f); // Blocks; 0 = unlimited.

    // Estimated mode only.
    std::unordered_map<uint64_t, PlayerEstimate> estimates;
    uint64_t updateCounter = 0;

    // Hurt events arrive in the packet handler and are drained by onUpdate.
    std::mutex hitMutex;
    std::unordered_map<uint64_t, int> pendingHits;
    std::unordered_map<uint64_t, int> hitsScratch; // Reused every update to avoid reallocating.
    std::unordered_map<uint64_t, bool> pendingArmorSyncs;
    std::unordered_map<uint64_t, bool> armorSyncScratch;
};
