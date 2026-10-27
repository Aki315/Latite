#pragma once
#include "../Command.h"

// .nbt [offhand]: prints the NBT of the item in your hand in chat and copies it to the clipboard. With "offhand" it
// reads the item in your offhand instead. An item with no custom data (no enchantments, no name, ...) has no NBT.
class NbtCommand final : public Command {
public:
    NbtCommand();
    ~NbtCommand() = default;

    bool execute(std::string const label, std::vector<std::string> args) override;
};
