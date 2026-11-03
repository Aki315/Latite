#pragma once
#include "../../Module.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SDK {
    class Packet;
}

// Writes a line in your chat whenever the server tells your client that a container was opened or closed, with the
// block position, so you can see where containers are used.
//
// What the server actually sends (Bedrock protocol, packet ids from the official docs):
//   * ContainerOpenPacket (46) / ContainerClosePacket (47): clientbound, but they only describe the container window
//     of THIS client. They never describe another player's container.
//   * BlockEventPacket (26): the animation of a block. For chests, trapped chests, ender chests and shulker boxes the
//     event type is 1 and the event data is how many players have it open (0 = closed). The server sends it to every
//     client that has the chunk loaded, so this is the one packet that shows OTHER players using a container.
//   * BlockActorDataPacket (56): the block entity's NBT. Only some servers send an "Items" list in it.
//   * InventoryContentPacket (49) / InventorySlotPacket (50): the items of the window you have open (yours only).
//
// What cannot be seen: containers that don't animate (hoppers, furnaces, brewing stands, dispensers...) send nothing to
// other players when someone opens them, and the server doesn't send block events outside the chunks you have loaded,
// so a container beyond your render distance is invisible to this module. Nothing here guesses positions.
class ContainerLogger final : public Module {
public:
    ContainerLogger();
    ~ContainerLogger() override = default;

    void onPacketReceive(Event& event);
    void onLeaveGame(Event& event);
    void onEnable() override;

private:
    struct Pos {
        int x, y, z;
    };

    // Where a window you opened is, until it is closed (a close packet has no position, only the window id).
    struct OpenWindow {
        Pos pos;
        std::string type;
    };

    struct BlockState {
        int openCount = 0; // From the last block event for this block.
    };

    enum class Kind { Opened, Closed };

    void reset();
    void handleBlockEvent(SDK::Packet* pkt);
    void handleContainerOpen(SDK::Packet* pkt);
    void handleContainerClose(SDK::Packet* pkt);
    void handleBlockActorData(SDK::Packet* pkt);
    void handleInventory(SDK::Packet* pkt, bool wholeContainer);
    void debugDump(SDK::Packet* pkt, uint8_t id);

    void report(Kind kind, Pos const& pos, std::string const& type, const char* source);
    void reportContents(Pos const& pos, std::string const& type, std::vector<std::pair<std::string, int>> const& items);
    void line(std::string const& text);
    void debugLine(std::string const& text);

    ValueType logOpen = BoolValue(true);
    ValueType logClose = BoolValue(true);
    ValueType logContents = BoolValue(false);
    ValueType showType = BoolValue(true);
    ValueType showCoords = BoolValue(true);
    ValueType showTime = BoolValue(true);
    ValueType debugMode = BoolValue(false);

    // Both maps are cleared when you leave, change dimension or switch the module on, and are capped.
    std::unordered_map<uint64_t, BlockState> blocks;
    std::unordered_map<int, OpenWindow> windows;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastLocalOpen;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastLocalClose;
    std::unordered_map<uint64_t, size_t> lastContentsHash;
    // When each container was last reported as opened [0] / closed [1], whatever packet it came from: your own window
    // and the block event for the same container arrive together and must give one line, not two.
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastReport[2];
    std::unordered_map<int, int> dumped; // Packet id -> how many raw dumps were written (Debug Mode).
};
