#pragma once
#include "client/event/Event.h"
#include "util/Crypto.h"
#include "util/Util.h"
#include "mc/common/world/actor/Actor.h"

// FIXME: is this really cancellable?
class OverlayColorEvent : public Cancellable {
public:
    static const uint32_t hash = TOHASH(OverlayColorEvent);

    [[nodiscard]] Color& getColor() { return *color; }
    [[nodiscard]] SDK::Actor* getActor() { return actor; }
    // The render controller this color is for (the thing that draws one model: the body, a held item, a shield...).
    // Only an address to tell controllers apart, nothing to call. May be null.
    [[nodiscard]] const void* getController() const { return controller; }

    OverlayColorEvent(Color& color, SDK::Actor* actor, const void* controller = nullptr)
        : color(&color)
        , actor(actor)
        , controller(controller) {}

protected:
    Color* color;
    SDK::Actor* actor;
    const void* controller;
};
