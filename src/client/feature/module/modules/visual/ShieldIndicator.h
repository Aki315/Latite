#pragma once
#include "../../Module.h"

#include "util/ShieldState.h"

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

// Colors other players' shields in the world: green while the shield is raised, red while an axe has it disabled.
//
// Made for servers that run Blurry's Java PvP addon. That addon keeps the shield's state on the server and never sends
// it to clients, so this works from the two things that do reach you (see util/ShieldState.h):
//   * a raised shield shows as the "using item" status flag on the player, and
//   * when an axe breaks a shield the addon plays "random.break" at pitch 1.2 at that player. The red state runs for
//     the addon's cooldown (5 seconds by default; set Shield Disabled Time to the server's value).
//
// The colored panel is an overlay drawn in front of the shield, where the shield sits in the raised pose. It is placed
// from the player's body direction, not from the shield model itself (the client doesn't expose that), so the
// Size and Offset settings are there to line it up.
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

    ValueType ownThirdPerson = BoolValue(true);  // Also color your own shield while you are in third person.
    ValueType maxDistance = FloatValue(32.f);    // Players further away than this aren't drawn.
    ValueType cooldown = FloatValue(5.f);        // How long a broken shield stays red (the addon's default is 5).
    ValueType showReady = BoolValue(false);      // Also color a held, lowered shield (not only a raised one).
    ValueType blockColor = ColorValue(0.2f, 0.9f, 0.35f, 0.5f);
    ValueType brokenColor = ColorValue(0.95f, 0.2f, 0.2f, 0.55f);
    ValueType size = FloatValue(1.f);
    ValueType offsetForward = FloatValue(0.f);   // In blocks, to line the panel up with the shield.
    ValueType offsetSide = FloatValue(0.f);
    ValueType offsetUp = FloatValue(0.f);
    ValueType debugLog = BoolValue(false);       // Log the break sounds it sees and who they were matched to.

    ShieldState::Tracker tracker;

    std::mutex soundMutex;
    std::vector<BreakSound> pendingSounds; // Filled by the packet event, emptied each frame.
};
