#pragma once
#include "../../TextModule.h"

#include "util/VaultNbt.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace SDK {
    class Packet;
}

// Shows what your game knows about the trial vault (or ominous vault) you are looking at.
//
// What it can and can't do:
//   * It reads the vault's block entity data, which the server sends to your client: the loot table (that is what tells a
//     normal vault from an ominous one), the item shown in the vault right now (NOT a reward: the game picks it at
//     random once a second), and the rewards list (items_to_eject) when the server has filled it.
//   * Rewards are only listed when the server really sent them. Nothing is calculated from the seed: no verified
//     Bedrock algorithm exists that turns a world seed into a vault's rewards, and this module never shows a guess as
//     if it were a prediction. The seed setting is shown on the HUD and written to the log for that reason.
//   * Debug Mode writes the raw vault data to the log (latest.log), which is how to see what the server sends.
//
// It is a TextModule, so position, size, colors and background are set in Latite's HUD editor.
class VaultPredictor final : public TextModule {
public:
    VaultPredictor();
    ~VaultPredictor() override = default;

    void render(DrawUtil& ctx, bool isDefault, bool inEditor) override;

    void onPacketReceive(Event& event);
    void onLeaveGame(Event& event);
    void onEnable() override;

protected:
    std::wstringstream text(bool isDefault, bool inEditor) override;

private:
    // The number is the entry's position in the list, and the first entry is the default: Both.
    static constexpr int TypeBoth = 0;
    static constexpr int TypeNormal = 1;
    static constexpr int TypeOminous = 2;

    struct Entry {
        vault_nbt::VaultData data;
        std::chrono::steady_clock::time_point seen {};
    };

    void reset();
    void handleBlockActorData(SDK::Packet* pkt);
    std::wstring buildText(bool preview);
    void appendItem(std::wstring& out, vault_nbt::ItemInfo const& item, bool indent);

    EnumData vaultType;
    ValueType seed = TextValue(L"");           // Blank = the seed the server sent when you joined.
    ValueType showNames = BoolValue(true);
    ValueType showAmounts = BoolValue(true);
    ValueType showEnchantments = BoolValue(true);
    ValueType showCoords = BoolValue(true);
    ValueType debugMode = BoolValue(false);

    std::mutex mutex;
    std::unordered_map<uint64_t, Entry> vaults; // Bounded, cleared on leaving the world or changing dimension.
    std::unordered_set<uint64_t> loggedNoData;
    bool haveServerSeed = false;
    int64_t serverSeed = 0;
    std::unordered_set<std::string> ignoredIds; // Block entity ids already mentioned in the log (Debug Mode).
    std::wstring pending;                       // The text for this frame, built by render().
};
