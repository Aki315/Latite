#pragma once

#include "client/feature/module/Module.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

// A debugging aid for working out how another client (or the server) shows other players' armor durability.
//
// It writes what the game itself exposes to a text file: every nearby player's raw nametag, their armor items with
// the durability this client was told about, their health, the armor and hurt related packets that arrive, and the
// hits you land. Put it next to a client that already shows armor durability on nametags, turn Latite's own Armor
// Nametag off, and compare the file with what the other client shows. Nothing is changed in the game.
//
// File: the Latite folder, Logs, ArmorDebug.log (cleared every time the module is turned on).
class ArmorDebug final : public Module {
public:
    ArmorDebug();
    ~ArmorDebug() override = default;

    void onUpdate(Event& event);
    void onPacketReceive(Event& event);
    void onAttack(Event& event);
    void onEnable() override;
    void onDisable() override;

private:
    void write(std::string const& line);

    ValueType logDistance = FloatValue(24.f);  // Blocks; 0 = every player the game has loaded.
    ValueType interval = FloatValue(5.f);      // Ticks between player checks. Lines only appear when something changed.
    ValueType logPackets = BoolValue(true);    // Armor, equipment, attribute and hurt packets.
    ValueType dumpPackets = BoolValue(false);  // Also the first bytes of each armor packet, for working out its layout.

    std::mutex fileMutex;
    std::ofstream file;
    std::chrono::steady_clock::time_point started {};

    uint64_t tick = 0;
    std::unordered_map<uint64_t, std::string> lastState; // Per player, what was last written.
    std::unordered_map<uint64_t, float> lastHealth;
    std::unordered_map<uint8_t, int> packetCounts;       // How many of each packet id arrived (printed when turned off).
};
