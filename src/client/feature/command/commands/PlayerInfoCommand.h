#pragma once
#include "../Command.h"

// .playerinfo [name|dump|flags]: prints every player in the player list with their platform, whether they are the host, their
// actor unique id and their skin id, one line each. With a name (part of it is enough) only matching players are
// listed. ".playerinfo dump" writes the raw bytes of every player list entry to the log, to find where a field
// moved if a value looks wrong (the 1.26.60 positions of the platform and host flag were found this way). ".playerinfo flags" prints the flag bytes (offsets 0xC0 to 0xC7) of every player,
// to see which byte tells the server's host apart.
class PlayerInfoCommand final : public Command {
public:
    PlayerInfoCommand();
    ~PlayerInfoCommand() = default;

    bool execute(std::string const label, std::vector<std::string> args) override;
};
