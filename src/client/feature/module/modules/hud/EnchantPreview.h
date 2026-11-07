#pragma once
#include "../../Module.h"
#include "client/event/events/RenderOverlayEvent.h"

#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Shows the full enchantments the enchanting table is offering.
//
// When you put an item in an enchanting table the server sends the game the three options (PlayerEnchantOptions, packet 146),
// and each option carries the complete list of enchantments with their levels. The table screen only shows a hint of the
// first one. This module reads the packet and lists everything: "Level 30: Protection IV, Unbreaking III".
//
// The options are what the server rolled for the item and bookshelves you have right now, not a prediction. The layout of the
// packet in memory is found by looking at it (see parseOptions), and Debug Mode writes what it looked like to the log.
class EnchantPreview final : public Module {
public:
    EnchantPreview();
    ~EnchantPreview() override = default;

    void onPacketReceive(Event& event);
    void onLeaveGame(Event& event);
    void onRenderOverlay(Event& event);

private:
    struct Option {
        int cost = 0;
        // Enchantment id and level, in the order the server listed them (the three activation lists joined, no repeats).
        std::vector<std::pair<int, int>> enchants;
    };

    ValueType textSize = FloatValue(22.f);
    ValueType posX = FloatValue(2.f);  // Left edge, in percent of the screen width.
    ValueType posY = FloatValue(25.f); // Top edge, in percent of the screen height.
    ValueType showBackground = BoolValue(true);
    ValueType onlyInScreens = BoolValue(true); // Hide while you are playing (cursor grabbed), show in the table screen.
    ValueType debugMode = BoolValue(false);

    std::mutex mutex;
    std::vector<Option> options;
    std::chrono::steady_clock::time_point received {};
};
