#include "pch.h"
#include "VaultPredictor.h"

#include "client/Latite.h"
#include "client/event/events/LeaveGameEvent.h"
#include "client/event/events/PacketReceiveEvent.h"
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
    addSetting("showIcons", LocalizeString::get("client.textmodule.vaultPredictor.showIcons.name"),
               LocalizeString::get("client.textmodule.vaultPredictor.showIcons.desc"), showIcons);
    addSliderSetting("iconSize", LocalizeString::get("client.textmodule.vaultPredictor.iconSize.name"),
                     LocalizeString::get("client.textmodule.vaultPredictor.iconSize.desc"), iconSize, FloatValue(8.f),
                     FloatValue(128.f), FloatValue(2.f), "showIcons"_istrue);
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
            const std::string id = out.mStr.substr(0, 60);
            std::lock_guard lock(mutex);
            if (ignoredIds.size() < 20 && ignoredIds.insert(id).second) {
                Logger::Info("[VaultPredictor] block entity data (not a vault): {}", id);
            }
        }
        return;
    }

    if (std::get<BoolValue>(debugMode)) {
        Logger::Info("[VaultPredictor] vault data:\n{}\n  -> loot table '{}', override '{}', display item {} x{}, "
                     "items_to_eject field {} with {} item(s)",
                     out.mStr.substr(0, 2500), data.lootTable, data.overrideLootTable,
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

void VaultPredictor::addItem(std::vector<Row>& rows, vault_nbt::ItemInfo const& item) {
    Row row;
    row.kind = RowKind::Item;
    row.item = item.name;
    row.count = item.count;
    if (std::get<BoolValue>(showNames)) {
        row.text = L"\u00A7f" + util::StrToWStr(vault_nbt::prettyName(item.name)) + L"\u00A7r";
    } else {
        row.text = L"\u00A7fItem\u00A7r";
    }
    if (std::get<BoolValue>(showAmounts)) row.text += L" \u00A77x" + std::to_wstring(item.count) + L"\u00A7r";
    rows.push_back(std::move(row));

    if (std::get<BoolValue>(showEnchantments)) {
        for (auto const& e : item.enchants) {
            Row detail;
            detail.kind = RowKind::Detail;
            detail.text = L"\u00A7d" + util::StrToWStr(vault_nbt::enchantName(e.id)) + L" " +
                          util::StrToWStr(vault_nbt::roman(e.level)) + L"\u00A7r";
            rows.push_back(std::move(detail));
        }
    }
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

std::vector<VaultPredictor::Row> VaultPredictor::buildRows(bool preview, SDK::Block*& block) {
    block = nullptr;
    std::vector<Row> rows;
    auto header = [&](std::wstring text) {
        Row row;
        row.text = std::move(text);
        rows.push_back(std::move(row));
    };
    const int wanted = vaultType.getSelectedKey();

    // What the seed setting and the server's seed say.
    std::wstring seedLine;
    {
        int64_t value = 0;
        const bool typed = parseSeed(std::get<TextValue>(seed).str, value);
        std::lock_guard lock(mutex);
        if (typed) seedLine = L"§7Seed: " + std::to_wstring(value) + L" (typed)§r";
        else if (haveServerSeed) seedLine = L"§7Seed: " + std::to_wstring(serverSeed) + L" (from the server)§r";
        else seedLine = L"§7Seed: unknown§r";
    }

    // The block the icons are built from: the vault in the crosshair (or any block there, for the sample).
    auto* client = SDK::ClientInstance::get();
    auto* level = client && client->minecraft ? client->minecraft->getLevel() : nullptr;
    auto* hit = level ? level->getHitResult() : nullptr;
    const bool aimingAtBlock = hit && hit->hitType == SDK::HitType::BLOCK;
    if (aimingAtBlock) {
        if (auto* region = client->getRegion()) block = region->getBlock(hit->hitBlock);
    }

    if (preview) {
        header(L"§6Vault Predictor§r");
        header(L"§bNormal Vault§r");
        header(seedLine);
        header(L"§aRewards sent by the server:§r");
        vault_nbt::ItemInfo book;
        book.name = "minecraft:enchanted_book";
        book.enchants.push_back({ 9, 5 });
        addItem(rows, book);
        vault_nbt::ItemInfo emerald;
        emerald.name = "minecraft:emerald";
        emerald.count = 3;
        addItem(rows, emerald);
        header(L"§8(sample)§r");
        return rows;
    }

    if (!aimingAtBlock || blockNameInner(hit->hitBlock) != "minecraft:vault") {
        block = nullptr;
        return rows;
    }

    const BlockPos pos = hit->hitBlock;
    const uint64_t key = keyOf(pos.x, pos.y, pos.z);
    vault_nbt::VaultData data;
    bool known = false;
    {
        std::lock_guard lock(mutex);
        if (auto it = vaults.find(key); it != vaults.end()) {
            data = it->second.data;
            known = true;
        } else if (std::get<BoolValue>(debugMode) && loggedNoData.size() < 64 && loggedNoData.insert(key).second) {
            Logger::Info("[VaultPredictor] looking at a vault: no data for it from the server yet");
        }
    }

    // The type, from the loot table. Without data the type isn't known, and then it is shown whatever the setting says.
    const bool ominous = known && vault_nbt::isOminous(data);
    if (known && !data.lootTable.empty()) {
        if (wanted == TypeNormal && ominous) return rows;
        if (wanted == TypeOminous && !ominous) return rows;
    }

    header(L"§6Vault Predictor§r");
    if (known && !data.lootTable.empty()) header(ominous ? L"§5Ominous Vault§r" : L"§bNormal Vault§r");
    else header(L"§7Vault (type not received)§r");
    header(seedLine);

    if (!known) {
        header(L"§7No data from the server for this vault yet.§r");
        return rows;
    }

    if (!data.toEject.empty()) {
        header(L"§aRewards sent by the server:§r");
        for (auto const& item : data.toEject) addItem(rows, item);
    } else {
        header(L"§7Rewards: the server hasn't sent any for this vault.§r");
    }
    if (data.hasDisplay) {
        header(L"§8Shown in the vault (not a reward):§r");
        addItem(rows, data.display);
    }
    return rows;
}

void VaultPredictor::render(DrawUtil& dc, bool isDefault, bool inEditor) {
    if (!dc.isMinecraft()) return;
    auto& mc = static_cast<MCDrawUtil&>(dc);

    SDK::Block* block = nullptr;
    const auto rows = buildRows(isDefault || inEditor, block);
    if (rows.empty()) return; // Not looking at a (wanted) vault: nothing is drawn.

    const bool fill = std::get<BoolValue>(fillBg);
    const bool outline = std::get<BoolValue>(showOutline);
    const bool icons = std::get<BoolValue>(showIcons);
    const float textSize = std::get<FloatValue>(textSizeS);
    const float padXv = fill ? static_cast<float>(std::get<FloatValue>(padX)) : 0.f;
    const float padYv = fill ? static_cast<float>(std::get<FloatValue>(padY)) * 2.f : 0.f;
    const d2d::Color bgCol = std::get<ColorValue>(bgColor).getMainColor();
    const d2d::Color textCol = std::get<ColorValue>(textColor).getMainColor();
    const d2d::Color outlineCol = std::get<ColorValue>(outlineColor).getMainColor();

    const auto font = Renderer::FontSelection::SecondaryLight;
    const float textH = std::max(mc.getTextSize(L"Ag", font, textSize, false).y, 8.f);
    // Clamped to the slider range, so a value hand-edited into the config can't blow up the layout.
    const float icon = std::clamp(static_cast<float>(std::get<FloatValue>(iconSize)), 8.f, 128.f);
    const float iconGap = std::max(4.f, icon * 0.25f);
    const float indent = icons ? icon + iconGap : 0.f; // Item and detail lines start after the icon column.

    // Row heights and the widest line.
    std::vector<float> heights;
    heights.reserve(rows.size());
    float width = 0.f;
    float totalH = 0.f;
    for (auto const& row : rows) {
        const float h = row.kind == RowKind::Item && icons ? std::max(textH, icon) : textH;
        heights.push_back(h);
        totalH += h;
        const float left = row.kind == RowKind::Header ? 0.f : indent;
        width = std::max(width, left + mc.getTextSize(row.text, font, textSize, false).x);
    }
    width += padXv * 2.f;
    totalH += padYv * 2.f;

    const d2d::Rect rc = d2d::Rect(0.f, 0.f, width, totalH);
    const float rad = (static_cast<float>(std::get<FloatValue>(radius)) / 10.f) * (rc.getHeight() / 2.f);
    if (outline) mc.drawRoundedRectangle(rc, outlineCol, rad, std::get<FloatValue>(outlineThickness));
    if (fill) mc.fillRoundedRectangle(rc, bgCol, rad);

    float y = padYv;
    std::vector<float> tops; // where each row starts, for the icons
    tops.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const float left = padXv + (rows[i].kind == RowKind::Header ? 0.f : indent);
        mc.drawText(d2d::Rect(left, y, width - padXv, y + heights[i]), rows[i].text, textCol, font, textSize,
                    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        tops.push_back(y);
        y += heights[i];
    }

    this->rect.right = rect.left + width;
    this->rect.bottom = rect.top + totalH;

    // The icons: an item stack built from the vault block, pointed at each item in turn (the way WAILA draws its tool
    // icons). Needs the vault block; without it (the HUD editor sample away from a block) the icon column stays empty.
    if (icons && block && Signatures::ItemStackVtable.result && Signatures::ItemStackBase_destructor.result) {
        alignas(SDK::ItemStack) char storage[sizeof(SDK::ItemStack)] = {};
        if (auto* stack = SDK::ItemStack::constructBlockItem(storage, *block)) {
            auto originalItem = stack->item;
            auto originalBlock = stack->block;
            auto originalAux = stack->aux;

            mc.flush(false);
            for (size_t i = 0; i < rows.size(); ++i) {
                if (rows[i].kind != RowKind::Item || rows[i].item.empty()) continue;
                void* counter = findItemCounter(rows[i].item);
                if (!counter) continue;
                stack->item = reinterpret_cast<SDK::Item**>(counter);
                stack->block = nullptr;
                stack->aux = 0;
                stack->itemCount = static_cast<uint8_t>(std::clamp(rows[i].count, 1, 64));
                const float iconTop = tops[i] + (heights[i] - icon) / 2.f;
                mc.drawItem(stack, { padXv, iconTop }, icon / 48.f, 1.f);
            }

            stack->item = originalItem;
            stack->block = originalBlock;
            stack->aux = originalAux;
            stack->destruct();
        }
    }

    mc.flush();
}
