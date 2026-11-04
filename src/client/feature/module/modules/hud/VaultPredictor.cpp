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
#include "mc/common/world/level/BlockSource.h"
#include "mc/common/world/level/HitResult.h"
#include "mc/common/world/level/block/Block.h"
#include "mc/common/world/level/block/BlockLegacy.h"
#include "util/Util.h"

#include <algorithm>
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

    // The seed packet comes once, when you join, so the server's seed is only known if the module was on at that moment.
    listen<PacketReceiveEvent>((EventListenerFunc)&VaultPredictor::onPacketReceive);
    listen<LeaveGameEvent>((EventListenerFunc)&VaultPredictor::onLeaveGame, true);
    listen<RenderLayerEvent>((EventListenerFunc)&VaultPredictor::onRenderLayer);
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
            const std::string id = out.mStr.substr(0, 400);
            std::lock_guard lock(mutex);
            if (ignoredIds.size() < 20 && ignoredIds.insert(id).second) {
                Logger::Info("[VaultPredictor] block entity data (not a vault): {}", id);
            }
        }
        return;
    }

    if (std::get<BoolValue>(debugMode)) {
        int64_t typedSeed = 0;
        const bool typed = parseSeed(std::get<TextValue>(seed).str, typedSeed);
        {
            std::lock_guard lock(mutex);
            Logger::Info("[VaultPredictor] seed: {} ({})", typed ? typedSeed : serverSeed,
                         typed ? "typed" : (haveServerSeed ? "from the server" : "unknown"));
        }
        Logger::Info("[VaultPredictor] vault data:\n{}\n  -> loot table '{}', override '{}', display item {} x{}, "
                     "items_to_eject field {} with {} item(s)",
                     out.mStr.substr(0, 6000), data.lootTable, data.overrideLootTable,
                     data.hasDisplay ? data.display.name : std::string("none"), data.hasDisplay ? data.display.count : 0,
                     data.hasEjectField ? "present" : "absent", data.toEject.size());
    }

    std::lock_guard lock(mutex);
    if (vaults.size() > kMaxVaults) vaults.clear();
    auto& entry = vaults[keyOf(x, y, z)];
    entry.pos = BlockPos { x, y, z };
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

void* VaultPredictor::findItemCounter(std::string const& id) {
    auto* client = SDK::ClientInstance::get();
    auto* level = client && client->minecraft ? client->minecraft->getLevel() : nullptr;
    if (!level) return nullptr;

    // The item registry, the same one WAILA reads its tool icons from: a list of item counters, each pointing at an Item.
    void* registry = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(level) + 0x198);
    if (!registry) return nullptr;

    if (registry != itemCacheRegistry || itemCache.empty()) {
        itemCache.clear();
        itemCacheRegistry = registry;
        auto itemCounters = reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(registry) + 0x38);
        if (!itemCounters[0] || !itemCounters[1]) return nullptr;
        for (auto current = itemCounters[0]; current < itemCounters[1]; ++current) {
            auto counter = *current;
            auto item = counter ? *reinterpret_cast<SDK::Item**>(counter) : nullptr;
            if (!item) continue;
            const std::string name = item->namespacedId.getString();
            if (!name.empty()) itemCache.emplace(name, counter);
        }
    }

    auto it = itemCache.find(id.find(':') == std::string::npos ? "minecraft:" + id : id);
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

    std::vector<IconJob> jobs;
    SDK::Block* iconBlock = nullptr;
    int drawn = 0;

    for (auto const& pick : picks) {
        if (drawn >= 6) break;

        // It must still be a vault; an empty name means the chunk isn't loaded on your side, which is fine.
        const std::string blockName = blockNameInner(pick.entry.pos);
        if (!blockName.empty() && blockName != "minecraft:vault") continue;

        auto const& data = pick.entry.data;
        const bool typeKnown = !data.lootTable.empty() || !data.overrideLootTable.empty();
        const bool ominous = typeKnown && vault_nbt::isOminous(data);
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

        // The columns: the rewards the server sent or, with none, the item shown in the vault (labelled).
        const bool shownOnly = data.toEject.empty() && data.hasDisplay;
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
        if (!data.toEject.empty()) {
            for (auto const& item : data.toEject) addColumn(item);
        } else if (data.hasDisplay) {
            addColumn(data.display);
        }

        // The title under the icons: the type and the distance.
        const std::wstring title = std::wstring(ominous ? L"Ominous Vault" : L"Vault") + L" " +
                                   std::to_wstring(static_cast<int>(std::lround(pick.dist))) + L"m";
        const d2d::Color titleColor = ominous ? d2d::Color::RGB(120, 200, 255) : d2d::Color::RGB(255, 170, 0);
        std::wstring note;
        if (shownOnly) note = L"shown in the vault, not a reward";
        else if (cols.empty()) note = L"no reward data from the server";

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
    }

    // The icons: an item stack built from a vault block, pointed at each item in turn (the way WAILA draws its tool
    // icons). Without the block (chunk not loaded on your side) the icons are skipped and the text stays.
    if (!jobs.empty() && iconBlock && Signatures::ItemStackVtable.result && Signatures::ItemStackBase_destructor.result) {
        alignas(SDK::ItemStack) char storage[sizeof(SDK::ItemStack)] = {};
        if (auto* stack = SDK::ItemStack::constructBlockItem(storage, *iconBlock)) {
            auto originalItem = stack->item;
            auto originalBlock = stack->block;
            auto originalAux = stack->aux;

            mc.flush(false);
            for (auto const& job : jobs) {
                void* counter = findItemCounter(job.item);
                if (!counter) continue;
                stack->item = reinterpret_cast<SDK::Item**>(counter);
                stack->block = nullptr;
                stack->aux = 0;
                stack->itemCount = static_cast<uint8_t>(std::clamp(job.count, 1, 64));
                // drawItem's size is a multiplier of 16 gui units: pixels * guiScale (= gui units per pixel) / 16.
                mc.drawItem(stack, job.pos, job.size * mc.guiScale / 16.f, 1.f);
            }

            stack->item = originalItem;
            stack->block = originalBlock;
            stack->aux = originalAux;
            stack->destruct();
        }
    }

    mc.flush();
}
