#include "pch.h"
#include "SeedCommand.h"

#include "client/Latite.h"
#include "client/event/events/PacketReceiveEvent.h"
#include "mc/common/network/Packet.h"
#include "util/Util.h"

#include <cstring>
#include <format>

namespace {
    // StartGamePacket: protocol packet id 11 (0x0B).
    constexpr auto kStartGamePacketId = static_cast<SDK::PacketID>(0x0B);

    // The packet's memory begins with the Packet base class (0x30 bytes), and the level settings follow, starting with
    // the seed (8 bytes). Checked in a running game: the seed sits at 0x30 (a .seed dump showed the server's seed
    // there, as a little-endian 64-bit number).
    constexpr size_t kSeedOffset = 0x30;

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
        if (size < kSeedOffset + 8) return false;
        std::memcpy(&seed, bytes + kSeedOffset, 8);
        at = kSeedOffset;
        return true;
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
