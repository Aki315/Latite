#include "pch.h"
#include "ContainerLogger.h"

#include "client/Latite.h"
#include "client/event/events/LeaveGameEvent.h"
#include "client/event/events/PacketReceiveEvent.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/nbt/Tag.h"
#include "mc/common/network/Packet.h"
#include "mc/common/world/level/Dimension.h"
#include "mc/common/world/actor/Actor.h"
#include "mc/common/world/actor/player/Player.h"
#include "mc/common/world/level/BlockSource.h"
#include "mc/common/world/level/block/Block.h"
#include "mc/common/world/level/block/BlockLegacy.h"
#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <functional>
#include <string_view>

namespace {
    // Packet ids (Bedrock protocol, official docs): BlockEvent 26, ContainerOpen 46, ContainerClose 47,
    // InventoryContent 49, InventorySlot 50, BlockActorData 56, ChangeDimension 61.
    constexpr uint8_t kBlockEvent = 26;
    constexpr uint8_t kContainerOpen = 46;
    constexpr uint8_t kContainerClose = 47;
    constexpr uint8_t kInventoryContent = 49;
    constexpr uint8_t kInventorySlot = 50;
    constexpr uint8_t kBlockActorData = 56;
    constexpr uint8_t kChangeDimension = 61;

    // BlockEventPacket: for chests, trapped chests, ender chests and shulker boxes the event type is 1 and the data is
    // the number of players that have it open (the server sends 1 when it opens and 0 when it closes).
    constexpr int kChestEventType = 1;

    // Field offsets inside the packet objects. They follow the layout of the game's own packet classes: the Packet base
    // class takes 0x30 bytes (the seed in a StartGamePacket is at 0x30 too), and the fields come after it in the order
    // of the protocol. NOT yet verified in a running game for these packets: every read below checks that the values
    // are plausible and quietly gives up otherwise, and Debug Mode writes the first bytes of each packet to the log so
    // the layout can be confirmed.
    constexpr size_t kPosOffset = 0x30; // NetworkBlockPosition { int x, y, z } in BlockEvent, ContainerOpen, BlockActorData
    constexpr size_t kBlockEventType = 0x3C;
    constexpr size_t kBlockEventData = 0x40;
    constexpr size_t kOpenWindowId = 0x30;
    constexpr size_t kOpenWindowType = 0x31;
    constexpr size_t kOpenPos = 0x34;
    constexpr size_t kOpenEntity = 0x40; // target actor unique id, -1 for a block
    constexpr size_t kCloseWindowId = 0x30;
    constexpr size_t kBlockActorTag = 0x40; // the CompoundTag member
    constexpr size_t kInventoryWindowId = 0x30;

    constexpr size_t kMaxTracked = 512;
    constexpr int kMaxItemLines = 54;

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

    // Name of the block at a position (as the client knows it), copied into a buffer. An access violation returns
    // false. Split in two because __try can't be in a function that has objects with destructors.
    void blockIdInner(int x, int y, int z, char* out, size_t size) {
        auto* ci = SDK::ClientInstance::get();
        auto* region = ci ? ci->getRegion() : nullptr;
        if (!region) return;
        SDK::Block* block = region->getBlock(BlockPos { x, y, z });
        if (!block || !block->legacyBlock) return;
        const std::string id = block->legacyBlock->namespacedId.getString();
        std::strncpy(out, id.c_str(), size - 1);
        out[size - 1] = '\0';
    }

    bool blockIdRaw(int x, int y, int z, char* buf, size_t size) {
        __try {
            blockIdInner(x, y, z, buf, size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::string blockIdAt(int x, int y, int z) {
        char buf[96] = {};
        if (!blockIdRaw(x, y, z, buf, sizeof(buf))) return {};
        return buf;
    }

    void dimensionNameInner(char* out, size_t size) {
        auto* ci = SDK::ClientInstance::get();
        auto* lp = ci ? ci->getLocalPlayer() : nullptr;
        if (!lp || !lp->dimension) return;
        const std::string& name = lp->dimension->dimensionName;
        if (name.empty() || name.size() > 40) return;
        std::strncpy(out, name.c_str(), size - 1);
        out[size - 1] = '\0';
    }

    bool dimensionNameRaw(char* buf, size_t size) {
        __try {
            dimensionNameInner(buf, size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::string currentDimension() {
        char buf[48] = {};
        if (!dimensionNameRaw(buf, sizeof(buf))) return {};
        return buf;
    }

    std::string_view stripNamespace(std::string_view id) {
        const size_t colon = id.find(':');
        return colon == std::string_view::npos ? id : id.substr(colon + 1);
    }

    // The container blocks this module knows by name. `animated` is true for the ones whose open/close the server shows
    // to other players with a block event (chests and shulker boxes).
    struct ContainerInfo {
        const char* name;
        bool animated;
    };

    bool containerFromBlock(std::string_view fullId, ContainerInfo& out) {
        const std::string_view id = stripNamespace(fullId);
        if (id == "chest") out = { "Chest", true };
        else if (id == "trapped_chest") out = { "Trapped Chest", true };
        else if (id == "ender_chest") out = { "Ender Chest", true };
        else if (id.ends_with("shulker_box")) out = { "Shulker Box", true };
        else if (id == "barrel") out = { "Barrel", false };
        else if (id == "hopper") out = { "Hopper", false };
        else if (id == "furnace" || id == "lit_furnace") out = { "Furnace", false };
        else if (id == "blast_furnace" || id == "lit_blast_furnace") out = { "Blast Furnace", false };
        else if (id == "smoker" || id == "lit_smoker") out = { "Smoker", false };
        else if (id == "brewing_stand") out = { "Brewing Stand", false };
        else if (id == "dispenser") out = { "Dispenser", false };
        else if (id == "dropper") out = { "Dropper", false };
        else return false;
        return true;
    }

    // ContainerOpenPacket's window type (the protocol's container type). Only the storage-like windows are logged;
    // crafting tables, anvils and so on are not containers that hold items.
    const char* windowTypeName(int type) {
        switch (type) {
        case 0: return "Container";
        case 2: return "Furnace";
        case 4: return "Brewing Stand";
        case 6: return "Dispenser";
        case 7: return "Dropper";
        case 8: return "Hopper";
        case 27: return "Blast Furnace";
        case 28: return "Smoker";
        default: return nullptr;
        }
    }

    // BlockActorData's "id" field to a container name (block entity ids).
    const char* containerFromBlockEntityId(std::string_view id) {
        if (id == "Chest") return "Chest";
        if (id == "EnderChest") return "Ender Chest";
        if (id == "ShulkerBox") return "Shulker Box";
        if (id == "Barrel") return "Barrel";
        if (id == "Hopper") return "Hopper";
        if (id == "Furnace") return "Furnace";
        if (id == "BlastFurnace") return "Blast Furnace";
        if (id == "Smoker") return "Smoker";
        if (id == "BrewingStand") return "Brewing Stand";
        if (id == "Dispenser") return "Dispenser";
        if (id == "Dropper") return "Dropper";
        return nullptr;
    }

    // The game's own printing of a tag, in a function of its own so that an access violation is caught (same approach as
    // the .nbt command).
    void printTagInner(SDK::Tag* tag, SDK::BasicPrintStream* out) { tag->print("", *out); }

    bool printTag(SDK::Tag* tag, SDK::BasicPrintStream* out) {
        __try {
            // A compound tag has id 10. Anything else means the packet isn't laid out the way this expects.
            if (tag->getId() != 10) return false;
            printTagInner(tag, out);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool looksLikeGameObject(const void* p) {
        uintptr_t vtable = 0;
        if (!readRaw(p, 0, vtable)) return false;
        static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        return vtable > base && vtable < base + 0x40000000ull;
    }

    // Reads "Name"/"Count" pairs out of the printed NBT. The items of a container are compounds that hold Count, Damage,
    // Name, ... in that (alphabetical) order, so the Count seen last before a Name belongs to it. Returns what it found;
    // an empty result means the tag has no item list that could be read (not every block entity update has one).
    std::vector<std::pair<std::string, int>> parseItems(std::string const& text) {
        std::vector<std::pair<std::string, int>> items;
        if (text.find("Items") == std::string::npos) return items;

        int count = 1;
        size_t pos = 0;
        while (pos < text.size()) {
            const size_t c = text.find("Count", pos);
            const size_t n = text.find("Name", pos);
            if (c == std::string::npos && n == std::string::npos) break;
            if (c != std::string::npos && (n == std::string::npos || c < n)) {
                size_t i = c + 5;
                while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i])) && text[i] != '-' &&
                       text[i] != '\n')
                    ++i;
                if (i < text.size() && text[i] != '\n') count = std::atoi(text.c_str() + i);
                pos = c + 5;
            } else {
                size_t i = n + 4;
                while (i < text.size() && (text[i] == ':' || text[i] == ' ' || text[i] == '"' || text[i] == '\t')) ++i;
                size_t end = i;
                while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_' ||
                                             text[end] == ':' || text[end] == '.' || text[end] == '/'))
                    ++end;
                if (end > i) {
                    std::string name(stripNamespace(std::string_view(text).substr(i, end - i)));
                    if (!name.empty()) items.emplace_back(std::move(name), count);
                }
                count = 1;
                pos = n + 4;
            }
        }
        return items;
    }

    std::string blockEntityId(std::string const& text) {
        // Looks like: id: "Chest"  (the exact punctuation depends on the game's printer).
        size_t at = 0;
        while ((at = text.find("id", at)) != std::string::npos) {
            const bool wordStart = at == 0 || !std::isalnum(static_cast<unsigned char>(text[at - 1]));
            size_t i = at + 2;
            if (wordStart && i < text.size() && (text[i] == ':' || text[i] == ' ' || text[i] == '"' || text[i] == '=')) {
                while (i < text.size() && (text[i] == ':' || text[i] == ' ' || text[i] == '"' || text[i] == '=')) ++i;
                size_t end = i;
                while (end < text.size() && std::isalnum(static_cast<unsigned char>(text[end]))) ++end;
                if (end > i) return text.substr(i, end - i);
            }
            at += 2;
        }
        return {};
    }

    std::string timeNow() {
        const std::time_t t = std::time(nullptr);
        std::tm tm {};
        localtime_s(&tm, &t);
        return std::format("{:02}:{:02}:{:02}", tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
} // namespace

ContainerLogger::ContainerLogger()
    : Module("ContainerLogger", LocalizeString::get("client.module.containerLogger.name"),
             LocalizeString::get("client.module.containerLogger.desc"), GAME) {
    addSetting("logOpen", LocalizeString::get("client.module.containerLogger.logOpen.name"),
               LocalizeString::get("client.module.containerLogger.logOpen.desc"), logOpen);
    addSetting("logClose", LocalizeString::get("client.module.containerLogger.logClose.name"),
               LocalizeString::get("client.module.containerLogger.logClose.desc"), logClose);
    addSetting("logContents", LocalizeString::get("client.module.containerLogger.logContents.name"),
               LocalizeString::get("client.module.containerLogger.logContents.desc"), logContents);
    addSetting("showType", LocalizeString::get("client.module.containerLogger.showType.name"),
               LocalizeString::get("client.module.containerLogger.showType.desc"), showType);
    addSetting("showCoords", LocalizeString::get("client.module.containerLogger.showCoords.name"),
               LocalizeString::get("client.module.containerLogger.showCoords.desc"), showCoords);
    addSetting("showTime", LocalizeString::get("client.module.containerLogger.showTime.name"),
               LocalizeString::get("client.module.containerLogger.showTime.desc"), showTime);
    addSetting("debugMode", LocalizeString::get("client.module.containerLogger.debugMode.name"),
               LocalizeString::get("client.module.containerLogger.debugMode.desc"), debugMode);

    // The packet hooks already exist (PacketHooks sends every incoming packet as a PacketReceiveEvent): nothing new is
    // hooked here. Leaving the game clears the state even while the module is off.
    listen<PacketReceiveEvent>((EventListenerFunc)&ContainerLogger::onPacketReceive);
    listen<LeaveGameEvent>((EventListenerFunc)&ContainerLogger::onLeaveGame, true);
}

void ContainerLogger::reset() {
    blocks.clear();
    windows.clear();
    lastLocalOpen.clear();
    lastLocalClose.clear();
    lastContentsHash.clear();
    lastReport[0].clear();
    lastReport[1].clear();
}

void ContainerLogger::onEnable() { reset(); }

void ContainerLogger::onLeaveGame(Event&) { reset(); }

void ContainerLogger::line(std::string const& text) {
    Latite::getClientMessageQueue().push(util::Format("&b[ContainerLogger]&r " + text));
}

void ContainerLogger::debugLine(std::string const& text) {
    if (!std::get<BoolValue>(debugMode)) return;
    Logger::Info("[ContainerLogger] {}", text);
}

void ContainerLogger::onPacketReceive(Event& evG) {
    auto& ev = reinterpret_cast<PacketReceiveEvent&>(evG);
    SDK::Packet* pkt = ev.getPacket();
    if (!pkt) return;

    const uint8_t id = static_cast<uint8_t>(pkt->getID());
    switch (id) {
    case kBlockEvent:
        debugDump(pkt, id);
        handleBlockEvent(pkt);
        break;
    case kContainerOpen:
        debugDump(pkt, id);
        handleContainerOpen(pkt);
        break;
    case kContainerClose:
        debugDump(pkt, id);
        handleContainerClose(pkt);
        break;
    case kBlockActorData:
        debugDump(pkt, id);
        handleBlockActorData(pkt);
        break;
    case kInventoryContent:
        debugDump(pkt, id);
        handleInventory(pkt, true);
        break;
    case kInventorySlot:
        debugDump(pkt, id);
        handleInventory(pkt, false);
        break;
    case kChangeDimension:
        // The old dimension's containers are gone.
        reset();
        break;
    default:
        break;
    }
}

// Writes the first bytes of the packet to the log (a few times per packet type) so the field offsets can be checked.
void ContainerLogger::debugDump(SDK::Packet* pkt, uint8_t id) {
    if (!std::get<BoolValue>(debugMode)) return;
    int& count = dumped[id];
    if (count >= 12) return;
    ++count;
    uint8_t bytes[0x60] = {};
    if (!readRaw(pkt, 0, bytes)) return;
    std::string hex;
    for (size_t row = 0; row < sizeof(bytes); row += 16) {
        hex += std::format("\n  +0x{:02X}:", row);
        for (size_t i = row; i < row + 16; ++i) hex += std::format(" {:02X}", bytes[i]);
    }
    Logger::Info("[ContainerLogger] packet {} raw bytes:{}", static_cast<int>(id), hex);
}

void ContainerLogger::handleBlockEvent(SDK::Packet* pkt) {
    struct Raw {
        int x, y, z, type, data;
    } raw {};
    if (!readRaw(pkt, kPosOffset, raw.x) || !readRaw(pkt, kPosOffset + 4, raw.y) ||
        !readRaw(pkt, kPosOffset + 8, raw.z) || !readRaw(pkt, kBlockEventType, raw.type) ||
        !readRaw(pkt, kBlockEventData, raw.data))
        return;
    if (!plausiblePos(raw.x, raw.y, raw.z) || raw.type < 0 || raw.type > 64 || raw.data < 0 || raw.data > 4096) {
        debugLine("block event ignored: values don't look like a block event (layout wrong?)");
        return;
    }

    // Block events are also used by pistons, bells, note blocks and more. Only a container block counts, and the
    // client's own copy of the world says which block this is.
    const std::string blockId = blockIdAt(raw.x, raw.y, raw.z);
    ContainerInfo info {};
    if (raw.type != kChestEventType || blockId.empty() || !containerFromBlock(blockId, info) || !info.animated) {
        debugLine(std::format("block event at {} {} {} ignored: type {}, data {}, block '{}'", raw.x, raw.y, raw.z,
                              raw.type, raw.data, blockId.empty() ? "unknown" : blockId));
        return;
    }

    if (blocks.size() > kMaxTracked) blocks.clear();
    const uint64_t key = keyOf(raw.x, raw.y, raw.z);
    auto it = blocks.find(key);
    const bool known = it != blocks.end();
    const int before = known ? it->second.openCount : 0;
    blocks[key].openCount = raw.data;

    // The same data again is a repeat, not an event. A container that stays open while another player joins it
    // (1 -> 2) or leaves it (2 -> 1) is not opened or closed either.
    const bool nowOpen = raw.data > 0;
    const bool wasOpen = before > 0;
    if (nowOpen == wasOpen) return;
    if (!known && !nowOpen) return; // First sight of a closed container: nothing happened that we saw.

    const auto now = std::chrono::steady_clock::now();
    auto& recent = nowOpen ? lastLocalOpen : lastLocalClose;
    const auto local = recent.find(key);
    if (local != recent.end() && now - local->second < std::chrono::seconds(2)) return; // Already logged from your window.

    report(nowOpen ? Kind::Opened : Kind::Closed, { raw.x, raw.y, raw.z }, info.name, "block event (anyone nearby)");
}

void ContainerLogger::handleContainerOpen(SDK::Packet* pkt) {
    uint8_t windowId = 0;
    int8_t windowType = 0;
    int x = 0, y = 0, z = 0;
    int64_t entity = -1;
    if (!readRaw(pkt, kOpenWindowId, windowId) || !readRaw(pkt, kOpenWindowType, windowType) ||
        !readRaw(pkt, kOpenPos, x) || !readRaw(pkt, kOpenPos + 4, y) || !readRaw(pkt, kOpenPos + 8, z) ||
        !readRaw(pkt, kOpenEntity, entity))
        return;

    const char* typeName = windowTypeName(windowType);
    if (!typeName || entity != -1 || !plausiblePos(x, y, z)) {
        debugLine(std::format("container open ignored: window {} type {} entity {} pos {} {} {}", windowId,
                              static_cast<int>(windowType), entity, x, y, z));
        return;
    }

    // The window type only says "Container" for chests, barrels, shulker boxes... the block says which.
    std::string type = typeName;
    ContainerInfo info {};
    const std::string blockId = blockIdAt(x, y, z);
    if (!blockId.empty() && containerFromBlock(blockId, info)) type = info.name;

    if (windows.size() > 64) windows.clear();
    windows[windowId] = { { x, y, z }, type };

    const uint64_t key = keyOf(x, y, z);
    lastLocalOpen[key] = std::chrono::steady_clock::now();
    if (lastLocalOpen.size() > kMaxTracked) lastLocalOpen.clear();

    report(Kind::Opened, { x, y, z }, type, "your own container window");
}

void ContainerLogger::handleContainerClose(SDK::Packet* pkt) {
    uint8_t windowId = 0;
    if (!readRaw(pkt, kCloseWindowId, windowId)) return;

    // A close packet has no position: it is the one remembered from the open packet of the same window.
    const auto it = windows.find(windowId);
    if (it == windows.end()) {
        debugLine(std::format("container close for window {} ignored: no matching open", windowId));
        return;
    }
    const OpenWindow window = it->second;
    windows.erase(it);

    const uint64_t key = keyOf(window.pos.x, window.pos.y, window.pos.z);
    lastLocalClose[key] = std::chrono::steady_clock::now();
    if (lastLocalClose.size() > kMaxTracked) lastLocalClose.clear();
    lastContentsHash.erase(key);

    report(Kind::Closed, window.pos, window.type, "your own container window");
}

void ContainerLogger::handleBlockActorData(SDK::Packet* pkt) {
    if (!std::get<BoolValue>(logContents) && !std::get<BoolValue>(debugMode)) return;

    int x = 0, y = 0, z = 0;
    if (!readRaw(pkt, kPosOffset, x) || !readRaw(pkt, kPosOffset + 4, y) || !readRaw(pkt, kPosOffset + 8, z) ||
        !plausiblePos(x, y, z))
        return;

    auto* tag = reinterpret_cast<SDK::Tag*>(reinterpret_cast<char*>(pkt) + kBlockActorTag);
    if (!looksLikeGameObject(tag)) {
        debugLine("block actor data ignored: no tag where expected (layout wrong?)");
        return;
    }
    SDK::BasicPrintStream out;
    if (!printTag(tag, &out) || out.mStr.empty()) {
        debugLine("block actor data ignored: the tag couldn't be printed");
        return;
    }
    if (std::get<BoolValue>(debugMode)) {
        Logger::Info("[ContainerLogger] block actor data at {} {} {}:\n{}", x, y, z, out.mStr.substr(0, 1500));
    }
    if (!std::get<BoolValue>(logContents)) return;

    // Only container block entities, and only when they carry an item list: most updates (signs, banners...) or
    // containers whose server leaves the items out say nothing about contents.
    const char* name = containerFromBlockEntityId(blockEntityId(out.mStr));
    if (!name) return;
    const auto items = parseItems(out.mStr);
    if (items.empty()) return;

    reportContents({ x, y, z }, name, items);
}

void ContainerLogger::handleInventory(SDK::Packet* pkt, bool wholeContainer) {
    if (!std::get<BoolValue>(logContents)) return;

    uint8_t windowId = 0;
    if (!readRaw(pkt, kInventoryWindowId, windowId)) return;
    const auto it = windows.find(windowId);
    if (it == windows.end()) return; // The player's own inventory or a window this module didn't see open.

    // The items in these packets are network item descriptors (a numeric item id, not a name). Turning those into names
    // needs the game's item registry, so only the fact that contents arrived is reported, never made-up item names.
    const uint64_t key = keyOf(it->second.pos.x, it->second.pos.y, it->second.pos.z);
    const auto now = std::chrono::steady_clock::now();
    static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastNote;
    auto& last = lastNote[key];
    if (last.time_since_epoch().count() != 0 && now - last < std::chrono::seconds(2)) return;
    last = now;
    if (lastNote.size() > kMaxTracked) lastNote.clear();

    line(std::format("{} {} at X: {} Y: {} Z: {} (item names are not decoded from this packet)",
                     it->second.type, wholeContainer ? "contents received" : "data updated", it->second.pos.x,
                     it->second.pos.y, it->second.pos.z));
}

void ContainerLogger::report(Kind kind, Pos const& pos, std::string const& type, const char* source) {
    if (kind == Kind::Opened && !std::get<BoolValue>(logOpen)) return;
    if (kind == Kind::Closed && !std::get<BoolValue>(logClose)) return;

    const auto now = std::chrono::steady_clock::now();
    auto& reported = lastReport[kind == Kind::Opened ? 0 : 1];
    if (reported.size() > kMaxTracked) reported.clear();
    const uint64_t key = keyOf(pos.x, pos.y, pos.z);
    const auto previous = reported.find(key);
    if (previous != reported.end() && now - previous->second < std::chrono::milliseconds(1500)) return;
    reported[key] = now;

    const char* what = kind == Kind::Opened ? "opened" : "closed";
    if (std::get<BoolValue>(showCoords)) {
        line(std::format("Container {} at X: {} Y: {} Z: {}", what, pos.x, pos.y, pos.z));
    } else {
        line(std::format("Container {}", what));
    }
    if (std::get<BoolValue>(showType)) line(std::format("Type: {}", type));
    const std::string dimension = currentDimension();
    if (!dimension.empty()) line(std::format("Dimension: {}", dimension));
    if (std::get<BoolValue>(showTime)) line(std::format("Time: {}", timeNow()));
    if (std::get<BoolValue>(debugMode)) line(std::format("Source: {}", source));
    debugLine(std::format("{} {} at {} {} {} ({})", type, what, pos.x, pos.y, pos.z, source));
}

void ContainerLogger::reportContents(Pos const& pos, std::string const& type,
                                     std::vector<std::pair<std::string, int>> const& items) {
    // The same list again (the server repeats block entity data) is not new information.
    size_t hash = 0;
    for (auto const& [name, count] : items) {
        hash = hash * 1099511628211ull ^ std::hash<std::string> {}(name);
        hash = hash * 1099511628211ull ^ static_cast<size_t>(count);
    }
    const uint64_t key = keyOf(pos.x, pos.y, pos.z);
    if (lastContentsHash.size() > kMaxTracked) lastContentsHash.clear();
    auto found = lastContentsHash.find(key);
    if (found != lastContentsHash.end() && found->second == hash) return;
    lastContentsHash[key] = hash;

    line(std::format("{} Data at X: {} Y: {} Z: {}", type, pos.x, pos.y, pos.z));
    line("Items:");
    int shown = 0;
    for (auto const& [name, count] : items) {
        if (shown++ >= kMaxItemLines) {
            line(std::format("... and {} more", items.size() - kMaxItemLines));
            break;
        }
        line(std::format("- {} x{}", name, count));
    }
}
