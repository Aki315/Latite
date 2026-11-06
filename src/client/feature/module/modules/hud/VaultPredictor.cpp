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

void* VaultPredictor::findItemCounter(std::string const& id) {
    auto* client = SDK::ClientInstance::get();
    auto* level = client && client->minecraft ? client->minecraft->getLevel() : nullptr;
    if (!level) {
        registryNote = "no level";
        return nullptr;
    }

    // The item registry, the same one WAILA reads its tool icons from: a list of item counters, each pointing at an Item.
    void* registry = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(level) + 0x198);
    if (!registry) {
        registryNote = "the item registry pointer (Level + 0x198) is null";
        return nullptr;
    }

    if (registry != itemCacheRegistry || itemCache.empty()) {
        itemCache.clear();
        itemCacheRegistry = registry;
        auto itemCounters = reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(registry) + 0x38);
        if (!itemCounters[0] || !itemCounters[1]) {
            registryNote = "the item list (registry + 0x38) is empty";
            return nullptr;
        }
        size_t slots = 0, withItem = 0;
        std::string examples;
        for (auto current = itemCounters[0]; current < itemCounters[1]; ++current) {
            ++slots;
            auto counter = *current;
            auto item = counter ? *reinterpret_cast<SDK::Item**>(counter) : nullptr;
            if (!item) continue;
            ++withItem;
            const std::string name = item->namespacedId.getString();
            if (name.empty()) continue;
            itemCache.emplace(name, counter);
            // Also under the short name, in case the game keeps the ids without "minecraft:".
            if (name.rfind("minecraft:", 0) == 0) itemCache.emplace(name.substr(10), counter);
            if (examples.size() < 60) examples += (examples.empty() ? "" : ", ") + name;
        }
        registryNote = std::format("{} slot(s), {} with an item, {} names read (e.g. {})", slots, withItem, itemCache.size(),
                                   examples.empty() ? std::string("none") : examples);
    }

    auto it = itemCache.find(id.find(':') == std::string::npos ? "minecraft:" + id : id);
    if (it == itemCache.end()) it = itemCache.find(id);
    return it == itemCache.end() ? nullptr : it->second;
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
    };

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
    {
        std::lock_guard lock(mutex);
        for (auto const& [k, entry] : vaults) {
            const float d = me.distance(Vec3 { entry.pos.x + 0.5f, entry.pos.y + 0.5f, entry.pos.z + 0.5f });
            if (d <= maxDist) picks.push_back({ entry, d });
        }
    }
    if (picks.empty()) return;
    std::sort(picks.begin(), picks.end(), [](Pick const& a, Pick const& b) { return a.dist < b.dist; });

    const int wanted = vaultType.getSelectedKey();
    const bool icons = std::get<BoolValue>(showIcons);
    const bool names = std::get<BoolValue>(showNames);
    const bool amounts = std::get<BoolValue>(showAmounts);
    const bool enchants = std::get<BoolValue>(showEnchantments);
    const bool background = std::get<BoolValue>(showBackground);
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

    for (auto const& pick : picks) {
        if (drawn >= 6) break;

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
            if (pick.entry.sawOminousOnly != pick.entry.sawNormalOnly) {
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

        // Above the vault, like a name tag. It shrinks with distance so far vaults don't fill the screen.
        const Vec3 anchorWorld { pick.entry.pos.x + 0.5f, pick.entry.pos.y + 1.15f, pick.entry.pos.z + 0.5f };
        auto screen = WorldToScreen::convert(anchorWorld, true);
        if (!screen) continue;
        const float scale = std::clamp(8.f / std::max(pick.dist, 1.f), 0.55f, 1.f);
        const float ts = baseText * scale;
        const float S = baseIcon * scale;
        const float gap = 6.f * scale;
        const float pad = 5.f * scale;

        // The columns: only rewards the server really sent. The item spinning in the vault (display_item) is not a reward
        // and is never drawn.
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
        for (auto const& item : data.toEject) addColumn(item);

        // No rewards from the server: work them out. The loot depends on the vault type, so an unknown type can't be done.
        std::wstring note;
        bool predictedShown = false;
        if (cols.empty()) {
            if (!typeKnown) {
                note = L"vault type unknown: wait for the vault item to change, or set Vault Type";
            } else if (!haveCalcSeed) {
                note = L"no valid world seed (see the World Seed setting)";
            } else if (!havePlayer) {
                note = L"player id unknown (see the Player ID setting)";
            } else {
                const auto result = vault_loot::predict(calcSeed, calcPlayer, pick.entry.pos.x, pick.entry.pos.y,
                                                        pick.entry.pos.z, ominous);
                for (auto const& s : result.items) {
                    vault_nbt::ItemInfo info;
                    info.name = s.item;
                    info.count = s.count;
                    addColumn(info);
                    auto& lines = cols.back().lines;
                    if (names && !s.variant.empty()) {
                        if (s.item == "minecraft:ominous_bottle") {
                            // The amplifier counts from 0; the bottle's name shows the Bad Omen level.
                            lines.push_back({ L"Bad Omen " + util::StrToWStr(vault_nbt::roman(std::atoi(s.variant.c_str()) + 1)),
                                              d2d::Color::RGB(170, 170, 255), 0.8f });
                        } else {
                            lines.push_back({ util::StrToWStr(vault_nbt::prettyName(s.variant)), d2d::Color::RGB(170, 170, 255),
                                              0.8f });
                        }
                    }
                    if (!s.enchant.empty() && enchants) {
                        lines.push_back({ util::StrToWStr(vault_nbt::prettyName(s.enchant)) + L" " +
                                              util::StrToWStr(vault_nbt::roman(s.enchantLevel)),
                                          d2d::Color::RGB(255, 170, 255), 0.8f });
                    }
                    if (s.details && enchants) {
                        lines.push_back({ s.item == "minecraft:shield" ? L"damage unknown" : L"enchantments unknown",
                                          d2d::Color::RGB(170, 170, 170), 0.8f });
                    }
                }
                predictedShown = !cols.empty();
                if (!result.complete) note = L"the rest can't be predicted yet (enchanted item)";
                else if (cols.empty()) note = L"nothing ejected";
            }
        }

        // The title under the icons: the type and the distance.
        const std::wstring title = std::wstring(ominous ? L"Ominous Vault" : L"Vault") + L" " +
                                   std::to_wstring(static_cast<int>(std::lround(pick.dist))) + L"m" +
                                   (predictedShown ? L" (predicted)" : L"");
        const d2d::Color titleColor = ominous ? d2d::Color::RGB(120, 200, 255) : d2d::Color::RGB(255, 170, 0);

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
        const float bottom = screen->y;
        const float top = bottom - totalH;
        if (background) {
            mc.fillRoundedRectangle(d2d::Rect(left - pad, top, left + contentW + pad, bottom), d2d::Color(0.f, 0.f, 0.f, 0.4f),
                                    ts * 0.3f);
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
                    mc.drawText(d2d::Rect(colCenter + S * 0.5f - nb.x, y + S - nb.y, colCenter + S * 0.5f + 2.f, y + S + 2.f), n,
                                d2d::Colors::WHITE, font, ts * 0.8f, DWRITE_TEXT_ALIGNMENT_TRAILING,
                                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }
                y += S + gap * 0.5f;
            } else if (amounts && cols[i].item.count > 1 && !cols[i].lines.empty()) {
                cols[i].lines[0].text += L" x" + std::to_wstring(cols[i].item.count);
            }
            for (auto const& line : cols[i].lines) {
                mc.drawText(d2d::Rect(colCenter - colW[i] * 0.5f, y, colCenter + colW[i] * 0.5f, y + lineH * 0.85f), line.text,
                            line.color, font, ts * line.scale, DWRITE_TEXT_ALIGNMENT_CENTER,
                            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                y += lineH * 0.85f;
            }
            x += colW[i];
        }

        // The note and the title at the bottom, next to the vault.
        float ty = top + pad + iconH + textH;
        if (!note.empty()) {
            mc.drawText(d2d::Rect(left, ty, left + contentW, ty + noteH), note, d2d::Color::RGB(170, 170, 170), font, ts * 0.8f,
                        DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ty += noteH;
        }
        mc.drawText(d2d::Rect(left, ty, left + contentW, ty + lineH), title, titleColor, font, ts, DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

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
                    "  Vault detected: OK\n"
                    "  Position: {}, {}, {}\n"
                    "  Vault type: {}\n"
                    "  Player id: {}\n"
                    "  Server reward data (items_to_eject): {}\n"
                    "  Predicted items: {}\n"
                    "  Icons (last frame): {}\n"
                    "  HUD: {}",
                    haveSeed ? std::format("OK ({})", worldSeed) : std::string("NOT RECEIVED (join the world again with the mod loaded)"),
                    manual, match, pick.entry.pos.x, pick.entry.pos.y, pick.entry.pos.z,
                    typeKnown ? std::format("{} ({})", ominous ? "Ominous" : "Normal", typeSource)
                              : std::string("UNKNOWN (no loot table sent; no item seen yet that only one type has)"),
                    havePlayer ? std::format("{}", calcPlayer) : std::string("UNKNOWN (set the Player ID setting)"),
                    data.hasEjectField ? std::to_string(data.toEject.size()) + " item(s) sent" : std::string("not sent"),
                    predictedShown ? std::format("{} column(s) calculated (seed {} used)", cols.size(), calcSeed)
                                   : std::string("none"),
                    iconReport.empty() ? std::string("not drawn yet") : iconReport,
                    cols.empty() ? "title and note only (no rewards to draw)" : "rewards drawn");
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
