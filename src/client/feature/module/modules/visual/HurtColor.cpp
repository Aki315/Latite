#include "pch.h"
#include "HurtColor.h"
#include "client/event/Eventing.h"
#include "client/event/events/OverlayColorEvent.h"
#include "mc/common/client/game/ClientInstance.h"

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

    addSetting("showOnSelf", LocalizeString::get("client.module.hurtColor.showOnSelf.name"),
               LocalizeString::get("client.module.hurtColor.showOnSelf.desc"), showOnSelf);

    listen<OverlayColorEvent>(static_cast<EventListenerFunc>(&HurtColor::onActorOverlay));
}

// Called every time the game works out the overlay color of an entity; the color the game picked is already in the
// event. We replace it when the entity was just hurt.
void HurtColor::onActorOverlay(Event& evGeneric) {
    auto& ev = reinterpret_cast<OverlayColorEvent&>(evGeneric);
    auto* actor = ev.getActor();
    if (!actor) return;

    const bool isSelf = actor == SDK::ClientInstance::get()->getLocalPlayer();
    if (isSelf && !std::get<BoolValue>(showOnSelf)) return;

    // What the game picked for this entity right now. Its hurt flash is a red with some alpha; a creeper about to
    // blow is white, and nothing is drawn (alpha 0) when there is no flash.
    const Color game = ev.getColor();
    const bool gameFlashing = game.a > 0.01f;
    const bool whiteFlash = game.g > 0.6f && game.b > 0.6f;

    if (isSelf) {
        // Your own entity: the client knows your immunity time, so the longer window can be used too.
        if (actor->invulnerableTime <= 0) return;
        if (std::get<BoolValue>(matchVanillaFlash) && !gameFlashing) return;
    } else {
        // Other players and mobs: the client is never told how long they can't be hit again (that stays at 0 on
        // their entity), so the only sign that they were hurt is the game's own red flash. The color follows that
        // flash, whichever Match Game Flash is set to. The creeper's white flash is left alone.
        if (!gameFlashing || whiteFlash) return;
    }

    if (std::get<BoolValue>(onlyPlayers) && !actor->isPlayer()) return;

    const auto setColor = std::get<ColorValue>(color).getMainColor();
    const float alpha = std::clamp(setColor.a * std::get<FloatValue>(opacity).value, 0.f, 1.f);
    ev.getColor() = { setColor.r, setColor.g, setColor.b, alpha };
}
