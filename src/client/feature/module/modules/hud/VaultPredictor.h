#pragma once
#include "../../Module.h"

#include "util/VaultNbt.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SDK {
    class Block;
    class Packet;
}

// Draws the rewards of the trial vaults near you as item icons floating above each vault, in the world (not in a HUD box).
//
// What it can and can't do:
//   * It shows every vault the server has sent data about that is within Max Distance (up to a few, nearest first).
//   * Bedrock decides a vault's rewards from the level, the vault's position and a player seed (1.21.20 changelog:
//     "Loot ejected from Vaults in Trial Chambers are now dependent on level, position and player seed"). The formula
//     isn't public and nothing here calculates it. The rewards shown are the ones in the vault data the server sends to
//     your game (items_to_eject); when the server sends none, the vault shows only its type and distance.
//   * The item shown inside the vault (display_item) is not a reward and is only shown, labelled, when no rewards are known.
//   * Debug Mode writes the raw vault data to the log (latest.log), which is how to see what the server sends.
class VaultPredictor final : public Module {
public:
    VaultPredictor();
    ~VaultPredictor() override = default;

    void onRenderLayer(Event& event);
    void onPacketReceive(Event& event);
    void onLeaveGame(Event& event);
    void onEnable() override;

private:
    // The number is the entry's position in the list, and the first entry is the default: Both.
    static constexpr int TypeBoth = 0;
    static constexpr int TypeNormal = 1;
    static constexpr int TypeOminous = 2;

    struct Entry {
        BlockPos pos {};
        vault_nbt::VaultData data;
        std::chrono::steady_clock::time_point seen {};
    };

    void reset();
    void handleBlockActorData(SDK::Packet* pkt);
    void* findItemCounter(std::string const& id);

    EnumData vaultType;
    ValueType seed = TextValue(L"");           // Blank = the seed the server sent when you joined.
    ValueType showNames = BoolValue(true);
    ValueType showAmounts = BoolValue(true);
    ValueType showEnchantments = BoolValue(true);
    ValueType showIcons = BoolValue(true);
    ValueType iconSize = FloatValue(40.f);     // Width and height of an item icon, in pixels at close range.
    ValueType textSize = FloatValue(20.f);
    ValueType showBackground = BoolValue(true);
    ValueType maxDistance = FloatValue(16.f);  // Vaults further away than this (in blocks) are not shown.
    ValueType debugMode = BoolValue(false);

    std::mutex mutex;
    std::unordered_map<uint64_t, Entry> vaults; // Bounded, cleared on leaving the world or changing dimension.
    std::unordered_set<std::string> ignoredIds; // Block entity kinds already mentioned in the log (Debug Mode).
    bool haveServerSeed = false;
    int64_t serverSeed = 0;

    // Item id -> the game's item counter (what an ItemStack points at), built from the item registry. Render thread only.
    std::unordered_map<std::string, void*> itemCache;
    void* itemCacheRegistry = nullptr;
};
