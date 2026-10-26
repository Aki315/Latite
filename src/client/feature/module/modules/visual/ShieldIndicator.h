#pragma once
#include "../../Module.h"

#include "util/ShieldState.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace SDK {
    class Actor;
}

// Shows other players' shield state on the shield itself: green while it is raised and working, red while an axe has it
// disabled.
//
// Made for servers that run Blurry's Java PvP addon. That addon keeps the shield's state on the server and never sends
// it to clients, so this works from the two things that do reach you (see util/ShieldState.h):
//   * a raised shield shows as the "using item" status flag on the player, and
//   * when an axe breaks a shield the addon plays "random.break" at pitch 1.2 at that player. The red state runs for
//     the addon's cooldown (5 seconds by default; set Shield Disabled Time to the server's value).
//
// How it is shown, in order of preference:
//   1. Recolor the real shield. The game asks "what overlay color does this model get" once for every model it draws,
//      and the module answers red or green when that model is the shield of a player whose shield is disabled or raised.
//      That is the same thing a resource pack does when it swaps the shield's texture, only the module knows the
//      disabled state, which a resource pack cannot see. The shield's model is recognised by the name of its render
//      controller ("controller.render.bf_shield" in Blurry's pack), found by looking through the controller object for
//      its text (util/ObjectStrings.h). Shields that share the generic "controller.render.item_default" cannot be told
//      apart from other held items, so those are not recolored.
//   2. Draw an image over the shield, but only for a shield the recolor did not reach (Draw Image Instead, on by
//      default), so a shield is never left unmarked. It is a flat panel standing where the shield is in the raised
//      pose, placed from the player's body direction (the client doesn't expose the shield model's position), so the
//      Size and Offset settings are there to line it up. The images are assets/shield_on.png and assets/shield_off.png.
class ShieldIndicator final : public Module {
public:
    ShieldIndicator();
    ~ShieldIndicator() override = default;

    void onRenderLevel(RenderLevelEvent& event);
    void onRenderOverlay(Event& event);
    void onPacketReceive(Event& event);
    void onOverlayColor(Event& event);
    void onDisable() override;

private:
    // A break sound as it came in, waiting for the next frame to be matched to a player.
    struct BreakSound {
        std::string name;
        float pitch;
        int x, y, z; // The packet's position field, raw.
        std::chrono::steady_clock::time_point when;
    };

    ValueType tintShield = BoolValue(true);      // Recolor the real shield (red disabled, green raised).
    ValueType imageFallback = BoolValue(true);   // Draw the image over a shield the recolor did not reach.
    ValueType ownThirdPerson = BoolValue(true);  // Also color your own shield while you are in third person.
    ValueType maxDistance = FloatValue(32.f);    // Players further away than this aren't drawn.
    ValueType cooldown = FloatValue(5.f);        // How long a broken shield stays red (the addon's default is 5).
    ValueType markRaised = BoolValue(true);      // Mark raised shields green (off: only mark disabled ones, e.g. when a resource pack greens the real shield).
    ValueType showReady = BoolValue(false);      // Also mark a held, lowered shield (not only a raised one).
    ValueType opacity = FloatValue(0.9f);        // How solid the shield image is.
    ValueType size = FloatValue(1.f);
    ValueType offsetForward = FloatValue(0.f);   // In blocks, to line the panel up with the shield.
    ValueType offsetSide = FloatValue(0.f);
    ValueType offsetUp = FloatValue(0.f);
    ValueType debugLog = BoolValue(false);       // Log the break sounds it sees and who they were matched to.

    // A shield image to draw this frame: the four corners in the world (bottom left, bottom right, top right, top left as
    // seen from the front), and which image.
    struct Panel {
        Vec3 a, b, c, d;
        float distance;
        float alpha;
        bool broken;
        const SDK::Actor* actor; // Whose shield it is (only compared, never used).
    };
    std::mutex panelMutex;
    std::vector<Panel> panels; // Built in the world render, drawn in the overlay render.
    std::chrono::steady_clock::time_point panelsTime {};

    // Recoloring the real shield. The world render decides who gets which color each frame; the overlay color hook,
    // which runs while the game draws each model, reads that. All of it is under tintMutex.
    struct ControllerGuess {
        bool shield = false; // Is this render controller the shield's?
        std::chrono::steady_clock::time_point checked {};
    };
    std::mutex tintMutex;
    std::unordered_map<const SDK::Actor*, bool> tintTargets; // Actor -> shield disabled (red) or raised (green).
    std::unordered_map<const SDK::Actor*, std::chrono::steady_clock::time_point> tintedAt; // Last time one was recolored.
    std::unordered_map<const void*, ControllerGuess> controllers;
    std::atomic<bool> anyTint { false }; // Whether tintTargets has anything, so the hook can skip the lock when not.

    ShieldState::Tracker tracker;

    std::mutex soundMutex;
    std::vector<BreakSound> pendingSounds; // Filled by the packet event, emptied each frame.
};
