#pragma once
#include "../Command.h"

// .playerinfo [name|dump|flags]: prints every player in the player list with their platform, whether they are the host, their
// actor unique id and their skin id, one line each. With a name (part of it is enough) only matching players are
// listed. ".playerinfo dump" writes the raw bytes of the first player list entry to the log, to check the field
// positions if a value looks wrong. ".playerinfo flags" prints the flag bytes (offsets 0xA0 to 0xA7) of every player,
// to see which byte tells the server's host apart.
class PlayerInfoCommand final : public Command {
public:
    PlayerInfoCommand();
    ~PlayerInfoCommand() = default;

    bool execute(std::string const label, std::vector<std::string> args) override;
};
