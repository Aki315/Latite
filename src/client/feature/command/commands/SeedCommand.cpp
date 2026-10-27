#include "pch.h"
#include "SeedCommand.h"

#include "client/Latite.h"
#include "client/event/events/PacketReceiveEvent.h"
#include "mc/common/network/Packet.h"
#include "util/Util.h"

#include <cmath>
#include <cstring>
#include <format>

namespace {
    // StartGamePacket: protocol packet id 11 (0x0B).
    constexpr auto kStartGamePacketId = static_cast<SDK::PacketID>(0x0B);

    // The packet's memory begins with the Packet base class (0x30 bytes). The StartGame fields that follow, in the
    // order the protocol sends them: entity unique id (8), entity runtime id (8), game mode (4), position (3 floats),
    // rotation (2 floats), then the level settings, which start with the seed (8).
    //
    // This layout is NOT verified in a running game, so nothing here trusts a fixed offset: it looks for the place in
    // the packet where those fields line up (ids that are small, a game mode of 0 to 6, a position and rotation that
    // are sane numbers), and reads the seed from 0x28 bytes after it. If nothing lines up it reports that instead of
    // a made-up number.
    constexpr size_t kScanStart = 0x30;
    constexpr size_t kScanEnd = 0x1C0;
    constexpr size_t kSeedAfterIds = 0x28;

    bool sane(float v, float limit) { return std::isfinite(v) && std::fabs(v) <= limit; }

    // Copies bytes out of the packet. An access violation returns false instead of crashing. No objects with
    // destructors in here, which __try requires.
    bool copyBytes(const void* from, uint8_t* to, size_t count) {
        __try {
            std::memcpy(to, from, count);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool findSeed(const uint8_t* bytes, size_t size, int64_t& seed, size_t& at) {
        for (size_t o = kScanStart; o + kSeedAfterIds + 8 <= size && o <= kScanEnd; o += 8) {
            uint64_t uniqueId, runtimeId;
            int32_t mode;
            float f[5];
            std::memcpy(&uniqueId, bytes + o, 8);
            std::memcpy(&runtimeId, bytes + o + 8, 8);
            std::memcpy(&mode, bytes + o + 0x10, 4);
            std::memcpy(f, bytes + o + 0x14, 20);

            if (uniqueId == 0 || runtimeId == 0) continue;
            if (uniqueId > (1ull << 48) && uniqueId < (0ull - (1ull << 48))) continue; // Not a plausible id (signed).
            if (runtimeId > (1ull << 48)) continue;
            if (mode < 0 || mode > 6) continue;
            if (!sane(f[0], 3.0e7f) || !sane(f[1], 4096.f) || !sane(f[2], 3.0e7f)) continue;
            if (!sane(f[3], 360.f) || !sane(f[4], 360.f)) continue;
            if (f[0] == 0.f && f[1] == 0.f && f[2] == 0.f) continue;

            std::memcpy(&seed, bytes + o + kSeedAfterIds, 8);
            at = o + kSeedAfterIds;
            return true;
        }
        return false;
    }
} // namespace

SeedCommand::SeedCommand()
    : Command("seed", L"Prints the world seed and copies it to the clipboard", "[dump]") {
    // Called while it isn't running a command too: the packet comes when you join, long before .seed is typed.
    Eventing::get().listen<PacketReceiveEvent>(this, (EventListenerFunc)&SeedCommand::onPacketReceive, 0, true);
}

void SeedCommand::onPacketReceive(Event& evG) {
    auto& ev = reinterpret_cast<PacketReceiveEvent&>(evG);
    auto* pkt = ev.getPacket();
    if (!pkt || pkt->getID() != kStartGamePacketId) return;

    std::array<uint8_t, kCaptureBytes> bytes {};
    if (!copyBytes(pkt, bytes.data(), bytes.size())) return;

    int64_t value = 0;
    size_t at = 0;
    const bool ok = findSeed(bytes.data(), bytes.size(), value, at);

    std::lock_guard lock(mutex);
    raw = bytes;
    captured = true;
    found = ok;
    seed = value;
    seedOffset = at;
    Logger::Info("[Seed] StartGame packet seen: {}", ok ? std::format("seed {} (at offset 0x{:X})", value, at) : "layout not recognised");
}

bool SeedCommand::execute(std::string const label, std::vector<std::string> args) {
    std::lock_guard lock(mutex);

    if (!args.empty() && util::ToLower(args[0]) == "dump") {
        if (!captured) {
            message(L"No StartGame packet seen yet. Rejoin the world or server, then try again.", true);
            return true;
        }
        for (size_t row = 0; row < raw.size(); row += 16) {
            std::string line = std::format("+0x{:03X}:", row);
            for (size_t i = row; i < row + 16; ++i) line += std::format(" {:02X}", raw[i]);
            Logger::Info("[Seed] {}", line);
        }
        message(std::format("Wrote the packet's first {} bytes to the log. {}", raw.size(),
                            found ? std::format("Seed read from offset 0x{:X}.", seedOffset)
                                  : std::string("The seed could not be located in it.")));
        return true;
    }

    if (!captured) {
        message(L"The seed isn't known yet: the server sends it when you join. Rejoin the world or server (after "
                L"injecting), then use .seed again.", true);
        return true;
    }
    if (!found) {
        message(L"Got the join packet but couldn't find the seed in it. Use .seed dump and send me the log.", true);
        return true;
    }

    const std::string text = std::to_string(seed);
    util::SetClipboardText(util::StrToWStr(text));
    message(std::format("Seed: {} (copied to clipboard){}", text,
                        seed == 0 ? " - 0 usually means the server hides its seed" : ""));
    return true;
}
