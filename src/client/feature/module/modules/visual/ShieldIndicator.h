#pragma once
#include "../../Module.h"

#include "util/ShieldState.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Switches the texture of other players' Blurry's Java PvP shield (bf:shield): the normal texture, a green one while
// the shield is raised and a red one while an axe has it disabled.
//
// The texture is changed on the real shield model by the game itself, not drawn or tinted over it. This module only
// works out each player's shield state and tells the resource pack through the player's status flags: a raised shield
// is already visible to the pack as query.is_using_item, and a disabled one is marked by setting the "saddled" flag
// (query.is_saddled). The resource pack's controller.render.bf_shield, the render controller of the bf:shield
// attachable (and of nothing else), picks the texture from those two. Without that resource pack this module changes
// nothing you can see.
//
// Blurry's addon keeps a shield's state on the server and never sends it to clients, so it is worked out from the two
// things that do reach you (see util/ShieldState.h):
//   * a raised shield shows as the "using item" status flag on the player, and
//   * when an axe breaks a shield the addon plays "random.break" at pitch 1.2 at that player. The disabled state runs
//     for the addon's cooldown (5 seconds by default; set Shield Disabled Time to the server's value).
class ShieldIndicator final : public Module {
public:
    ShieldIndicator();
    ~ShieldIndicator() override = default;

    void onRenderLevel(RenderLevelEvent& event);
    void onPacketReceive(Event& event);
    void onDisable() override;

private:
    // A break sound as it came in, waiting for the next frame to be matched to a player.
    struct BreakSound {
        std::string name;
        float pitch;
        int x, y, z; // The packet's position field, raw.
        std::chrono::steady_clock::time_point when;
    };

    ValueType includeSelf = BoolValue(true);     // Also switch the texture of your own shield.
    ValueType cooldown = FloatValue(5.f);        // How long a disabled shield stays disabled (the addon's default is 5).
    ValueType debugLog = BoolValue(false);       // Log the break sounds it sees and who they were matched to.

    ShieldState::Tracker tracker;
    std::unordered_map<uint64_t, int> published; // Runtime id -> the state last worked out for that player.
    bool announced = false;

    std::mutex soundMutex;
    std::vector<BreakSound> pendingSounds; // Filled by the packet event, emptied each frame.
};
