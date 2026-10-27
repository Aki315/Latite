#pragma once
#include "../../Module.h"
#include "client/event/events/OverlayColorEvent.h"

// Recolors the flash an entity gets when it is hurt (the red overlay in vanilla).
class HurtColor : public Module {
public:
    HurtColor();

    void onActorOverlay(Event& ev);

private:
    ValueType color = ColorValue(1.f, 0.f, 0.f, 1.f);
    ValueType opacity = FloatValue(0.6f);      // How strong the flash is, 0 = invisible, 1 = solid.
    ValueType matchVanillaFlash = BoolValue(true); // Only while the game's own flash is showing.
    ValueType onlyPlayers = BoolValue(false);
};
