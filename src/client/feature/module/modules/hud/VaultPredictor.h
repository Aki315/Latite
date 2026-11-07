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
    class BlockLegacy;
    class BlockSource;
    class Packet;
}

// Draws the rewards of the trial vaults near you as item icons floating above each vault, in the world (not in a HUD box).
//
// What it can and can't do:
//   * It shows every vault within Max Distance (up to a few, nearest first): the ones the server has sent data about, and
//     the ones found by scanning the blocks around you (Scan for Vaults), so you don't have to go near them first. When
//     the type of a vault isn't known yet, both kinds are shown.
//   * Bedrock decides a vault's rewards from the level seed, the vault's position and the player (1.21.20 changelog:
//     "Loot ejected from Vaults in Trial Chambers are now dependent on level, position and player seed"). util/VaultLoot.h
//     works that out from the world seed, your unique actor id and the position, the way the game does. Rewards the server
//     sends in the vault data (items_to_eject) are shown as they are and take priority.
//   * Enchanted books are predicted with their enchantment. The enchantments on gear come from a separate random that has
//     nothing to do with the vault, so they can't be known ("random enchantments").
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

    // Where the on-screen alert sits (the first entry is the default).
    static constexpr int PosTopCenter = 0;
    static constexpr int PosTopLeft = 1;
    static constexpr int PosTopRight = 2;
    static constexpr int PosCenter = 3;
    static constexpr int PosBottomCenter = 4;
    static constexpr int PosBottomLeft = 5;
    static constexpr int PosBottomRight = 6;

    struct Entry {
        BlockPos pos {};
        vault_nbt::VaultData data;
        std::chrono::steady_clock::time_point seen {};
        // The server doesn't send the loot table, but the item spinning in the vault comes from it: seeing an item that only
        // one of the two tables can give tells which vault this is. Kept across updates.
        bool sawNormalOnly = false;
        bool sawOminousOnly = false;
        bool fromScan = false; // found by looking for vault blocks around you, not yet by a server update
    };

    void reset();
    void handleBlockActorData(SDK::Packet* pkt);
    void* findItemCounter(std::string const& id);
    void scanForVaults(SDK::BlockSource* region, Vec3 const& me, float maxDist);
    bool localPlayerId(int64_t& out) const; // The typed Player ID, else the one in the player list.

    EnumData vaultType;
    ValueType seed = TextValue(L"");           // Blank = the seed the server sent when you joined.
    ValueType playerId = TextValue(L"");       // Blank (the normal case) = your unique actor id, read from the player list.
    ValueType notifyEnabled = BoolValue(true);
    ValueType notifyItems = TextValue(L"heavy_core, wind_burst"); // Comma separated item ids / enchantments to be told about.
    ValueType notifyScreen = BoolValue(true);   // The alert box on the screen.
    ValueType notifyChat = BoolValue(true);     // A line in the chat.
    EnumData notifyPosition;
    ValueType notifyOffsetX = FloatValue(0.f);  // Moves the alert from its place, in pixels.
    ValueType notifyOffsetY = FloatValue(0.f);
    ValueType notifyTextSize = FloatValue(20.f);
    ValueType notifyDuration = FloatValue(8.f); // Seconds an alert stays.
    ValueType notifyPreview = BoolValue(false); // Keeps a sample alert on the screen so it can be placed.
    ValueType showNames = BoolValue(true);
    ValueType showAmounts = BoolValue(true);
    ValueType showEnchantments = BoolValue(true);
    ValueType showIcons = BoolValue(true);
    ValueType iconSize = FloatValue(40.f);     // Width and height of an item icon, in pixels at close range.
    ValueType textSize = FloatValue(20.f);
    ValueType showBackground = BoolValue(true);
    ValueType scanBlocks = BoolValue(true);    // Look for vault blocks within Max Distance, so vaults show before you are near.
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
    // Where the item registry was found: in which object (0 Level, 1 Minecraft, 2 ClientInstance), at which offset, and
    // whether the pointer there is the registry itself (0) or points at a pointer to it (1: at +0, 2: at +8).
    bool registryFound = false;
    int registryRoot = 0;
    size_t registryOffset = 0;
    int registryDeref = 0;
    size_t registryInner = 0; // For deref 3: where in the object found at registryOffset the registry pointer sits.
    int registryStep = 0;     // Search attempts made so far (4 = given up until you leave the world).
    size_t registryVecOff = 0x38;
    size_t registryNameOff = 0x128; // Where the item's id string ("minecraft:...") sits in an Item.
    std::chrono::steady_clock::time_point lastRegistryTry {};

    // Vault type from the vault block's own state bits (render thread only): block -> 0 normal, 1 ominous, -1 unknown.
    std::unordered_map<const void*, int> ominousByBlock;
    std::string stateNote;
    // Notifications (render thread only): vaults already announced, and the parsed list of things to be told about.
    std::unordered_set<uint64_t> notified;
    struct Alert {
        std::wstring title;  // What was found.
        std::wstring detail; // Which vault and where.
        std::chrono::steady_clock::time_point created;
    };
    std::vector<Alert> alerts; // Alerts on the screen, oldest first. Render thread only.
    std::wstring notifyParsedFrom;
    std::vector<std::string> notifyTerms;

    // The block scan (render thread only): a rolling sweep over the box around you, a few thousand blocks per frame.
    size_t scanIndex = 0;
    size_t scanSweeps = 0;
    size_t scanVaultsFound = 0;
    SDK::BlockLegacy* vaultLegacy = nullptr;
    std::unordered_set<void*> otherLegacy; // block types already checked and not a vault
    std::string scanReport = "not run yet";
    std::string registryNote;  // Why the item registry couldn't be read, or what it holds (Debug Mode). Render thread only.
    std::string iconReport;    // What happened to the icons last frame (Debug Mode). Render thread only.
};
