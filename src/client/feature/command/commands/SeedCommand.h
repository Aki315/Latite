#pragma once
#include "../Command.h"

#include <array>
#include <cstdint>
#include <mutex>

// .seed: prints the world seed in chat and copies it to the clipboard.
//
// The client has no seed accessor, so the seed is read from the StartGame packet the server sends when you join a world
// (it carries the level settings, seed first). That packet only arrives when you join, so after injecting you need to
// rejoin the world/server once before .seed knows the seed. A server can send 0 or a fake seed to hide the real one;
// that is shown as it is.
//
// ".seed dump" writes the start of the packet's memory to the log (and says what was found), to check the reading when
// the seed looks wrong.
class SeedCommand final : public Command {
public:
    SeedCommand();
    ~SeedCommand() = default;

    bool execute(std::string const label, std::vector<std::string> args) override;

private:
    void onPacketReceive(Event& ev);

    static constexpr size_t kCaptureBytes = 0x200;

    std::mutex mutex;
    bool captured = false;
    bool found = false;     // The layout check passed and the seed was read.
    int64_t seed = 0;
    size_t seedOffset = 0;  // Where in the packet the seed was.
    std::array<uint8_t, kCaptureBytes> raw {}; // The start of the packet, for ".seed dump".
};
