#include "pch.h"
#include "PlayerInfoCommand.h"

#include "client/Latite.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/world/Minecraft.h"
#include "mc/common/world/level/Level.h"
#include "mc/common/world/actor/player/PlayerListEntry.h"
#include "util/Util.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

namespace {
    // BuildPlatform values as the protocol numbers them.
    std::string platformName(int32_t value) {
        switch (value) {
        case -1: return "Unknown";
        case 1: return "Android";
        case 2: return "iOS";
        case 3: return "macOS";
        case 4: return "Amazon";
        case 5: return "Gear VR";
        case 6: return "HoloLens";
        case 7: return "Win10";
        case 8: return "Win32";
        case 9: return "Dedicated";
        case 10: return "tvOS";
        case 11: return "PlayStation";
        case 12: return "Switch";
        case 13: return "Xbox";
        case 14: return "Windows Phone";
        default: return std::format("Unknown ({})", value);
        }
    }

    struct Row {
        std::string name;
        std::string platform;
        bool host;
        int64_t uniqueId;
        std::string skinId;
    };

    void print(std::string const& line) {
        Latite::get().getClientMessageQueue().display(util::WFormat(util::StrToWStr(line)));
    }
}

PlayerInfoCommand::PlayerInfoCommand()
    : Command("playerinfo", L"Lists the players with their platform, host flag, actor unique id and skin id",
              "[name|dump|flags]", { "pinfo" }) {}

bool PlayerInfoCommand::execute(std::string const label, std::vector<std::string> args) {
    auto* ci = SDK::ClientInstance::get();
    auto* level = ci && ci->minecraft ? ci->minecraft->getLevel() : nullptr;
    auto* players = level ? level->getPlayerList() : nullptr;
    if (!players || players->empty()) {
        message(L"No player list yet. Join a world first.", true);
        return true;
    }

    if (!args.empty() && util::ToLower(args[0]) == "dump") {
        auto& first = players->begin()->second;
        const auto* bytes = reinterpret_cast<const uint8_t*>(&first);
        for (size_t row = 0; row < 0xA8; row += 16) {
            std::string line = std::format("+0x{:03X}:", row);
            for (size_t i = row; i < std::min<size_t>(row + 16, 0xA8); ++i) line += std::format(" {:02X}", bytes[i]);
            Logger::Info("[PlayerInfo] {}", line);
        }
        const std::string firstName = first.name;
        message(std::format("Wrote the first player's entry ({}) to the log.", firstName));
        return true;
    }

    if (!args.empty() && util::ToLower(args[0]) == "flags") {
        for (auto& [uuid, entry] : *players) {
            const std::string name = entry.name;
            const auto* bytes = reinterpret_cast<const uint8_t*>(&entry);
            std::string line = std::format("&a{}&r:", name);
            for (size_t i = 0xA0; i < 0xA8; ++i) line += std::format(" {:02X}", bytes[i]);
            print(line);
        }
        return true;
    }

    const std::string filter = args.empty() ? std::string() : util::ToLower(args[0]);

    std::vector<Row> rows;
    rows.reserve(players->size());
    for (auto& [uuid, entry] : *players) {
        std::string name = entry.name;
        if (!filter.empty() && util::ToLower(name).find(filter) == std::string::npos) continue;

        std::string skin;
        if (auto const* id = entry.skin.getId()) skin = *id;
        rows.push_back({ std::move(name), platformName(entry.buildPlatform), entry.isHost, entry.uniqueId,
                         std::move(skin) });
    }
    if (rows.empty()) {
        message(L"No player matches that name.", true);
        return true;
    }
    std::sort(rows.begin(), rows.end(), [](Row const& l, Row const& r) { return util::ToLower(l.name) < util::ToLower(r.name); });

    for (auto const& row : rows) {
        print(std::format("&aName: {}&r, &dPlatform: {}&r, &9Is Host: {}&r, &eActor Unique ID: {}&r, &bSkin Id: {}",
                          row.name, row.platform, row.host ? "Yes" : "No", row.uniqueId, row.skinId));
    }
    return true;
}
