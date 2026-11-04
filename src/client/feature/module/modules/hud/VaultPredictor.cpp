#include "pch.h"
#include "VaultPredictor.h"

#include "client/Latite.h"
#include "client/event/events/LeaveGameEvent.h"
#include "client/event/events/PacketReceiveEvent.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/nbt/Tag.h"
#include "mc/common/network/Packet.h"
#include "mc/common/world/level/BlockSource.h"
#include "mc/common/world/level/HitResult.h"
#include "mc/common/world/level/block/Block.h"
#include "mc/common/world/level/block/BlockLegacy.h"
#include "util/Util.h"

#include <cstdlib>
#include <cstring>
#include <format>

namespace {
    constexpr uint8_t kStartGame = 0x0B;
    constexpr uint8_t kBlockActorData = 56;
    constexpr uint8_t kChangeDimension = 61;

    // Same layout as ContainerLogger (checked in a running game for the position, and the seed is where the .seed
    // command reads it): the Packet base class takes 0x30 bytes, then the fields.
    constexpr size_t kSeedOffset = 0x30;
    constexpr size_t kPosOffset = 0x30;
    constexpr size_t kTagOffset = 0x40;
    constexpr size_t kMaxVaults = 128;

    template<typename T>
    bool readRaw(const void* base, size_t offset, T& out) {
        __try {
            std::memcpy(&out, static_cast<const char*>(base) + offset, sizeof(T));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool plausiblePos(int x, int y, int z) {
        return std::abs(x) < 30000000 && std::abs(z) < 30000000 && y >= -2048 && y <= 4096;
    }

    uint64_t keyOf(int x, int y, int z) {
        return (static_cast<uint64_t>(x + (1 << 25)) << 38) | (static_cast<uint64_t>(z + (1 << 25)) << 12) |
               static_cast<uint64_t>(y + 1024);
    }

    bool looksLikeGameObject(const void* p) {
        uintptr_t vtable = 0;
        if (!readRaw(p, 0, vtable)) return false;
        static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        return vtable > base && vtable < base + 0x40000000ull;
    }

    void printTagInner(SDK::Tag* tag, SDK::BasicPrintStream* out) { tag->print("", *out); }

    bool printTag(SDK::Tag* tag, SDK::BasicPrintStream* out) {
        __try {
            if (tag->getId() != 10) return false; // a compound tag
            printTagInner(tag, out);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // The name of the block at a position, e.g. "minecraft:vault".
    std::string blockNameInner(BlockPos const& pos) {
        auto* ci = SDK::ClientInstance::get();
        auto* region = ci ? ci->getRegion() : nullptr;
        if (!region) return {};
        SDK::Block* block = region->getBlock(pos);
        if (!block || !block->legacyBlock) return {};
        return block->legacyBlock->namespacedId.getString();
    }

    // The 64-bit number the user typed, if it is one.
    bool parseSeed(std::wstring const& text, int64_t& out) {
        if (text.empty()) return false;
        wchar_t* end = nullptr;
        const long long value = std::wcstoll(text.c_str(), &end, 10);
        if (end == text.c_str() || (end && *end != L'\0')) return false;
        out = value;
        return true;
    }
} // namespace

VaultPredictor::VaultPredictor()
    : TextModule("VaultPredictor", LocalizeString::get("client.textmodule.vaultPredictor.name"),
                 LocalizeString::get("client.textmodule.vaultPredictor.desc"), HUD) {
    vaultType.addEntry(EnumEntry(TypeBoth, LocalizeString::get("client.textmodule.vaultPredictor.type.both.name"),
                                 LocalizeString::get("client.textmodule.vaultPredictor.type.both.desc")));
    vaultType.addEntry(EnumEntry(TypeNormal, LocalizeString::get("client.textmodule.vaultPredictor.type.normal.name"),
                                 LocalizeString::get("client.textmodule.vaultPredictor.type.normal.desc")));
    vaultType.addEntry(EnumEntry(TypeOminous, LocalizeString::get("client.textmodule.vaultPredictor.type.ominous.name"),
                                 LocalizeString::get("client.textmodule.vaultPredictor.type.ominous.desc")));
    addEnumSetting("vaultType", LocalizeString::get("client.textmodule.vaultPredictor.vaultType.name"),
                   LocalizeString::get("client.textmodule.vaultPredictor.vaultType.desc"), vaultType);
    addSetting("seed", LocalizeString::get("client.textmodule.vaultPredictor.seed.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.seed.desc"), seed);
    addSetting("showNames", LocalizeString::get("client.textmodule.vaultPredictor.showNames.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.showNames.desc"), showNames);
    addSetting("showAmounts", LocalizeString::get("client.textmodule.vaultPredictor.showAmounts.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.showAmounts.desc"), showAmounts);
    addSetting("showEnchantments", LocalizeString::get("client.textmodule.vaultPredictor.showEnchantments.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.showEnchantments.desc"), showEnchantments);
    addSetting("showCoords", LocalizeString::get("client.textmodule.vaultPredictor.showCoords.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.showCoords.desc"), showCoords);
    addSetting("debugMode", LocalizeString::get("client.textmodule.vaultPredictor.debugMode.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.debugMode.desc"), debugMode);

    // The seed packet comes once, when you join, so the server's seed is only known if the module was on at that moment.
    listen<PacketReceiveEvent>((EventListenerFunc)&VaultPredictor::onPacketReceive);
    listen<LeaveGameEvent>((EventListenerFunc)&VaultPredictor::onLeaveGame, true);
}

void VaultPredictor::onEnable() { reset(); }
void VaultPredictor::onLeaveGame(Event&) {
    reset();
    std::lock_guard lock(mutex);
    haveServerSeed = false;
}

void VaultPredictor::reset() {
    std::lock_guard lock(mutex);
    vaults.clear();
    loggedNoData.clear();
    ignoredIds.clear();
}

void VaultPredictor::onPacketReceive(Event& evG) {
    auto& ev = reinterpret_cast<PacketReceiveEvent&>(evG);
    SDK::Packet* pkt = ev.getPacket();
    if (!pkt) return;

    switch (static_cast<uint8_t>(pkt->getID())) {
    case kStartGame: {
        int64_t value = 0;
        if (readRaw(pkt, kSeedOffset, value)) {
            std::lock_guard lock(mutex);
            serverSeed = value;
            haveServerSeed = true;
            if (std::get<BoolValue>(debugMode)) Logger::Info("[VaultPredictor] world seed from the server: {}", value);
        }
        break;
    }
    case kBlockActorData:
        handleBlockActorData(pkt);
        break;
    case kChangeDimension:
        reset();
        break;
    default:
        break;
    }
}

void VaultPredictor::handleBlockActorData(SDK::Packet* pkt) {
    int x = 0, y = 0, z = 0;
    if (!readRaw(pkt, kPosOffset, x) || !readRaw(pkt, kPosOffset + 4, y) || !readRaw(pkt, kPosOffset + 8, z) ||
        !plausiblePos(x, y, z))
        return;

    auto* tag = reinterpret_cast<SDK::Tag*>(reinterpret_cast<char*>(pkt) + kTagOffset);
    if (!looksLikeGameObject(tag)) return;
    SDK::BasicPrintStream out;
    if (!printTag(tag, &out) || out.mStr.empty()) return;

    auto data = vault_nbt::parse(out.mStr);
    if (!data.isVault) {
        // Debug: say which other block entities arrive, once per kind, so it is clear the packet works at all.
        if (std::get<BoolValue>(debugMode)) {
            const std::string id = out.mStr.substr(0, 60);
            std::lock_guard lock(mutex);
            if (ignoredIds.size() < 20 && ignoredIds.insert(id).second) {
                Logger::Info("[VaultPredictor] block entity data at {} {} {} (not a vault): {}", x, y, z, id);
            }
        }
        return;
    }

    if (std::get<BoolValue>(debugMode)) {
        Logger::Info("[VaultPredictor] vault data at {} {} {}:\n{}\n  -> loot table '{}', override '{}', display item {} x{}, "
                     "items_to_eject field {} with {} item(s)",
                     x, y, z, out.mStr.substr(0, 2500), data.lootTable, data.overrideLootTable,
                     data.hasDisplay ? data.display.name : std::string("none"), data.hasDisplay ? data.display.count : 0,
                     data.hasEjectField ? "present" : "absent", data.toEject.size());
    }

    std::lock_guard lock(mutex);
    if (vaults.size() > kMaxVaults) vaults.clear();
    auto& entry = vaults[keyOf(x, y, z)];
    // An update that leaves a field out must not wipe what an earlier update said about it.
    if (data.lootTable.empty()) data.lootTable = entry.data.lootTable;
    if (data.overrideLootTable.empty()) data.overrideLootTable = entry.data.overrideLootTable;
    if (!data.hasDisplay && entry.data.hasDisplay) {
        data.hasDisplay = true;
        data.display = entry.data.display;
    }
    entry.data = std::move(data);
    entry.seen = std::chrono::steady_clock::now();
}

void VaultPredictor::appendItem(std::wstring& out, vault_nbt::ItemInfo const& item, bool indent) {
    const std::wstring pad = indent ? L"  " : L"";
    std::wstring line = pad;
    if (std::get<BoolValue>(showNames)) {
        line += L"\u00A7f" + util::StrToWStr(vault_nbt::prettyName(item.name)) + L"\u00A7r";
    } else {
        line += L"\u00A7fItem\u00A7r";
    }
    if (std::get<BoolValue>(showAmounts)) line += L" \u00A77x" + std::to_wstring(item.count) + L"\u00A7r";
    out += line + L"\n";

    if (std::get<BoolValue>(showEnchantments)) {
        for (auto const& e : item.enchants) {
            out += pad + L"  \u00A7d" + util::StrToWStr(vault_nbt::enchantName(e.id)) + L" " +
                   util::StrToWStr(vault_nbt::roman(e.level)) + L"\u00A7r\n";
        }
    }
}

std::wstring VaultPredictor::buildText(bool preview) {
    const int wanted = vaultType.getSelectedKey();

    // What the seed setting and the server's seed say.
    std::wstring seedLine;
    {
        int64_t value = 0;
        const bool typed = parseSeed(std::get<TextValue>(seed).str, value);
        std::lock_guard lock(mutex);
        if (typed) seedLine = L"\u00A77Seed: " + std::to_wstring(value) + L" (typed)\u00A7r\n";
        else if (haveServerSeed) seedLine = L"\u00A77Seed: " + std::to_wstring(serverSeed) + L" (from the server)\u00A7r\n";
        else seedLine = L"\u00A77Seed: unknown\u00A7r\n";
    }

    if (preview) {
        std::wstring out = L"\u00A76Vault Predictor\u00A7r\n\u00A7bNormal Vault\u00A7r\n";
        if (std::get<BoolValue>(showCoords)) out += L"\u00A77" L"120  64  -120\u00A7r\n";
        out += seedLine;
        out += L"\u00A77Rewards sent by the server:\u00A7r\n";
        vault_nbt::ItemInfo book;
        book.name = "minecraft:enchanted_book";
        book.count = 1;
        book.enchants.push_back({ 9, 5 });
        appendItem(out, book, true);
        vault_nbt::ItemInfo emerald;
        emerald.name = "minecraft:emerald";
        emerald.count = 3;
        appendItem(out, emerald, true);
        out += L"\u00A78(sample)\u00A7r";
        return out;
    }

    auto* client = SDK::ClientInstance::get();
    auto* level = client && client->minecraft ? client->minecraft->getLevel() : nullptr;
    auto* hit = level ? level->getHitResult() : nullptr;
    if (!hit || hit->hitType != SDK::HitType::BLOCK) return {};

    const BlockPos pos = hit->hitBlock;
    if (blockNameInner(pos) != "minecraft:vault") return {};

    const uint64_t key = keyOf(pos.x, pos.y, pos.z);
    vault_nbt::VaultData data;
    bool known = false;
    {
        std::lock_guard lock(mutex);
        if (auto it = vaults.find(key); it != vaults.end()) {
            data = it->second.data;
            known = true;
        } else if (std::get<BoolValue>(debugMode) && loggedNoData.size() < 64 && loggedNoData.insert(key).second) {
            Logger::Info("[VaultPredictor] looking at a vault at {} {} {}: no data for it from the server yet", pos.x, pos.y,
                         pos.z);
        }
    }

    // The type, from the loot table. Without data the type isn't known, and then it is shown whatever the setting says.
    const bool ominous = known && vault_nbt::isOminous(data);
    if (known && !data.lootTable.empty()) {
        if (wanted == TypeNormal && ominous) return {};
        if (wanted == TypeOminous && !ominous) return {};
    }

    std::wstring out = L"\u00A76Vault Predictor\u00A7r\n";
    if (known && !data.lootTable.empty()) out += ominous ? L"\u00A75Ominous Vault\u00A7r\n" : L"\u00A7bNormal Vault\u00A7r\n";
    else out += L"\u00A77Vault (type not received)\u00A7r\n";
    if (std::get<BoolValue>(showCoords)) {
        out += L"\u00A77" + std::to_wstring(pos.x) + L"  " + std::to_wstring(pos.y) + L"  " + std::to_wstring(pos.z) +
               L"\u00A7r\n";
    }
    out += seedLine;

    if (!known) {
        out += L"\u00A77No data from the server for this vault yet.\u00A7r";
        return out;
    }

    if (!data.toEject.empty()) {
        out += L"\u00A7aRewards sent by the server:\u00A7r\n";
        for (auto const& item : data.toEject) appendItem(out, item, true);
    } else {
        out += L"\u00A77Rewards: the server hasn't sent any for this vault.\u00A7r\n";
    }
    if (data.hasDisplay) {
        out += L"\u00A78Shown in the vault (not a reward):\u00A7r\n";
        appendItem(out, data.display, true);
    }
    while (!out.empty() && out.back() == L'\n') out.pop_back();
    return out;
}

void VaultPredictor::render(DrawUtil& dc, bool isDefault, bool inEditor) {
    pending = buildText(isDefault || inEditor);
    if (pending.empty()) return; // Not looking at a (wanted) vault: nothing is drawn.
    TextModule::render(dc, isDefault, inEditor);
}

std::wstringstream VaultPredictor::text(bool, bool) {
    std::wstringstream ss;
    ss << pending;
    return ss;
}
