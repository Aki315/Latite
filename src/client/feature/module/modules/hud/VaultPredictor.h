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
//   * Bedrock decides a vault's rewards from the level seed, the vault's position and the player (1.21.20 changelog:
//     "Loot ejected from Vaults in Trial Chambers are now dependent on level, position and player seed"). util/VaultLoot.h
//     works that out from the world seed, your unique actor id and the position, the way the game does. Rewards the server
//     sends in the vault data (items_to_eject) are shown as they are and take priority.
//   * Enchanted items can't be predicted yet (their code isn't decoded): the prediction stops at the first one.
//   * The item shown inside the vault (display_item) is not a reward and is only shown, labelled, when no rewards are known.
//   * Debug Mode writes the raw vault data to the log (latest.log), which is how to see what the server sends.
class VaultPredictor final : public Module {
public:
    VaultPredictor();
    ~VaultPredictor() override = default;

    void onRenderLayer(Event& event);
    void onPacketReceive(Event& event);
    void onLeaveGame(Event& event);

private:
    // The number is the entry's position in the list, and the first entry is the default: Both.
    static constexpr int TypeBoth = 0;
    static constexpr int TypeNormal = 1;
    static constexpr int TypeOminous = 2;

    struct Entry {
        BlockPos pos {};
        vault_nbt::VaultData data;
        std::chrono::steady_clock::time_point seen {};
        // The server doesn't send the loot table, but the item spinning in the vault comes from it: seeing an item that only
        // one of the two tables can give tells which vault this is. Kept across updates.
        bool sawNormalOnly = false;
        bool sawOminousOnly = false;
    };

    void reset();
    void handleBlockActorData(SDK::Packet* pkt);
    void* findItemCounter(std::string const& id);
    bool localPlayerId(int64_t& out) const; // The typed Player ID, else the one in the player list.

    EnumData vaultType;
    ValueType seed = TextValue(L"");           // Blank = the seed the server sent when you joined.
    ValueType playerId = TextValue(L"");       // Blank = your unique actor id, read from the player list.
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
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastDiag; // Debug Mode, render thread only.
    void* itemCacheRegistry = nullptr;
    std::string registryNote;  // Why the item registry couldn't be read, or what it holds (Debug Mode). Render thread only.
    std::string iconReport;    // What happened to the icons last frame (Debug Mode). Render thread only.
};
