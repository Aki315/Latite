#include "pch.h"
#include "HurtColor.h"
#include "client/event/Eventing.h"
#include "client/event/events/OverlayColorEvent.h"

#include <algorithm>

HurtColor::HurtColor()
    : Module("HurtColor", LocalizeString::get("client.module.hurtColor.name"),
             LocalizeString::get("client.module.hurtColor.desc"), GAME) {
    addSetting("color", LocalizeString::get("client.module.hurtColor.color.name"),
               LocalizeString::get("client.module.hurtColor.color.desc"), color);
    addSliderSetting("opacity", LocalizeString::get("client.module.hurtColor.opacity.name"),
                     LocalizeString::get("client.module.hurtColor.opacity.desc"), opacity, FloatValue(0.f),
                     FloatValue(1.f), FloatValue(0.05f));
    addSetting("matchVanillaFlash", LocalizeString::get("client.module.hurtColor.matchVanillaFlash.name"),
               LocalizeString::get("client.module.hurtColor.matchVanillaFlash.desc"), matchVanillaFlash);
    addSetting("onlyPlayers", LocalizeString::get("client.module.hurtColor.onlyPlayers.name"),
               LocalizeString::get("client.module.hurtColor.onlyPlayers.desc"), onlyPlayers);

    listen<OverlayColorEvent>(static_cast<EventListenerFunc>(&HurtColor::onActorOverlay));
}

// Called every time the game works out the overlay color of an entity; the color the game picked is already in the
// event. We replace it when the entity was just hurt.
void HurtColor::onActorOverlay(Event& evGeneric) {
    auto& ev = reinterpret_cast<OverlayColorEvent&>(evGeneric);
    auto* actor = ev.getActor();
    if (!actor) return;

    // Recent damage: the entity is in its "just hurt" window.
    if (actor->invulnerableTime <= 0) return;
    // Keep the game's own timing: only recolor while its flash is actually showing (not the whole invulnerable
    // window, which is longer). The creeper's white flash is left alone this way too.
    if (std::get<BoolValue>(matchVanillaFlash) && ev.getColor().a <= 0.01f) return;

    if (std::get<BoolValue>(onlyPlayers) && !actor->isPlayer()) return;

    const auto setColor = std::get<ColorValue>(color).getMainColor();
    const float alpha = std::clamp(setColor.a * std::get<FloatValue>(opacity).value, 0.f, 1.f);
    ev.getColor() = { setColor.r, setColor.g, setColor.b, alpha };
}
