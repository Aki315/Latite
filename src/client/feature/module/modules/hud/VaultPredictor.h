#pragma once
#include "../../TextModule.h"

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

// Shows what your game knows about the trial vault (or ominous vault) you are looking at.
//
// What it can and can't do:
//   * It shows the vault in your crosshair or, when there is none, the nearest vault the server has sent data for within
//     Max Distance.
//   * It reads the vault's block entity data, which the server sends to your client: the loot table (that is what tells a
//     normal vault from an ominous one), the item shown in the vault right now (NOT a reward: the game picks it at
//     random once a second), and the rewards list (items_to_eject) when the server has filled it.
//   * Rewards are only listed when the server really sent them. Nothing is calculated from the seed: no verified
//     Bedrock algorithm exists that turns a world seed into a vault's rewards, and this module never shows a guess as
//     if it were a prediction. The seed setting is shown on the HUD and written to the log for that reason.
//   * Every reward and the shown item get its icon, drawn the same way Latite's WAILA draws item icons.
//   * Debug Mode writes the raw vault data to the log (latest.log), which is how to see what the server sends.
//
// It is a TextModule, so position, size, colors and background are set in Latite's HUD editor.
class VaultPredictor final : public TextModule {
public:
    VaultPredictor();
    ~VaultPredictor() override = default;

    void render(DrawUtil& ctx, bool isDefault, bool inEditor) override;

    // Item icons are game textures, so they can only be drawn with the Minecraft renderer.
    bool forceMinecraftRenderer() override { return true; }

    void onPacketReceive(Event& event);
    void onLeaveGame(Event& event);
    void onEnable() override;

protected:
    std::wstringstream text(bool, bool) override { return {}; } // Everything is drawn in render().

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

    // One line of the HUD. Item lines get an icon in front, detail lines (enchantments) are indented under their item.
    enum class RowKind { Header, Item, Detail };
    struct Row {
        std::wstring text;
        RowKind kind = RowKind::Header;
        std::string item; // The item's id for an icon ("minecraft:diamond"); empty for none.
        int count = 1;
    };

    void reset();
    void handleBlockActorData(SDK::Packet* pkt);
    // The lines to show for what the player is looking at (or a sample). Empty = nothing to draw. `block` gets the
    // vault block, which the icons are built from.
    std::vector<Row> buildRows(bool preview, SDK::Block*& block);
    void addItem(std::vector<Row>& rows, vault_nbt::ItemInfo const& item);
    void* findItemCounter(std::string const& id);

    EnumData vaultType;
    ValueType seed = TextValue(L"");           // Blank = the seed the server sent when you joined.
    ValueType showNames = BoolValue(true);
    ValueType showAmounts = BoolValue(true);
    ValueType showEnchantments = BoolValue(true);
    ValueType maxDistance = FloatValue(16.f);  // Vaults further away than this (in blocks) are not shown.
    ValueType showIcons = BoolValue(true);
    ValueType iconSize = FloatValue(32.f);     // Width and height of an item icon, in the same units as the text size.
    ValueType debugMode = BoolValue(false);

    std::mutex mutex;
    std::unordered_map<uint64_t, Entry> vaults; // Bounded, cleared on leaving the world or changing dimension.
    std::unordered_set<uint64_t> loggedNoData;
    bool haveServerSeed = false;
    int64_t serverSeed = 0;
    std::unordered_set<std::string> ignoredIds; // Block entity ids already mentioned in the log (Debug Mode).

    // Item id -> the game's item counter (what an ItemStack points at), built from the item registry. Render thread only.
    std::unordered_map<std::string, void*> itemCache;
    void* itemCacheRegistry = nullptr;
};
