#include "pch.h"
#include "VaultPredictor.h"

#include "client/Latite.h"
#include "client/event/events/LeaveGameEvent.h"
#include "client/event/events/PacketReceiveEvent.h"
#include "client/event/events/RenderLayerEvent.h"
#include "mc/common/client/game/FontRepository.h"
#include "mc/common/client/gui/controls/UIControl.h"
#include "mc/common/client/gui/controls/VisualTree.h"
#include "util/DrawContext.h"
#include "util/WorldToScreen.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/nbt/Tag.h"
#include "mc/Addresses.h"
#include "mc/common/network/Packet.h"
#include "mc/common/world/Item.h"
#include "mc/common/world/ItemStack.h"
#include "mc/common/world/Minecraft.h"
#include "mc/common/world/actor/player/Player.h"
#include "mc/common/world/actor/player/PlayerListEntry.h"
#include "mc/common/world/level/Level.h"
#include "mc/common/world/level/BlockSource.h"
#include "mc/common/world/level/HitResult.h"
#include "mc/common/world/level/block/Block.h"
#include "mc/common/world/level/block/BlockLegacy.h"
#include "util/Util.h"
#include "util/VaultLoot.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
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
    constexpr size_t kMaxVaults = 512;

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

    // Items only one vault's loot table can produce (the display item is picked from it).
    bool normalOnlyItem(std::string const& n) {
        static const char* const items[] = { "honey_bottle", "iron_ingot", "golden_carrot", "bolt_armor_trim_smithing_template",
                                             "guster_banner_pattern", "music_disc_precipice", "trident", "shield", "iron_axe",
                                             "iron_chestplate", "bow" };
        for (auto const* i : items)
            if (n == i || n == std::string("minecraft:") + i) return true;
        return false;
    }

    bool ominousOnlyItem(std::string const& n) {
        static const char* const items[] = { "emerald_block", "iron_block", "diamond_block", "enchanted_golden_apple",
                                             "flow_armor_trim_smithing_template", "flow_banner_pattern", "music_disc_creator",
                                             "heavy_core" };
        for (auto const* i : items)
            if (n == i || n == std::string("minecraft:") + i) return true;
        return false;
    }

    enum class SeedStatus { Empty, Ok, Invalid, OutOfRange };

    // The whole text has to be one signed 64-bit whole number (spaces around it are fine). Nothing is ever turned into 0.
    SeedStatus parseSeed(std::wstring const& text, int64_t& out) {
        std::string s = util::WStrToStr(text);
        const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        if (s.empty()) return SeedStatus::Empty;

        const char* begin = s.data();
        const char* end = s.data() + s.size();
        if (*begin == '+') ++begin; // from_chars doesn't take a leading plus
        int64_t value = 0;
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        if (ec == std::errc::result_out_of_range) return SeedStatus::OutOfRange;
        if (ec != std::errc() || ptr != end) return SeedStatus::Invalid;
        out = value;
        return SeedStatus::Ok;
    }
} // namespace

VaultPredictor::VaultPredictor()
    : Module("VaultPredictor", LocalizeString::get("client.textmodule.vaultPredictor.name"),
             LocalizeString::get("client.textmodule.vaultPredictor.desc"), GAME) {
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
    addSetting("playerId", LocalizeString::get("client.textmodule.vaultPredictor.playerId.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.playerId.desc"), playerId);
    addSetting("notify", LocalizeString::get("client.textmodule.vaultPredictor.notify.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.notify.desc"), notifyEnabled);
    addSetting("notifyItems", LocalizeString::get("client.textmodule.vaultPredictor.notifyItems.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.notifyItems.desc"), notifyItems, "notify"_istrue);
    addSetting("notifyScreen", LocalizeString::get("client.textmodule.vaultPredictor.notifyScreen.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.notifyScreen.desc"), notifyScreen, "notify"_istrue);
    addSetting("notifyChat", LocalizeString::get("client.textmodule.vaultPredictor.notifyChat.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.notifyChat.desc"), notifyChat, "notify"_istrue);
    notifyPosition.addEntry(EnumEntry(PosTopCenter, LocalizeString::get("client.textmodule.vaultPredictor.pos.topCenter.name")));
    notifyPosition.addEntry(EnumEntry(PosTopLeft, LocalizeString::get("client.textmodule.vaultPredictor.pos.topLeft.name")));
    notifyPosition.addEntry(EnumEntry(PosTopRight, LocalizeString::get("client.textmodule.vaultPredictor.pos.topRight.name")));
    notifyPosition.addEntry(EnumEntry(PosCenter, LocalizeString::get("client.textmodule.vaultPredictor.pos.center.name")));
    notifyPosition.addEntry(EnumEntry(PosBottomCenter, LocalizeString::get("client.textmodule.vaultPredictor.pos.bottomCenter.name")));
    notifyPosition.addEntry(EnumEntry(PosBottomLeft, LocalizeString::get("client.textmodule.vaultPredictor.pos.bottomLeft.name")));
    notifyPosition.addEntry(EnumEntry(PosBottomRight, LocalizeString::get("client.textmodule.vaultPredictor.pos.bottomRight.name")));
    addEnumSetting("notifyPosition", LocalizeString::get("client.textmodule.vaultPredictor.notifyPosition.name"),
                   LocalizeString::get("client.textmodule.vaultPredictor.notifyPosition.desc"), notifyPosition);
    addSliderSetting("notifyOffsetX", LocalizeString::get("client.textmodule.vaultPredictor.notifyOffsetX.name"),
                     LocalizeString::get("client.textmodule.vaultPredictor.notifyOffsetX.desc"), notifyOffsetX, FloatValue(-800.f),
                     FloatValue(800.f), FloatValue(1.f), "notifyScreen"_istrue);
    addSliderSetting("notifyOffsetY", LocalizeString::get("client.textmodule.vaultPredictor.notifyOffsetY.name"),
                     LocalizeString::get("client.textmodule.vaultPredictor.notifyOffsetY.desc"), notifyOffsetY, FloatValue(-600.f),
                     FloatValue(600.f), FloatValue(1.f), "notifyScreen"_istrue);
    addSliderSetting("notifyTextSize", LocalizeString::get("client.textmodule.vaultPredictor.notifyTextSize.name"),
                     LocalizeString::get("client.textmodule.vaultPredictor.notifyTextSize.desc"), notifyTextSize, FloatValue(8.f),
                     FloatValue(48.f), FloatValue(1.f), "notifyScreen"_istrue);
    addSliderSetting("notifyDuration", LocalizeString::get("client.textmodule.vaultPredictor.notifyDuration.name"),
                     LocalizeString::get("client.textmodule.vaultPredictor.notifyDuration.desc"), notifyDuration, FloatValue(2.f),
                     FloatValue(60.f), FloatValue(1.f), "notifyScreen"_istrue);
    addSetting("notifyPreview", LocalizeString::get("client.textmodule.vaultPredictor.notifyPreview.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.notifyPreview.desc"), notifyPreview, "notifyScreen"_istrue);
    addSetting("showNames", LocalizeString::get("client.textmodule.vaultPredictor.showNames.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.showNames.desc"), showNames);
    addSetting("showAmounts", LocalizeString::get("client.textmodule.vaultPredictor.showAmounts.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.showAmounts.desc"), showAmounts);
    addSetting("showEnchantments", LocalizeString::get("client.textmodule.vaultPredictor.showEnchantments.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.showEnchantments.desc"), showEnchantments);
    addSliderSetting("maxDistance", LocalizeString::get("client.textmodule.vaultPredictor.maxDistance.name"),
                     LocalizeString::get("client.textmodule.vaultPredictor.maxDistance.desc"), maxDistance,
                     FloatValue(2.f), FloatValue(128.f), FloatValue(1.f));
    addSetting("showIcons", LocalizeString::get("client.textmodule.vaultPredictor.showIcons.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.showIcons.desc"), showIcons);
    addSliderSetting("iconSize", LocalizeString::get("client.textmodule.vaultPredictor.iconSize.name"),
                     LocalizeString::get("client.textmodule.vaultPredictor.iconSize.desc"), iconSize, FloatValue(8.f),
                     FloatValue(128.f), FloatValue(2.f), "showIcons"_istrue);
    addSliderSetting("textSize", LocalizeString::get("client.textmodule.vaultPredictor.textSize.name"),
                     LocalizeString::get("client.textmodule.vaultPredictor.textSize.desc"), textSize, FloatValue(8.f),
                     FloatValue(60.f), FloatValue(1.f));
    addSetting("showBackground", LocalizeString::get("client.textmodule.vaultPredictor.showBackground.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.showBackground.desc"), showBackground);
    addSetting("scanBlocks", LocalizeString::get("client.textmodule.vaultPredictor.scanBlocks.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.scanBlocks.desc"), scanBlocks);
    addSetting("debugMode", LocalizeString::get("client.textmodule.vaultPredictor.debugMode.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.debugMode.desc"), debugMode);

    // The seed packet comes once, when you join, and vault updates only while you are near, so the packets are read even
    // while the module is off: switching it on later still finds the seed and the vaults already seen.
    listen<PacketReceiveEvent>((EventListenerFunc)&VaultPredictor::onPacketReceive, true);
    listen<LeaveGameEvent>((EventListenerFunc)&VaultPredictor::onLeaveGame, true);
    listen<RenderLayerEvent>((EventListenerFunc)&VaultPredictor::onRenderLayer);
}

void VaultPredictor::onLeaveGame(Event&) {
    reset();
    std::lock_guard lock(mutex);
    haveServerSeed = false;
}

void VaultPredictor::reset() {
    std::lock_guard lock(mutex);
    vaults.clear();
    ignoredIds.clear();
    itemCache.clear();
    itemCacheRegistry = nullptr;
    registryFound = false;
    regPhase = 0;
    regSpan = 0;
    regOff = 0;
    registryLooked = 0;
    registryRejected.clear();
    ominousByBlock.clear();
    notified.clear();
    scanIndex = 0;
    scanSweeps = 0;
    scanVaultsFound = 0;
    vaultLegacy = nullptr;
    otherLegacy.clear();
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
            if (isEnabled() && std::get<BoolValue>(debugMode))
                Logger::Info("[VaultPredictor] StartGame packet: world seed {} (offset 0x30)", value);
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
        if (isEnabled() && std::get<BoolValue>(debugMode)) {
            const std::string id = out.mStr.substr(0, 400);
            std::lock_guard lock(mutex);
            if (ignoredIds.size() < 20 && ignoredIds.insert(id).second) {
                Logger::Info("[VaultPredictor] block entity data (not a vault): {}", id);
            }
        }
        return;
    }

    if (isEnabled() && std::get<BoolValue>(debugMode)) {
        Logger::Info("[VaultPredictor] vault update: loot table '{}', display item {}, items_to_eject {}",
                     data.lootTable.empty() ? std::string("not sent") : data.lootTable,
                     data.hasDisplay ? data.display.name : std::string("none"),
                     data.hasEjectField ? std::to_string(data.toEject.size()) + " item(s)" : std::string("not sent"));
    }

    std::lock_guard lock(mutex);
    // Entries not updated for ten minutes are dropped (the vault was left far behind or its chunk unloaded).
    const auto now = std::chrono::steady_clock::now();
    for (auto it = vaults.begin(); it != vaults.end();) {
        it = now - it->second.seen > std::chrono::minutes(10) ? vaults.erase(it) : std::next(it);
    }
    if (vaults.size() > kMaxVaults) vaults.clear();
    auto& entry = vaults[keyOf(x, y, z)];
    entry.pos = BlockPos { x, y, z };
    entry.fromScan = false;
    if (data.hasDisplay) {
        if (normalOnlyItem(data.display.name)) entry.sawNormalOnly = true;
        if (ominousOnlyItem(data.display.name)) entry.sawOminousOnly = true;
    }
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

// The unique actor id the loot is calculated with: the typed one, else your own entry in the player list (found by name;
// when the list has only one entry, that one is you).
bool VaultPredictor::localPlayerId(int64_t& out) const {
    const SeedStatus typed = parseSeed(std::get<TextValue>(playerId).str, out); // a whole number, same rules as the seed
    if (typed == SeedStatus::Ok) return true;
    if (typed != SeedStatus::Empty) return false; // typed something that isn't a number: don't guess

    auto* client = SDK::ClientInstance::get();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    auto* level = client && client->minecraft ? client->minecraft->getLevel() : nullptr;
    auto* players = level ? level->getPlayerList() : nullptr;
    if (!player || !players || players->empty()) return false;

    if (!player->playerName.empty()) {
        for (auto& pair : *players) {
            if (pair.second.name == player->playerName) {
                out = pair.second.uniqueId;
                return true;
            }
        }
    }
    if (players->size() == 1) {
        out = players->begin()->second.uniqueId;
        return true;
    }
    return false;
}

namespace {
    // True when the memory is committed and readable (no exception to pay for when it isn't).
    bool readableRange(const void* p, size_t size) {
        if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000) return false;
        MEMORY_BASIC_INFORMATION mbi {};
        const char* cur = static_cast<const char*>(p);
        const char* end = cur + size;
        while (cur < end) {
            if (!VirtualQuery(cur, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
            if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
            const char* regionEnd = static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd <= cur) return false;
            cur = regionEnd;
        }
        return true;
    }

    bool copyBytes(const void* from, void* to, size_t count) {
        __try {
            std::memcpy(to, from, count);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // A std::string at `str` whose text starts with "minecraft:" and is 11 to 80 characters long; copies it to `out`.
    bool readItemString(const void* str, char* out, size_t outSize) {
        uint64_t size = 0, cap = 0;
        if (!readRaw(str, 16, size) || !readRaw(str, 24, cap)) return false;
        if (size < 11 || size > 80 || cap < size || cap > 0x1000 || size + 1 > outSize) return false;
        const char* chars = static_cast<const char*>(str);
        if (cap >= 16) {
            uintptr_t ptr = 0;
            if (!readRaw(str, 0, ptr) || !ptr) return false;
            chars = reinterpret_cast<const char*>(ptr);
        }
        if (!copyBytes(chars, out, size)) return false;
        out[size] = 0;
        // An item id is "minecraft:name"; entity ids carry an event ("minecraft:goat<>") and are not items.
        return std::memcmp(out, "minecraft:", 10) == 0 && std::strpbrk(out, "<>") == nullptr;
    }

    // Where in an Item the id string sits (the layout moves between game versions): the first 8-aligned offset that holds a
    // std::string reading "minecraft:...". 0 = none.
    size_t findItemNameOffset(const void* item) {
        char text[96];
        for (size_t off = 0x20; off <= 0x300; off += 8) {
            if (readItemString(static_cast<const char*>(item) + off, text, sizeof(text))) return off;
        }
        return 0;
    }

    // Does the array of `n` 8-byte values at `a` hold `v`? Guarded.
    bool holdsValue(const uintptr_t* a, size_t n, uintptr_t v) {
        __try {
            for (size_t i = 0; i < n; ++i) {
                if (a[i] == v) return true;
            }
            return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // What a list that held the anchor item but wasn't the item registry looked like (for the Debug Mode report).
    struct Rejected {
        bool set = false;
        size_t vecOff = 0;
        size_t count = 0;
        size_t named = 0;
        char first[64] = {};
    };

    // Does `reg` hold the item registry? Somewhere in its first 0x100 bytes there must be a vector (begin and end pointers) of
    // item counters that contains `anchor`, the counter of an item we already have (the vault's). The Items then show where
    // they keep their id, and the list must hold the items every game has. Only memory known to be readable is read.
    bool probeRegistry(const void* reg, uintptr_t anchor, size_t& vecOff, size_t& nameOff, size_t& countOut, Rejected* rejected) {
        uint8_t buf[0x108];
        if (!readableRange(reg, sizeof(buf)) || !copyBytes(reg, buf, sizeof(buf))) return false;
        for (size_t vo = 0x08; vo <= 0xF0; vo += 8) {
            uintptr_t begin = 0, end = 0;
            std::memcpy(&begin, buf + vo, 8);
            std::memcpy(&end, buf + vo + 8, 8);
            if (!begin || end <= begin || (begin & 7) || (end & 7)) continue;
            const size_t count = (end - begin) / 8;
            if (count < 100 || count > 30000) continue;
            if (!readableRange(reinterpret_cast<const void*>(begin), count * 8)) continue;
            if (!holdsValue(reinterpret_cast<const uintptr_t*>(begin), count, anchor)) continue;

            uintptr_t anchorItem = 0;
            if (!readRaw(reinterpret_cast<const void*>(anchor), 0, anchorItem) || !anchorItem) continue;
            const size_t off = findItemNameOffset(reinterpret_cast<const void*>(anchorItem));
            if (!off) continue;

            bool stick = false, diamond = false;
            size_t named = 0;
            char first[64] = {};
            char text[96];
            for (size_t i = 0; i < count; ++i) {
                uintptr_t counter = 0, item = 0;
                if (!readRaw(reinterpret_cast<const void*>(begin), i * 8, counter) || !counter) continue;
                if (!readRaw(reinterpret_cast<const void*>(counter), 0, item) || !item) continue;
                if (!readItemString(reinterpret_cast<const char*>(item) + off, text, sizeof(text))) continue;
                if (!named++) std::snprintf(first, sizeof(first), "%.60s", text);
                if (std::strcmp(text, "minecraft:stick") == 0) stick = true;
                if (std::strcmp(text, "minecraft:diamond") == 0) diamond = true;
            }
            if (stick && diamond) {
                vecOff = vo;
                nameOff = off;
                countOut = count;
                return true;
            }
            if (rejected && !rejected->set) {
                rejected->set = true;
                rejected->vecOff = vo;
                rejected->count = count;
                rejected->named = named;
                std::memcpy(rejected->first, first, sizeof(first));
            }
        }
        return false;
    }

    // The memory a registry search starts from: 0 Level, 1 Minecraft, 2 ClientInstance, 3 MinecraftGame, 4 the game's image.
    const char* rootBase(int root) {
        auto* client = SDK::ClientInstance::get();
        if (!client) return nullptr;
        switch (root) {
        case 0: return client->minecraft ? reinterpret_cast<const char*>(client->minecraft->getLevel()) : nullptr;
        case 1: return reinterpret_cast<const char*>(client->minecraft);
        case 2: return reinterpret_cast<const char*>(client);
        case 3: return reinterpret_cast<const char*>(client->minecraftGame);
        default: return reinterpret_cast<const char*>(GetModuleHandleW(nullptr));
        }
    }

    // The registry pointer for a remembered place: root + offset, then the extra step the way it was found (3: the pointer
    // found there points at an object that holds the registry pointer at `inner`).
    bool registryAt(int root, size_t offset, int deref, size_t inner, void*& out) {
        const char* base = rootBase(root);
        uintptr_t p = 0;
        if (!base || !readableRange(base + offset, 8) || !readRaw(base, offset, p) || !p) return false;
        if (deref == 1 && (!readableRange(reinterpret_cast<const void*>(p), 8) || !readRaw(reinterpret_cast<const void*>(p), 0, p))) return false;
        if (deref == 2 && (!readableRange(reinterpret_cast<const void*>(p), 16) || !readRaw(reinterpret_cast<const void*>(p), 8, p))) return false;
        if (deref == 3 && (!readableRange(reinterpret_cast<const void*>(p), inner + 8) || !readRaw(reinterpret_cast<const void*>(p), inner, p))) return false;
        out = reinterpret_cast<void*>(p);
        return out != nullptr;
    }

    // The state bits of a block, read the way the vault's own code reads them: the block's data is a 16-bit number at
    // Block + 0x120, and the block type (Block + 0x68) keeps a map at +0x1D8 from each state to where its bits sit (the
    // number of bits at +0x2C of a map node, the highest bit at +0x30). Returns the number of states, -1 if unreadable.
    struct StateBits {
        uint64_t key;
        int bits;
        int top;
        int value;
    };

    int readBlockStates(const void* block, StateBits* out, int maxOut) {
        __try {
            const uintptr_t b = reinterpret_cast<uintptr_t>(block);
            const uint16_t data = *reinterpret_cast<const uint16_t*>(b + 0x120);
            const uintptr_t legacy = *reinterpret_cast<const uintptr_t*>(b + 0x68);
            if (!legacy) return -1;
            const uintptr_t head = *reinterpret_cast<const uintptr_t*>(legacy + 0x1D8);
            if (!head) return -1;
            uintptr_t pending[64];
            int sp = 0, n = 0, guard = 0;
            pending[sp++] = *reinterpret_cast<const uintptr_t*>(head + 8);
            while (sp > 0 && guard++ < 512) {
                const uintptr_t node = pending[--sp];
                if (!node || *reinterpret_cast<const uint8_t*>(node + 25) != 0) continue; // empty
                const int bits = static_cast<int>(*reinterpret_cast<const uint32_t*>(node + 44));
                const int top = static_cast<int>(*reinterpret_cast<const uint8_t*>(node + 48));
                if (n < maxOut && bits >= 1 && bits <= 16 && top >= bits - 1 && top < 16) {
                    out[n].key = *reinterpret_cast<const uint64_t*>(node + 32);
                    out[n].bits = bits;
                    out[n].top = top;
                    out[n].value = (0xFFFFu >> (16 - bits)) & (data >> (top - bits + 1));
                    ++n;
                }
                if (sp < 62) {
                    pending[sp++] = *reinterpret_cast<const uintptr_t*>(node);
                    pending[sp++] = *reinterpret_cast<const uintptr_t*>(node + 16);
                }
            }
            return n;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    // A vault has three states: its direction (4 values), vault_state (4 values) and ominous (on/off). The only one that
    // takes a single bit is therefore "ominous". 1 ominous, 0 normal, -1 can't tell.
    int ominousFromStates(StateBits const* s, int n) {
        int found = 0, value = -1;
        for (int i = 0; i < n; ++i) {
            if (s[i].bits != 1) continue;
            ++found;
            value = s[i].value;
        }
        return found == 1 ? value : -1;
    }
} // namespace

// Looks for the item registry (what WAILA reads its tool icons from: a list of item counters, each pointing at an Item) for
// about 2 ms of a frame at most, and carries on from where it stopped on the next frame, so it can never freeze the game. The
// registry is recognised by `anchor`, the counter of an item we already have (the vault's): it is the list that holds it.
// Pass 0 looks at what the main game objects (Level, Minecraft, ClientInstance, MinecraftGame) and the game's static data
// point at; pass 1 one level deeper in the objects. Returns true once it is found.
bool VaultPredictor::stepRegistrySearch(uintptr_t anchor) {
    if (registryFound) return true;
    if (regPhase >= 2 || !anchor) return false;
    auto* client = SDK::ClientInstance::get();
    if (!client) return false;
    const auto t0 = std::chrono::steady_clock::now();
    const auto budget = std::chrono::microseconds(2000);

    struct Span {
        int root;
        size_t start; // offset from the root's base
        size_t size;
        const char* name;
    };
    std::vector<Span> spans;
    if (client->minecraft && client->minecraft->getLevel()) spans.push_back({ 0, 0, 0x3000, "Level" });
    if (client->minecraft) spans.push_back({ 1, 0, 0x3000, "Minecraft" });
    spans.push_back({ 2, 0, 0x3000, "ClientInstance" });
    if (client->minecraftGame) spans.push_back({ 3, 0, 0x3000, "MinecraftGame" });
    const size_t objectSpans = spans.size();

    const char* const image = rootBase(4);
    size_t imageSize = 0;
    if (image) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
        imageSize = nt->OptionalHeader.SizeOfImage;
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const DWORD f = sec[i].Characteristics;
            if ((f & IMAGE_SCN_MEM_WRITE) && !(f & IMAGE_SCN_MEM_EXECUTE) && !(f & IMAGE_SCN_MEM_DISCARDABLE))
                spans.push_back({ 4, sec[i].VirtualAddress, sec[i].Misc.VirtualSize, "game data" });
        }
    }

    Rejected rejected;
    auto candidate = [&](uintptr_t c, int root, const char* name, size_t off, int deref, size_t inner) -> bool {
        if (c < 0x10000) return false;
        size_t vo = 0, no = 0, cnt = 0;
        rejected = Rejected {};
        if (!probeRegistry(reinterpret_cast<const void*>(c), anchor, vo, no, cnt, &rejected)) {
            if (rejected.set && registryRejected.size() < 300) {
                registryRejected += std::format(" [{} +0x{:X}{}: list at +0x{:X} has {} slots, {} item-like names, e.g. {}; no stick/diamond]", name,
                                                off, deref == 3 ? std::format(" -> +0x{:X}", inner) : std::string(), rejected.vecOff,
                                                rejected.count, rejected.named, rejected.first);
            }
            return false;
        }
        const std::string via = deref == 0 ? "" : deref == 1 ? " -> +0" : deref == 2 ? " -> +8" : std::format(" -> +0x{:X}", inner);
        registryFound = true;
        registryRoot = root;
        registryOffset = off;
        registryDeref = deref;
        registryInner = inner;
        registryVecOff = vo;
        registryNameOff = no;
        registryFoundNote = std::format("found in {} at +0x{:X}{} (list at +0x{:X}, item ids at +0x{:X}, {} slots)", name, off, via, vo, no, cnt);
        registryNote = registryFoundNote;
        itemCache.clear();
        itemCacheRegistry = nullptr; // read on the next lookup
        return true;
    };

    uint8_t chunk[0x1000];
    const size_t spanCount = regPhase == 0 ? spans.size() : objectSpans;
    while (regSpan < spanCount && !registryFound) {
        const Span sp = spans[regSpan];
        const char* base = rootBase(sp.root);
        if (!base) {
            ++regSpan;
            regOff = 0;
            continue;
        }
        size_t chunkStart = static_cast<size_t>(-1);
        bool chunkOk = false;
        while (regOff < sp.size && !registryFound) {
            if (std::chrono::steady_clock::now() - t0 > budget) {
                if (std::get<BoolValue>(debugMode))
                    registryNote = std::format("still looking for the item registry (pass {} of 2, {} pointers looked at so far){}", regPhase + 1,
                                               registryLooked, registryRejected);
                return false;
            }
            const size_t off = sp.start + regOff;
            const size_t cs = off & ~static_cast<size_t>(0xFFF);
            if (cs != chunkStart) {
                chunkStart = cs;
                chunkOk = readableRange(base + cs, sizeof(chunk)) && copyBytes(base + cs, chunk, sizeof(chunk));
            }
            if (!chunkOk) {
                regOff = cs + 0x1000 - sp.start; // skip the rest of an unreadable page
                continue;
            }
            regOff += 8;
            uintptr_t p = 0;
            std::memcpy(&p, chunk + (off - cs), 8);
            if (p < 0x10000 || (p & 7)) continue;
            if (sp.root == 4 && image && p >= reinterpret_cast<uintptr_t>(image) && p < reinterpret_cast<uintptr_t>(image) + imageSize) continue;
            ++registryLooked;
            if (regPhase == 0) {
                for (int deref = 0; deref < 3 && !registryFound; ++deref) {
                    uintptr_t c = p;
                    if (deref == 1) {
                        if (!readableRange(reinterpret_cast<const void*>(p), 8) || !readRaw(reinterpret_cast<const void*>(p), 0, c)) continue;
                    } else if (deref == 2) {
                        if (!readableRange(reinterpret_cast<const void*>(p), 16) || !readRaw(reinterpret_cast<const void*>(p), 8, c)) continue;
                    }
                    candidate(c, sp.root, sp.name, off, deref, 0);
                }
            } else {
                uint8_t inside[0x400];
                if (!readableRange(reinterpret_cast<const void*>(p), sizeof(inside)) ||
                    !copyBytes(reinterpret_cast<const void*>(p), inside, sizeof(inside)))
                    continue;
                for (size_t inner = 0; inner < sizeof(inside) && !registryFound; inner += 8) {
                    uintptr_t q = 0;
                    std::memcpy(&q, inside + inner, 8);
                    candidate(q, sp.root, sp.name, off, 3, inner);
                }
            }
        }
        if (registryFound) break;
        ++regSpan;
        regOff = 0;
    }
    if (registryFound) return true;
    ++regPhase;
    regSpan = 0;
    regOff = 0;
    if (regPhase >= 2) {
        registryNote = std::format("no item registry found: looked at {} pointers in the Level, Minecraft, ClientInstance and MinecraftGame objects, "
                                   "what they point at and the game's data; none was a list holding the vault's item{}",
                                   registryLooked, registryRejected);
    }
    return false;
}

// The counter of an item (what an ItemStack points at), from the item registry once stepRegistrySearch has found it.
void* VaultPredictor::findItemCounter(std::string const& id) {
    if (!registryFound) return nullptr;

    auto forget = [&](const char* why) {
        registryFound = false;
        itemCache.clear();
        itemCacheRegistry = nullptr;
        regPhase = 0;
        regSpan = 0;
        regOff = 0;
        registryLooked = 0;
        registryRejected.clear();
        registryNote = why;
    };

    void* registry = nullptr;
    if (!registryAt(registryRoot, registryOffset, registryDeref, registryInner, registry)) {
        forget("the item registry moved; looking for it again");
        return nullptr;
    }
    if (registry != itemCacheRegistry) {
        itemCache.clear();
        itemCacheRegistry = registry;
    }

    if (itemCache.empty()) {
        uintptr_t begin = 0, end = 0;
        if (!readRaw(registry, registryVecOff, begin) || !readRaw(registry, registryVecOff + 8, end) || !begin || end <= begin) {
            forget("the item list is empty; looking for it again");
            return nullptr;
        }
        size_t slots = 0, withItem = 0;
        std::string examples;
        char text[96];
        for (uintptr_t current = begin; current < end; current += 8) {
            ++slots;
            uintptr_t counter = 0, item = 0;
            if (!readRaw(reinterpret_cast<const void*>(current), 0, counter) || !counter) continue;
            if (!readRaw(reinterpret_cast<const void*>(counter), 0, item) || !item) continue;
            ++withItem;
            if (!readItemString(reinterpret_cast<const char*>(item) + registryNameOff, text, sizeof(text))) continue;
            const std::string name = text;
            itemCache.emplace(name, reinterpret_cast<void*>(counter));
            // Also under the short name, in case the game keeps the ids without "minecraft:".
            itemCache.emplace(name.substr(10), reinterpret_cast<void*>(counter));
            if (examples.size() < 60) examples += (examples.empty() ? "" : ", ") + name;
        }
        registryNote = registryFoundNote + std::format("; {} slot(s), {} with an item, {} names read (e.g. {})", slots, withItem,
                                                       itemCache.size() / 2, examples.empty() ? std::string("none") : examples);
    }

    auto it = itemCache.find(id.find(':') == std::string::npos ? "minecraft:" + id : id);
    if (it == itemCache.end()) it = itemCache.find(id);
    return it == itemCache.end() ? nullptr : it->second;
}

// Looks for vault blocks around you, a few thousand positions per frame in a rolling sweep over the box that Max Distance
// describes, so a vault shows its loot before the server has sent anything about it. Only the block's type is compared:
// the name of each distinct type is read once.
void VaultPredictor::scanForVaults(SDK::BlockSource* region, Vec3 const& me, float maxDist) {
    const int R = static_cast<int>(std::ceil(maxDist));
    const int cx = static_cast<int>(std::floor(me.x));
    const int cy = static_cast<int>(std::floor(me.y));
    const int cz = static_cast<int>(std::floor(me.z));
    const int y0 = std::max(-64, cy - R);
    const int y1 = std::min(320, cy + R);
    const int nxz = 2 * R + 1;
    const int ny = y1 - y0 + 1;
    if (ny <= 0) return;
    const size_t total = static_cast<size_t>(nxz) * static_cast<size_t>(nxz) * static_cast<size_t>(ny);
    if (scanIndex >= total) {
        scanIndex = 0;
        ++scanSweeps;
    }

    constexpr int kBudget = 12000;
    const auto now = std::chrono::steady_clock::now();
    for (int n = 0; n < kBudget && scanIndex < total; ++n, ++scanIndex) {
        size_t i = scanIndex;
        const int dy = static_cast<int>(i % static_cast<size_t>(ny));
        i /= static_cast<size_t>(ny);
        const int dz = static_cast<int>(i % static_cast<size_t>(nxz));
        const int dx = static_cast<int>(i / static_cast<size_t>(nxz));
        const BlockPos pos { cx - R + dx, y0 + dy, cz - R + dz };

        SDK::Block* block = region->getBlock(pos);
        if (!block) continue;
        SDK::BlockLegacy* legacy = block->legacyBlock;
        if (!legacy) continue;
        if (legacy != vaultLegacy) {
            if (otherLegacy.contains(legacy)) continue;
            if (legacy->namespacedId.getString() != "minecraft:vault") {
                otherLegacy.insert(legacy);
                continue;
            }
            vaultLegacy = legacy;
        }

        std::lock_guard lock(mutex);
        const uint64_t key = keyOf(pos.x, pos.y, pos.z);
        auto it = vaults.find(key);
        if (it != vaults.end()) {
            it->second.seen = now;
        } else if (vaults.size() < kMaxVaults) {
            Entry e;
            e.pos = pos;
            e.fromScan = true;
            e.seen = now;
            vaults.emplace(key, std::move(e));
            ++scanVaultsFound;
        }
    }
    scanReport = std::format("{} new vault(s) found so far; position {} of {} in the current sweep ({} full sweep(s) done); {} other block type(s) checked",
                             scanVaultsFound, scanIndex, total, scanSweeps, otherLegacy.size());
}

namespace {
    struct Line {
        std::wstring text;
        d2d::Color color;
        float scale = 1.f; // relative to the text size
    };

    struct Column {
        vault_nbt::ItemInfo item;
        std::vector<Line> lines;
        bool hot = false; // one of the things the notify list asks about
    };

    // "Heavy Core", "minecraft:heavy_core" and "heavycore" all read the same.
    std::string normalizeTerm(std::string s) {
        std::string out;
        for (char c : s) {
            if (c == ' ' || c == '-' || c == '_') continue;
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (out.rfind("minecraft:", 0) == 0) out.erase(0, 10);
        return out;
    }

    bool termMatches(std::vector<std::string> const& terms, std::string const& name) {
        if (terms.empty() || name.empty()) return false;
        // The id ("bolt_armor_trim_smithing_template") and the name shown in the label ("Bolt Armor Trim") both count.
        const std::string n = normalizeTerm(name);
        const std::string shown = normalizeTerm(vault_nbt::prettyName(name));
        return std::find(terms.begin(), terms.end(), n) != terms.end() ||
               std::find(terms.begin(), terms.end(), shown) != terms.end();
    }

    struct IconJob {
        std::string item;
        int count;
        Vec2 pos;
        float size;
    };
} // namespace

void VaultPredictor::onRenderLayer(Event& evG) {
    auto& ev = reinterpret_cast<RenderLayerEvent&>(evG);
    SDK::ScreenView* view = ev.getScreenView();
    if (!view || !view->visualTree || !view->visualTree->rootControl || view->visualTree->rootControl->name != "hud_screen")
        return;

    auto* client = SDK::ClientInstance::get();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    auto* region = client ? client->getRegion() : nullptr;
    if (!player || !region || !client->minecraftGame || !client->minecraftGame->getFontRepository()) return;

    // The vaults to draw: the ones the server has sent data about within Max Distance, nearest first.
    struct Pick {
        Entry entry;
        float dist;
    };
    std::vector<Pick> picks;
    const float maxDist = std::clamp(static_cast<float>(std::get<FloatValue>(maxDistance)), 2.f, 128.f);
    const Vec3 me = player->getPos();
    if (std::get<BoolValue>(scanBlocks)) scanForVaults(region, me, maxDist);
    {
        std::lock_guard lock(mutex);
        for (auto const& [k, entry] : vaults) {
            const float d = me.distance(Vec3 { entry.pos.x + 0.5f, entry.pos.y + 0.5f, entry.pos.z + 0.5f });
            if (d <= maxDist) picks.push_back({ entry, d });
        }
    }
    // Nothing to do when there is no vault in range, no alert still on the screen and no preview asked for.
    if (picks.empty() && alerts.empty() && !std::get<BoolValue>(notifyPreview)) return;
    std::sort(picks.begin(), picks.end(), [](Pick const& a, Pick const& b) { return a.dist < b.dist; });

    const int wanted = vaultType.getSelectedKey();
    const bool icons = std::get<BoolValue>(showIcons);
    const bool names = std::get<BoolValue>(showNames);
    const bool amounts = std::get<BoolValue>(showAmounts);
    const bool enchants = std::get<BoolValue>(showEnchantments);
    const bool background = std::get<BoolValue>(showBackground);

    // The notify list: names separated by commas, parsed again only when the text changes.
    const bool notifyOn = std::get<BoolValue>(notifyEnabled);
    {
        auto const& raw = std::get<TextValue>(notifyItems).str;
        if (raw != notifyParsedFrom) {
            notifyParsedFrom = raw;
            notifyTerms.clear();
            std::string text = util::WStrToStr(raw), part;
            text += ',';
            for (char c : text) {
                if (c == ',' || c == ';' || c == '\n') {
                    const std::string term = normalizeTerm(part);
                    if (!term.empty()) notifyTerms.push_back(term);
                    part.clear();
                } else {
                    part += c;
                }
            }
        }
    }
    const float baseText = std::clamp(static_cast<float>(std::get<FloatValue>(textSize)), 8.f, 60.f);
    const float baseIcon = std::clamp(static_cast<float>(std::get<FloatValue>(iconSize)), 8.f, 128.f);
    const auto font = Renderer::FontSelection::SecondaryLight;

    MCDrawUtil mc { ev.getUIRenderContext(), client->minecraftGame->getFontRepository()->getMinecraftFont() };
    mc.setImmediate(false);

    // What the loot is calculated from: the typed seed (or, when blank, the server's) and your unique actor id. A seed that
    // is typed but isn't a number is never replaced by the server's.
    int64_t calcSeed = 0;
    bool haveCalcSeed = false;
    {
        const SeedStatus st = parseSeed(std::get<TextValue>(seed).str, calcSeed);
        if (st == SeedStatus::Ok) {
            haveCalcSeed = true;
        } else if (st == SeedStatus::Empty) {
            std::lock_guard lock(mutex);
            if (haveServerSeed) {
                calcSeed = serverSeed;
                haveCalcSeed = true;
            }
        }
    }
    int64_t calcPlayer = 0;
    const bool havePlayer = localPlayerId(calcPlayer);

    std::vector<IconJob> jobs;
    SDK::Block* iconBlock = nullptr;
    int drawn = 0;

    const bool canPredict = haveCalcSeed && havePlayer;

    for (auto const& pick : picks) {
        if (drawn >= 8) break;

        // It must still be a vault; an empty name means the chunk isn't loaded on your side, which is fine.
        const std::string blockName = blockNameInner(pick.entry.pos);
        if (!blockName.empty() && blockName != "minecraft:vault") continue;

        auto const& data = pick.entry.data;
        // The type: the loot table when the server sends it; otherwise the one the spinning item points to; otherwise, if
        // the Vault Type setting is Normal or Ominous, that.
        const bool tableKnown = !data.lootTable.empty() || !data.overrideLootTable.empty();
        bool typeKnown = tableKnown;
        bool ominous = tableKnown && vault_nbt::isOminous(data);
        std::string typeSource = tableKnown ? "loot table sent by the server" : "";
        if (!tableKnown) {
            // The vault block says it itself: it has an "ominous" state.
            int stateOminous = -1;
            if (SDK::Block* vaultBlock = region->getBlock(pick.entry.pos)) {
                auto cached = ominousByBlock.find(vaultBlock);
                if (cached != ominousByBlock.end()) {
                    stateOminous = cached->second;
                } else {
                    StateBits bits[16];
                    const int n = readBlockStates(vaultBlock, bits, 16);
                    stateOminous = n > 0 ? ominousFromStates(bits, n) : -1;
                    if (ominousByBlock.size() > 512) ominousByBlock.clear();
                    ominousByBlock.emplace(vaultBlock, stateOminous);
                    if (std::get<BoolValue>(debugMode)) {
                        std::string list;
                        for (int i = 0; i < n; ++i)
                            list += std::format("{}{{key 0x{:X}, {} bit(s) ending at bit {}, value {}}}", i ? ", " : "", bits[i].key,
                                                bits[i].bits, bits[i].top, bits[i].value);
                        Logger::Info("[VaultPredictor] vault block states ({}): {} => ominous {}", n, list.empty() ? "none" : list,
                                     stateOminous < 0 ? "unknown" : stateOminous ? "yes" : "no");
                    }
                }
            }
            if (stateOminous >= 0) {
                typeKnown = true;
                ominous = stateOminous == 1;
                typeSource = "read from the vault block's ominous state";
                if (pick.entry.sawOminousOnly != pick.entry.sawNormalOnly && pick.entry.sawOminousOnly != ominous)
                    typeSource += " (the spinning item points to the other type)";
            } else if (pick.entry.sawOminousOnly != pick.entry.sawNormalOnly) {
                typeKnown = true;
                ominous = pick.entry.sawOminousOnly;
                typeSource = "worked out from the item spinning in the vault";
            } else if (wanted != TypeBoth) {
                typeKnown = true;
                ominous = wanted == TypeOminous;
                typeSource = "taken from the Vault Type setting";
            }
        }
        if (typeKnown) {
            if (wanted == TypeNormal && ominous) continue;
            if (wanted == TypeOminous && !ominous) continue;
        }

        // Tell about a vault whose loot has something from the notify list, once per vault, whether or not it is on screen.
        if (notifyOn && !notifyTerms.empty() && (canPredict || !data.toEject.empty())) {
            auto announce = [&](std::vector<std::wstring> const& found, bool om, bool known) {
                if (found.empty()) return;
                const uint64_t nk = keyOf(pick.entry.pos.x, pick.entry.pos.y, pick.entry.pos.z) * 2 + (om ? 1 : 0);
                if (!notified.insert(nk).second) return;
                if (notified.size() > 4096) notified.clear();
                std::wstring list;
                for (auto const& n : found) list += (list.empty() ? L"" : L", ") + n;
                const std::wstring kind = known ? (om ? L"Ominous vault" : L"Vault") : (om ? L"Vault, if ominous" : L"Vault, if normal");
                const std::wstring coords = std::to_wstring(pick.entry.pos.x) + L", " + std::to_wstring(pick.entry.pos.y) + L", " +
                                            std::to_wstring(pick.entry.pos.z);
                // On the screen (where Notify Position puts it) and/or in the chat: the items, then which vault and where.
                if (std::get<BoolValue>(notifyScreen)) {
                    alerts.push_back({ list, kind + L" at " + coords, std::chrono::steady_clock::now() });
                    if (alerts.size() > 6) alerts.erase(alerts.begin());
                }
                if (std::get<BoolValue>(notifyChat)) {
                    Latite::get().getClientMessageQueue().push(util::WFormat(L"[Vault] " + list + L" - " + kind + L" at " + coords));
                }
            };
            if (!data.toEject.empty()) {
                std::vector<std::wstring> found;
                for (auto const& item : data.toEject) {
                    bool hit = termMatches(notifyTerms, item.name);
                    for (auto const& e : item.enchants) hit = hit || termMatches(notifyTerms, vault_nbt::enchantName(e.id));
                    if (hit) found.push_back(util::StrToWStr(vault_nbt::prettyName(item.name)));
                }
                announce(found, ominous, typeKnown);
            } else {
                auto scanPredicted = [&](bool om, bool known) {
                    const auto result = vault_loot::predict(calcSeed, calcPlayer, pick.entry.pos.x, pick.entry.pos.y,
                                                            pick.entry.pos.z, om);
                    std::vector<std::wstring> found;
                    for (auto const& s : result.items) {
                        if (termMatches(notifyTerms, s.item)) {
                            found.push_back(util::StrToWStr(vault_nbt::prettyName(s.item)));
                        } else if (!s.enchant.empty() && termMatches(notifyTerms, s.enchant)) {
                            found.push_back(util::StrToWStr(vault_nbt::prettyName(s.enchant)) + L" " +
                                            util::StrToWStr(vault_nbt::roman(s.enchantLevel)) + L" book");
                        }
                    }
                    announce(found, om, known);
                };
                if (typeKnown) {
                    scanPredicted(ominous, true);
                } else {
                    scanPredicted(false, false);
                    scanPredicted(true, false);
                }
            }
        }

        // Above the vault, like a name tag. It shrinks with distance so far vaults don't fill the screen.
        const Vec3 anchorWorld { pick.entry.pos.x + 0.5f, pick.entry.pos.y + 1.15f, pick.entry.pos.z + 0.5f };
        auto screen = WorldToScreen::convert(anchorWorld, true);
        if (!screen) continue;
        const float scale = std::clamp(8.f / std::max(pick.dist, 1.f), 0.55f, 1.f);
        const float ts = baseText * scale;
        const float S = baseIcon * scale;
        const float gap = 6.f * scale;
        const float pad = 5.f * scale;
        const std::wstring distText = std::to_wstring(static_cast<int>(std::lround(pick.dist))) + L"m";

        const bool serverRewards = !data.toEject.empty();

        // The boxes to draw, bottom first. One when the type is known (or the server sent the rewards). When the type isn't
        // known yet (the server doesn't send it, and no item only one type has was seen spinning in the vault), one box per
        // type, so the loot of a vault you haven't been near can still be read once you can tell which kind it is.
        struct Box {
            bool ominous;
            bool known;
        };
        std::vector<Box> boxes;
        if (typeKnown || serverRewards) boxes.push_back({ ominous, typeKnown });
        else if (canPredict) boxes = { { false, false }, { true, false } };
        else boxes.push_back({ false, false }); // only the note

        bool firstBox = true;
        bool predictedShown = false;
        size_t firstCols = 0;

        auto drawBox = [&](Box const& box, float bottom) -> float {
            // The columns: the rewards the server sent, else the calculated ones. The item spinning in the vault
            // (display_item) is not a reward and is never drawn.
            std::vector<Column> cols;
            auto addColumn = [&](vault_nbt::ItemInfo const& item) {
                Column col;
                col.item = item;
                if (names) col.lines.push_back({ util::StrToWStr(vault_nbt::prettyName(item.name)), d2d::Colors::WHITE, 0.85f });
                if (enchants) {
                    for (auto const& e : item.enchants) {
                        col.lines.push_back({ util::StrToWStr(vault_nbt::enchantName(e.id)) + L" " +
                                                  util::StrToWStr(vault_nbt::roman(e.level)),
                                              d2d::Color::RGB(255, 100, 255), 0.85f });
                    }
                }
                cols.push_back(std::move(col));
            };
            const d2d::Color hotColor = d2d::Color::RGB(255, 215, 0);
            auto makeHot = [&]() {
                cols.back().hot = true;
                if (!cols.back().lines.empty()) cols.back().lines[0].color = hotColor;
            };
            for (auto const& item : data.toEject) {
                addColumn(item);
                bool hit = notifyOn && termMatches(notifyTerms, item.name);
                for (auto const& e : item.enchants) hit = hit || (notifyOn && termMatches(notifyTerms, vault_nbt::enchantName(e.id)));
                if (hit) makeHot();
            }

            std::wstring note;
            bool predicted = false;
            if (cols.empty()) {
                if (!typeKnown && !canPredict) {
                    if (!haveCalcSeed) note = L"no valid world seed (see the World Seed setting)";
                    else note = L"player id not found yet (join a world; or type it in Player ID)";
                } else if (!haveCalcSeed) {
                    note = L"no valid world seed (see the World Seed setting)";
                } else if (!havePlayer) {
                    note = L"player id not found yet (join a world; or type it in Player ID)";
                } else {
                    const auto result = vault_loot::predict(calcSeed, calcPlayer, pick.entry.pos.x, pick.entry.pos.y,
                                                            pick.entry.pos.z, box.ominous);
                    for (auto const& s : result.items) {
                        vault_nbt::ItemInfo info;
                        info.name = s.item;
                        info.count = s.count;
                        addColumn(info);
                        if (notifyOn && (termMatches(notifyTerms, s.item) || termMatches(notifyTerms, s.enchant))) makeHot();
                        auto& lines = cols.back().lines;
                        if (names && !s.variant.empty()) {
                            if (s.item == "minecraft:ominous_bottle") {
                                // The amplifier counts from 0; the bottle's name shows the Bad Omen level.
                                lines.push_back({ L"Bad Omen " + util::StrToWStr(vault_nbt::roman(std::atoi(s.variant.c_str()) + 1)),
                                                  d2d::Color::RGB(170, 170, 255), 0.8f });
                            } else {
                                lines.push_back({ util::StrToWStr(vault_nbt::prettyName(s.variant)),
                                                  d2d::Color::RGB(170, 170, 255), 0.8f });
                            }
                        }
                        if (!s.enchant.empty() && enchants) {
                            lines.push_back({ util::StrToWStr(vault_nbt::prettyName(s.enchant)) + L" " +
                                                  util::StrToWStr(vault_nbt::roman(s.enchantLevel)),
                                              d2d::Color::RGB(255, 170, 255), 0.8f });
                        }
                        if (s.details && enchants) {
                            lines.push_back({ s.item == "minecraft:shield" ? L"damage unknown" : L"random enchantments",
                                              d2d::Color::RGB(170, 170, 170), 0.8f });
                        }
                    }
                    predicted = !cols.empty();
                    if (!result.complete) note = L"the rest can't be predicted yet";
                    else if (cols.empty()) note = L"nothing ejected";
                }
            }
            if (firstBox) {
                predictedShown = predicted;
                firstCols = cols.size();
                firstBox = false;
            }

            // The title under the icons: the type and the distance.
            std::wstring title;
            if (box.known) title = std::wstring(box.ominous ? L"Ominous Vault " : L"Vault ") + distText;
            else if (boxes.size() > 1) title = std::wstring(box.ominous ? L"If ominous: " : L"If normal: ") + distText;
            else title = L"Vault " + distText;
            if (predicted) title += L" (predicted)";
            for (auto const& col : cols) {
                if (col.hot) {
                    title = L"! " + title;
                    break;
                }
            }
            const d2d::Color titleColor = box.ominous ? d2d::Color::RGB(120, 200, 255) : d2d::Color::RGB(255, 170, 0);

            const float lineH = std::max(mc.getTextSize(L"Ag", font, ts, false).y, 6.f);
            const Vec2 titleBox = mc.getTextSize(title, font, ts, false);
            const Vec2 noteBox = note.empty() ? Vec2 { 0.f, 0.f } : mc.getTextSize(note, font, ts * 0.8f, false);

            // Column widths and the tallest text block.
            std::vector<float> colW;
            float rowW = 0.f;
            size_t maxLines = 0;
            for (auto const& col : cols) {
                float w = icons ? S : 0.f;
                for (auto const& line : col.lines) w = std::max(w, mc.getTextSize(line.text, font, ts * line.scale, false).x);
                colW.push_back(w + gap);
                rowW += colW.back();
                maxLines = std::max(maxLines, col.lines.size());
            }
            const float contentW = std::max({ rowW, titleBox.x, noteBox.x });
            const float iconH = (icons && !cols.empty()) ? S + gap * 0.5f : 0.f;
            const float textH = static_cast<float>(maxLines) * lineH * 0.85f;
            const float noteH = note.empty() ? 0.f : lineH * 0.8f;
            const float totalH = iconH + textH + noteH + lineH + pad * 2.f;

            const float left = screen->x - contentW * 0.5f;
            const float top = bottom - totalH;
            if (background) {
                mc.fillRoundedRectangle(d2d::Rect(left - pad, top, left + contentW + pad, bottom),
                                        d2d::Color(0.f, 0.f, 0.f, 0.4f), ts * 0.3f);
            }

            // Columns, centred as a row.
            float x = screen->x - rowW * 0.5f;
            for (size_t i = 0; i < cols.size(); ++i) {
                const float colCenter = x + (colW[i] - gap) * 0.5f;
                float y = top + pad;
                if (icons) {
                    jobs.push_back({ cols[i].item.name, cols[i].item.count, { colCenter - S * 0.5f, y }, S });
                    if (amounts && cols[i].item.count > 1) {
                        const std::wstring n = std::to_wstring(cols[i].item.count);
                        const Vec2 nb = mc.getTextSize(n, font, ts * 0.8f, false);
                        mc.drawText(d2d::Rect(colCenter + S * 0.5f - nb.x, y + S - nb.y, colCenter + S * 0.5f + 2.f, y + S + 2.f),
                                    n, d2d::Colors::WHITE, font, ts * 0.8f, DWRITE_TEXT_ALIGNMENT_TRAILING,
                                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    }
                    y += S + gap * 0.5f;
                } else if (amounts && cols[i].item.count > 1 && !cols[i].lines.empty()) {
                    cols[i].lines[0].text += L" x" + std::to_wstring(cols[i].item.count);
                }
                for (auto const& line : cols[i].lines) {
                    mc.drawText(d2d::Rect(colCenter - colW[i] * 0.5f, y, colCenter + colW[i] * 0.5f, y + lineH * 0.85f),
                                line.text, line.color, font, ts * line.scale, DWRITE_TEXT_ALIGNMENT_CENTER,
                                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    y += lineH * 0.85f;
                }
                x += colW[i];
            }

            // The note and the title at the bottom, next to the vault.
            float ty = top + pad + iconH + textH;
            if (!note.empty()) {
                mc.drawText(d2d::Rect(left, ty, left + contentW, ty + noteH), note, d2d::Color::RGB(170, 170, 170), font,
                            ts * 0.8f, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                ty += noteH;
            }
            mc.drawText(d2d::Rect(left, ty, left + contentW, ty + lineH), title, titleColor, font, ts,
                        DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            return top;
        };

        float bottom = screen->y;
        for (auto const& box : boxes) bottom = drawBox(box, bottom) - 4.f * scale;

        if (!iconBlock) iconBlock = region->getBlock(pick.entry.pos);
        ++drawn;

        if (std::get<BoolValue>(debugMode)) {
            const uint64_t key = keyOf(pick.entry.pos.x, pick.entry.pos.y, pick.entry.pos.z);
            auto& last = lastDiag[key];
            const auto now = std::chrono::steady_clock::now();
            if (lastDiag.size() > 256) lastDiag.clear();
            if (last.time_since_epoch().count() == 0 || now - last > std::chrono::seconds(5)) {
                last = now;
                int64_t typedSeed = 0;
                const SeedStatus seedStatus = parseSeed(std::get<TextValue>(seed).str, typedSeed);
                bool haveSeed = false;
                int64_t worldSeed = 0;
                {
                    std::lock_guard lock(mutex);
                    haveSeed = haveServerSeed;
                    worldSeed = serverSeed;
                }
                std::string manual = "not set";
                if (seedStatus == SeedStatus::Ok) manual = std::format("OK ({})", typedSeed);
                else if (seedStatus == SeedStatus::Invalid) manual = "INVALID (not a whole number)";
                else if (seedStatus == SeedStatus::OutOfRange) manual = "INVALID (outside the signed 64-bit range)";
                std::string match = "n/a";
                if (seedStatus == SeedStatus::Ok && haveSeed) match = typedSeed == worldSeed ? "typed seed equals the server's" : "DIFFERENT from the server's";
                Logger::Info(
                    "[VaultPredictor] diagnostics for a vault in range:\n"
                    "  World seed (StartGame packet): {}\n"
                    "  Manual seed: {}\n"
                    "  Seed comparison: {}\n"
                    "  Vault detected: OK ({})\n"
                    "  Position: {}, {}, {}\n"
                    "  Vault type: {}\n"
                    "  Player id: {}\n"
                    "  Server reward data (items_to_eject): {}\n"
                    "  Predicted items: {}\n"
                    "  Icons (last frame): {}\n"
                    "  Block scan: {}\n"
                    "  HUD: {}",
                    haveSeed ? std::format("OK ({})", worldSeed) : std::string("NOT RECEIVED (join the world again with the mod loaded)"),
                    manual, match, pick.entry.fromScan ? "found by the block scan" : "server update",
                    pick.entry.pos.x, pick.entry.pos.y, pick.entry.pos.z,
                    typeKnown ? std::format("{} ({})", ominous ? "Ominous" : "Normal", typeSource)
                              : std::string("not known yet: showing both kinds"),
                    havePlayer ? std::format("{}", calcPlayer) : std::string("UNKNOWN (set the Player ID setting)"),
                    data.hasEjectField ? std::to_string(data.toEject.size()) + " item(s) sent" : std::string("not sent"),
                    predictedShown ? std::format("{} column(s) calculated (seed {} used)", firstCols, calcSeed)
                                   : std::string("none"),
                    iconReport.empty() ? std::string("not drawn yet") : iconReport, scanReport,
                    firstCols == 0 ? "title and note only (no rewards to draw)" : "rewards drawn");
            }
        }
    }

    // The alerts (Notify): a box for each, newest first, from the place Notify Position gives, moved by the two offsets.
    {
        const float duration = std::clamp(static_cast<float>(std::get<FloatValue>(notifyDuration)), 2.f, 60.f);
        const auto nowT = std::chrono::steady_clock::now();
        auto ageOf = [&](Alert const& a) { return std::chrono::duration<float>(nowT - a.created).count(); };
        alerts.erase(std::remove_if(alerts.begin(), alerts.end(), [&](Alert const& a) { return ageOf(a) > duration; }), alerts.end());

        std::vector<std::pair<Alert, float>> shown; // alert, opacity
        for (auto it = alerts.rbegin(); it != alerts.rend(); ++it) {
            const float left = duration - ageOf(*it);
            shown.push_back({ *it, left < 0.6f ? std::clamp(left / 0.6f, 0.f, 1.f) : 1.f });
        }
        if (std::get<BoolValue>(notifyPreview))
            shown.push_back({ Alert { L"Heavy Core, Wind Burst I book", L"Ominous vault at 123, 45, -678", nowT }, 1.f });

        if (std::get<BoolValue>(notifyScreen) && !shown.empty() && client->getGuiData()) {
            const Vec2 screenSize = client->getGuiData()->screenSize;
            const float ats = std::clamp(static_cast<float>(std::get<FloatValue>(notifyTextSize)), 8.f, 48.f);
            const float lineA = std::max(mc.getTextSize(L"Ag", font, ats, false).y, 6.f);
            const float padA = ats * 0.45f;
            const float gapA = ats * 0.3f;
            const int where = notifyPosition.getSelectedKey();
            const bool atTop = where == PosTopCenter || where == PosTopLeft || where == PosTopRight;
            const bool atBottom = where == PosBottomCenter || where == PosBottomLeft || where == PosBottomRight;
            const bool atLeft = where == PosTopLeft || where == PosBottomLeft;
            const bool atRight = where == PosTopRight || where == PosBottomRight;
            const float offX = static_cast<float>(std::get<FloatValue>(notifyOffsetX));
            const float offY = static_cast<float>(std::get<FloatValue>(notifyOffsetY));
            const float boxH = lineA + lineA * 0.8f + padA * 2.f;
            // The edge the first box starts from: the top edge going down, or (at the bottom) the bottom edge going up.
            float edge = atTop ? 30.f : atBottom ? screenSize.y - 70.f : screenSize.y * 0.5f - boxH * 0.5f;
            const d2d::Color gold = d2d::Color::RGB(255, 215, 0);
            for (auto const& [alert, opacity] : shown) {
                const Vec2 tt = mc.getTextSize(alert.title, font, ats, false);
                const Vec2 dt = mc.getTextSize(alert.detail, font, ats * 0.8f, false);
                const float w = std::max(tt.x, dt.x) + padA * 2.f;
                const float x = atLeft ? 10.f : atRight ? screenSize.x - w - 10.f : (screenSize.x - w) * 0.5f;
                const float top = (atBottom ? edge - boxH : edge) + offY;
                const d2d::Rect box(x + offX, top, x + offX + w, top + boxH);
                mc.fillRoundedRectangle(box, d2d::Color(0.03f, 0.03f, 0.03f, 0.82f * opacity), ats * 0.3f);
                mc.drawText(d2d::Rect(box.left + padA, box.top + padA, box.right - padA, box.top + padA + lineA), alert.title,
                            gold.asAlpha(opacity), font, ats, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                mc.drawText(d2d::Rect(box.left + padA, box.top + padA + lineA, box.right - padA, box.bottom - padA), alert.detail,
                            d2d::Colors::WHITE.asAlpha(opacity), font, ats * 0.8f, DWRITE_TEXT_ALIGNMENT_CENTER,
                            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                edge += atBottom ? -(boxH + gapA) : boxH + gapA;
            }
        }
    }

    // The icons: an item stack built from a vault block, pointed at each item in turn (the way WAILA draws its tool
    // icons). Without the block (chunk not loaded on your side) the icons are skipped and the text stays.
    if (jobs.empty()) {
        iconReport = "no icons to draw this frame";
    } else if (!iconBlock) {
        iconReport = "the vault block isn't loaded on your side, so no item stack could be built";
    } else if (!Signatures::ItemStackVtable.result || !Signatures::ItemStackBase_destructor.result) {
        iconReport = "the item stack signatures did not resolve";
    } else {
        alignas(SDK::ItemStack) char storage[sizeof(SDK::ItemStack)] = {};
        auto* stack = SDK::ItemStack::constructBlockItem(storage, *iconBlock);
        if (!stack) iconReport = "could not build an item stack from the vault block (BlockType::asItemInstance)";
        if (stack) {
            int iconsDrawn = 0;
            std::string missing;
            auto originalItem = stack->item;
            auto originalBlock = stack->block;
            auto originalAux = stack->aux;

            // The item registry is looked for a little each frame; the vault's own item is what it is recognised by.
            if (!registryFound) {
                if (!originalItem) registryNote = "the vault block's item stack has no item, so the item registry can't be recognised";
                else stepRegistrySearch(reinterpret_cast<uintptr_t>(originalItem));
            }

            mc.flush(false);
            for (auto const& job : jobs) {
                void* counter = findItemCounter(job.item);
                if (!counter) {
                    if (missing.size() < 80) missing += (missing.empty() ? "" : ", ") + job.item;
                    continue;
                }
                stack->item = reinterpret_cast<SDK::Item**>(counter);
                stack->block = nullptr;
                stack->aux = 0;
                stack->itemCount = static_cast<uint8_t>(std::clamp(job.count, 1, 64));
                // Same convention as WAILA: the size is the icon width in the units of the position, divided by 48.
                mc.drawItem(stack, job.pos, job.size / 48.f, 1.f);
                ++iconsDrawn;
            }
            iconReport = std::format("{} of {} icon(s) drawn{}{}; item registry: {}", iconsDrawn, jobs.size(),
                                     missing.empty() ? "" : ", not found in the registry: ", missing,
                                     registryNote.empty() ? std::string("not read yet") : registryNote);

            stack->item = originalItem;
            stack->block = originalBlock;
            stack->aux = originalAux;
            stack->destruct();
        }
    }

    mc.flush();
}
