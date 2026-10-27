#include "pch.h"
#include "NbtCommand.h"

#include "client/Latite.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/entity/component/ActorEquipmentComponent.h"
#include "mc/common/nbt/CompoundTag.h"
#include "mc/common/world/Item.h"
#include "mc/common/world/ItemStack.h"
#include "mc/common/world/actor/player/Inventory.h"
#include "mc/common/world/actor/player/Player.h"
#include "mc/common/world/actor/player/PlayerInventory.h"
#include "util/Util.h"

#include <format>
#include <string>
#include <vector>

namespace {
    // The game's own printing of a tag (the same call the test command uses), done in a function of its own so that an
    // access violation inside it is caught instead of crashing the game.
    void printTagInner(SDK::Tag* tag, SDK::BasicPrintStream* out) { tag->print("", *out); }

    bool printTag(SDK::Tag* tag, SDK::BasicPrintStream* out) {
        __try {
            printTagInner(tag, out);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::vector<std::string> splitLines(std::string const& text) {
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(std::move(line));
            start = end + 1;
        }
        return lines;
    }
}

NbtCommand::NbtCommand()
    : Command("nbt", L"Prints the NBT of the item in your hand and copies it to the clipboard", "[offhand]") {}

bool NbtCommand::execute(std::string const label, std::vector<std::string> args) {
    auto* ci = SDK::ClientInstance::get();
    auto* lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp) {
        message(L"Join a world first.", true);
        return true;
    }

    const bool offhand = !args.empty() && util::ToLower(args[0]) == "offhand";

    SDK::ItemStack* stack = nullptr;
    if (offhand) {
        auto* equipment = lp->tryGetComponent<SDK::ActorEquipmentComponent>();
        if (equipment && equipment->handContainer) stack = equipment->handContainer->getItem(1);
    } else if (lp->supplies && lp->supplies->inventory) {
        stack = lp->supplies->inventory->getItem(lp->supplies->selectedSlot);
    }

    auto* item = stack ? stack->getItem() : nullptr;
    if (!item) {
        message(offhand ? L"Nothing in your offhand." : L"Nothing in your hand.", true);
        return true;
    }

    const std::string id = item->namespacedId.getString();
    if (!stack->tag) {
        message(std::format("{} has no NBT (no enchantments, name or other custom data).", id));
        return true;
    }

    SDK::BasicPrintStream out;
    if (!printTag(stack->tag.get(), &out) || out.mStr.empty()) {
        message(L"Couldn't read the item's NBT.", true);
        return true;
    }

    const std::string text = id + "\n" + out.mStr;
    util::SetClipboardText(util::StrToWStr(text));

    message(std::format("NBT of {} (copied to clipboard):", id));
    for (auto const& line : splitLines(out.mStr)) {
        Latite::get().getClientMessageQueue().display(util::StrToWStr(line));
    }
    return true;
}
