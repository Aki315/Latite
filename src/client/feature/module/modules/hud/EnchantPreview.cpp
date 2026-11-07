#include "pch.h"
#include "EnchantPreview.h"

#include "client/Latite.h"
#include "client/event/events/LeaveGameEvent.h"
#include "client/event/events/PacketReceiveEvent.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/game/MinecraftGame.h"
#include "mc/common/network/Packet.h"
#include "util/DrawContext.h"
#include "util/Util.h"
#include "util/VaultNbt.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace {
    constexpr uint8_t kPlayerEnchantOptions = 146;
    constexpr size_t kPacketBase = 0x30; // the Packet base class takes 0x30 bytes, the fields follow

    template<typename T>
    bool readRaw(const void* base, size_t offset, T& out) {
        __try {
            std::memcpy(&out, static_cast<const char*>(base) + offset, sizeof(T));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool copyBytes(const void* from, void* to, size_t count) {
        __try {
            std::memcpy(to, from, count);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // A std::vector's three pointers.
    struct VecHeader {
        uintptr_t begin = 0, end = 0, cap = 0;
    };

    bool readVec(const void* base, size_t offset, VecHeader& v) {
        return readRaw(base, offset, v.begin) && readRaw(base, offset + 8, v.end) && readRaw(base, offset + 16, v.cap);
    }

    // A list of enchantments: 8 bytes each (the enchantment id, then the level). `entries` gets the ones that look right.
    // Returns false when this isn't such a list. An empty list is fine.
    bool readEnchantList(VecHeader const& v, std::vector<std::pair<int, int>>& entries) {
        if (v.begin == 0 && v.end == 0) return true;
        if (!v.begin || v.end < v.begin || v.cap < v.end || ((v.end - v.begin) & 7) != 0) return false;
        const size_t count = (v.end - v.begin) / 8;
        if (count > 12) return false;
        for (size_t i = 0; i < count; ++i) {
            int32_t type = 0, level = 0;
            if (!readRaw(reinterpret_cast<const void*>(v.begin), i * 8, type) || !readRaw(reinterpret_cast<const void*>(v.begin), i * 8 + 4, level))
                return false;
            const int id = type & 0xFF;
            if (id > 63 || level < 1 || level > 20) return false;
            entries.emplace_back(id, level);
        }
        return true;
    }

    // One option, whatever its exact layout: the three enchantment lists sit one after another (three vectors, 24 bytes
    // each) somewhere in the first 0x60 bytes. The first place where three vectors in a row look like enchantment lists
    // (and at least one has something in it) is taken; the cost is the first small number in the first 8 bytes.
    bool parseOneOption(const char* elem, int& cost, std::vector<std::pair<int, int>>& enchants, size_t& listsAt) {
        char buf[0x80];
        if (!copyBytes(elem, buf, sizeof(buf))) return false;
        for (size_t base = 0x08; base <= 0x38; base += 8) {
            std::vector<std::pair<int, int>> all;
            bool ok = true;
            size_t nonEmpty = 0;
            for (int list = 0; list < 3 && ok; ++list) {
                VecHeader v;
                std::memcpy(&v.begin, buf + base + list * 24, 8);
                std::memcpy(&v.end, buf + base + list * 24 + 8, 8);
                std::memcpy(&v.cap, buf + base + list * 24 + 16, 8);
                std::vector<std::pair<int, int>> one;
                ok = readEnchantList(v, one);
                if (ok && !one.empty()) {
                    ++nonEmpty;
                    for (auto const& e : one)
                        if (std::find(all.begin(), all.end(), e) == all.end()) all.push_back(e);
                }
            }
            if (!ok || nonEmpty == 0) continue;
            int32_t first = 0, second = 0;
            std::memcpy(&first, buf, 4);
            std::memcpy(&second, buf + 4, 4);
            cost = first >= 1 && first <= 60 ? first : (second >= 1 && second <= 60 ? second : 0);
            enchants = std::move(all);
            listsAt = base;
            return true;
        }
        return false;
    }

    // Finds the vector of options in the packet and reads every option. `layout` says what was found (for the log).
    bool parseOptions(SDK::Packet* pkt, std::vector<std::pair<int, std::vector<std::pair<int, int>>>>& out, std::string& layout) {
        // The vector of options is the first field after the Packet base class; look a little further too.
        const size_t sizes[] = { 0x80, 0x78, 0x88, 0x70, 0x90, 0x68, 0x98, 0xA0, 0x60, 0xA8, 0xB0, 0x58, 0x50, 0xC0, 0x100 };
        for (size_t off = kPacketBase; off <= kPacketBase + 0x30; off += 8) {
            VecHeader v;
            if (!readVec(pkt, off, v) || !v.begin || v.end <= v.begin || v.cap < v.end) continue;
            const size_t bytes = v.end - v.begin;
            for (size_t size : sizes) {
                if (bytes % size != 0) continue;
                const size_t count = bytes / size;
                if (count < 1 || count > 6) continue;
                std::vector<std::pair<int, std::vector<std::pair<int, int>>>> parsed;
                size_t listsAt = 0;
                bool ok = true;
                for (size_t i = 0; i < count && ok; ++i) {
                    int cost = 0;
                    std::vector<std::pair<int, int>> enchants;
                    ok = parseOneOption(reinterpret_cast<const char*>(v.begin) + i * size, cost, enchants, listsAt);
                    if (ok) parsed.emplace_back(cost, std::move(enchants));
                }
                if (!ok) continue;
                out = std::move(parsed);
                layout = std::format("options vector at packet +0x{:X}, {} option(s) of 0x{:X} bytes, enchantment lists at element +0x{:X}", off,
                                     count, size, listsAt);
                return true;
            }
        }
        return false;
    }
} // namespace

EnchantPreview::EnchantPreview()
    : Module("EnchantPreview", LocalizeString::get("client.module.enchantPreview.name"),
             LocalizeString::get("client.module.enchantPreview.desc"), GAME) {
    addSliderSetting("textSize", LocalizeString::get("client.module.enchantPreview.textSize.name"),
                     LocalizeString::get("client.module.enchantPreview.textSize.desc"), textSize, FloatValue(10.f),
                     FloatValue(60.f), FloatValue(1.f));
    addSliderSetting("posX", LocalizeString::get("client.module.enchantPreview.posX.name"),
                     LocalizeString::get("client.module.enchantPreview.posX.desc"), posX, FloatValue(0.f), FloatValue(95.f),
                     FloatValue(0.5f));
    addSliderSetting("posY", LocalizeString::get("client.module.enchantPreview.posY.name"),
                     LocalizeString::get("client.module.enchantPreview.posY.desc"), posY, FloatValue(0.f), FloatValue(95.f),
                     FloatValue(0.5f));
    addSetting("showBackground", LocalizeString::get("client.module.enchantPreview.showBackground.name"),
               LocalizeString::get("client.module.enchantPreview.showBackground.desc"), showBackground);
    addSetting("onlyInScreens", LocalizeString::get("client.module.enchantPreview.onlyInScreens.name"),
               LocalizeString::get("client.module.enchantPreview.onlyInScreens.desc"), onlyInScreens);
    addSetting("debugMode", LocalizeString::get("client.module.enchantPreview.debugMode.name"),
               LocalizeString::get("client.module.enchantPreview.debugMode.desc"), debugMode);

    // The options come when the item is put in the table, so the packets are read even while the module is off.
    listen<PacketReceiveEvent>((EventListenerFunc)&EnchantPreview::onPacketReceive, true);
    listen<LeaveGameEvent>((EventListenerFunc)&EnchantPreview::onLeaveGame, true);
    listen<RenderOverlayEvent>((EventListenerFunc)&EnchantPreview::onRenderOverlay);
}

void EnchantPreview::onLeaveGame(Event&) {
    std::lock_guard lock(mutex);
    options.clear();
}

void EnchantPreview::onPacketReceive(Event& evG) {
    auto& ev = reinterpret_cast<PacketReceiveEvent&>(evG);
    SDK::Packet* pkt = ev.getPacket();
    if (!pkt || static_cast<uint8_t>(pkt->getID()) != kPlayerEnchantOptions) return;

    const bool debug = isEnabled() && std::get<BoolValue>(debugMode);
    if (debug) {
        // What the packet looks like in memory, to find the layout if it isn't read right.
        for (size_t row = 0; row < 0x70; row += 16) {
            std::string line = std::format("+0x{:03X}:", row);
            uint8_t bytes[16] = {};
            if (!copyBytes(static_cast<const char*>(static_cast<const void*>(pkt)) + row, bytes, 16)) break;
            for (uint8_t b : bytes) line += std::format(" {:02X}", b);
            Logger::Info("[EnchantPreview] packet {}", line);
        }
    }

    std::vector<std::pair<int, std::vector<std::pair<int, int>>>> parsed;
    std::string layout;
    VecHeader v;
    const bool emptyVector = readVec(pkt, kPacketBase, v) && v.begin == v.end;
    if (!parseOptions(pkt, parsed, layout)) {
        if (!emptyVector) {
            if (debug) Logger::Info("[EnchantPreview] enchant options packet seen, but its layout was not recognised");
            return; // keep showing what was there before
        }
        std::lock_guard lock(mutex); // an empty list: the item was taken out of the table
        options.clear();
        return;
    }

    std::vector<Option> list;
    for (auto& [cost, enchants] : parsed) {
        Option o;
        o.cost = cost;
        o.enchants = std::move(enchants);
        list.push_back(std::move(o));
    }
    if (debug) {
        Logger::Info("[EnchantPreview] {}", layout);
        for (auto const& o : list) {
            std::string text;
            for (auto const& [id, level] : o.enchants)
                text += std::format("{}{} {}", text.empty() ? "" : ", ", vault_nbt::enchantName(id), vault_nbt::roman(level));
            Logger::Info("[EnchantPreview] option level {}: {}", o.cost, text);
        }
    }
    std::lock_guard lock(mutex);
    options = std::move(list);
    received = std::chrono::steady_clock::now();
}

void EnchantPreview::onRenderOverlay(Event&) {
    std::vector<Option> shown;
    {
        std::lock_guard lock(mutex);
        if (options.empty()) return;
        // An old table (left more than ten minutes ago) isn't shown.
        if (std::chrono::steady_clock::now() - received > std::chrono::minutes(10)) {
            options.clear();
            return;
        }
        shown = options;
    }

    auto* ci = SDK::ClientInstance::get();
    if (!ci || !ci->minecraftGame || !ci->getLocalPlayer()) return;
    if (std::get<BoolValue>(onlyInScreens) && ci->minecraftGame->isCursorGrabbed()) return; // playing, not in a screen

    D2DUtil dc;
    const D2D1_SIZE_F ss = Latite::getRenderer().getScreenSize();
    const float ts = std::clamp(static_cast<float>(std::get<FloatValue>(textSize)), 10.f, 60.f);
    const auto font = Renderer::FontSelection::SecondaryLight;
    const float lineH = std::max(dc.getTextSize(L"Ag", font, ts, false).y, 8.f);
    const float pad = ts * 0.5f;

    struct Row {
        std::wstring text;
        d2d::Color color;
    };
    std::vector<Row> rows;
    rows.push_back({ L"Enchantments offered", d2d::Color::RGB(170, 170, 170) });
    for (auto const& o : shown) {
        rows.push_back({ std::format(L"Level {}", o.cost), d2d::Color::RGB(120, 255, 120) });
        if (o.enchants.empty()) rows.push_back({ L"  (none)", d2d::Colors::WHITE });
        for (auto const& [id, level] : o.enchants)
            rows.push_back({ L"  " + util::StrToWStr(vault_nbt::enchantName(id) + " " + vault_nbt::roman(level)), d2d::Color::RGB(255, 170, 255) });
    }

    float w = 0.f;
    for (auto const& r : rows) w = std::max(w, dc.getTextSize(r.text, font, ts, false).x);
    const float h = lineH * static_cast<float>(rows.size());
    const float left = ss.width * std::clamp(static_cast<float>(std::get<FloatValue>(posX)), 0.f, 95.f) / 100.f;
    const float top = ss.height * std::clamp(static_cast<float>(std::get<FloatValue>(posY)), 0.f, 95.f) / 100.f;

    if (std::get<BoolValue>(showBackground)) {
        dc.fillRoundedRectangle(d2d::Rect(left, top, left + w + pad * 2.f, top + h + pad * 2.f), d2d::Color(0.f, 0.f, 0.f, 0.55f),
                                ts * 0.3f);
    }
    float y = top + pad;
    for (auto const& r : rows) {
        dc.drawText(d2d::Rect(left + pad, y, left + pad + w + 4.f, y + lineH), r.text, r.color, font, ts, DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        y += lineH;
    }
}
