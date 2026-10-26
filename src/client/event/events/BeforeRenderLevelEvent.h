#pragma once
#include "client/event/Event.h"
#include "util/Crypto.h"
#include "mc/common/client/renderer/game/LevelRenderer.h"

// Fired right BEFORE the game draws the world (and so every entity in it). RenderLevelEvent fires after that, which is
// too late for anything that must be in place when the entities are drawn: whatever the game changes on an entity
// between two frames is already there when the next frame is drawn, and a change made in RenderLevelEvent only shows up
// in the frame after, if it survived.
class BeforeRenderLevelEvent : public Event {
public:
    static const uint32_t hash = TOHASH(BeforeRenderLevelEvent);

    [[nodiscard]] SDK::LevelRenderer* getLevelRenderer() { return rend; }
    [[nodiscard]] SDK::ScreenContext* getScreenContext() { return screen; }

    BeforeRenderLevelEvent(SDK::LevelRenderer* rend, SDK::ScreenContext* ctx)
        : rend(rend)
        , screen(ctx) {}

private:
    SDK::LevelRenderer* rend;
    SDK::ScreenContext* screen;
};
