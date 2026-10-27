#pragma once
#include "../Command.h"

// .playerinfo [name|dump]: prints every player in the player list with their platform, whether they are the host, their
// actor unique id and their skin id, one line each. With a name (part of it is enough) only matching players are
// listed. ".playerinfo dump" writes the raw bytes of the first player list entry to the log, to check the field
// positions if a value looks wrong.
class PlayerInfoCommand final : public Command {
public:
    PlayerInfoCommand();
    ~PlayerInfoCommand() = default;

    bool execute(std::string const label, std::vector<std::string> args) override;
};
