#include "pch.h"
#include "Hitboxes.h"
#include <util/DrawUtil3D.h>
#include "mc/Addresses.h"
#include "mc/common/client/game/Options.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace {
    // How far a point is from the closest point of a box (0 when the point is inside it).
    float distanceToBox(Vec3 const& p, AABB const& bb) {
        const float dx = p.x - std::clamp(p.x, bb.lower.x, bb.higher.x);
        const float dy = p.y - std::clamp(p.y, bb.lower.y, bb.higher.y);
        const float dz = p.z - std::clamp(p.z, bb.lower.z, bb.higher.z);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Things that are neither a player nor a mob. Matched on the entity's name (minecraft:<name>), so it does not
    // depend on the build-specific numeric entity ids.
    bool isNonMobEntity(std::string const& type) {
        static constexpr std::string_view kNames[] = {
            "arrow", "trident", "snowball", "egg", "ender_pearl", "wind_charge", "fireball", "wither_skull",
            "fishing_hook", "xp_orb", "xp_bottle", "potion", "boat", "minecart", "armor_stand", "tnt",
            "falling_block", "painting", "item_frame", "leash_knot", "ender_crystal", "firework", "lightning_bolt",
            "evocation_fang", "shulker_bullet", "llama_spit", "area_effect_cloud", "eye_of_ender", "dragon_fireball",
            "thrown", "ice_bomb", "balloon", "tripod_camera", "chalkboard", "command_block_minecart",
        };
        for (auto name : kNames) {
            if (type.find(name) != std::string::npos) return true;
        }
        return false;
    }
}

Hitboxes::Hitboxes()
    : Module("Hitboxes", LocalizeString::get("client.module.hitboxes.name"),
             LocalizeString::get("client.module.hitboxes.desc"), GAME) {
    addSetting("transparent", LocalizeString::get("client.module.hitboxes.transparent.name"),
               LocalizeString::get("client.module.hitboxes.transparent.desc"), transparent);
    addSetting("boxColor", LocalizeString::get("client.module.hitboxes.boxColor.name"),
               LocalizeString::get("client.module.hitboxes.boxColor.desc"), boxColor);
    addSetting("rangeColorOn", LocalizeString::get("client.module.hitboxes.rangeColorOn.name"),
               LocalizeString::get("client.module.hitboxes.rangeColorOn.desc"), rangeColorOn);
    addSliderSetting("hitRange", LocalizeString::get("client.module.hitboxes.hitRange.name"),
                     LocalizeString::get("client.module.hitboxes.hitRange.desc"), hitRange, FloatValue(1.f),
                     FloatValue(8.f), FloatValue(0.1f), "rangeColorOn"_istrue);
    addSetting("rangeColor", LocalizeString::get("client.module.hitboxes.rangeColor.name"),
               LocalizeString::get("client.module.hitboxes.rangeColor.desc"), rangeColor, "rangeColorOn"_istrue);
    addSliderSetting("widthScale", LocalizeString::get("client.module.hitboxes.widthScale.name"),
                     LocalizeString::get("client.module.hitboxes.widthScale.desc"), widthScale, FloatValue(0.25f),
                     FloatValue(3.f), FloatValue(0.05f));
    addSliderSetting("heightScale", LocalizeString::get("client.module.hitboxes.heightScale.name"),
                     LocalizeString::get("client.module.hitboxes.heightScale.desc"), heightScale, FloatValue(0.25f),
                     FloatValue(3.f), FloatValue(0.05f));
    addSliderSetting("maxDistance", LocalizeString::get("client.module.hitboxes.maxDistance.name"),
                     LocalizeString::get("client.module.hitboxes.maxDistance.desc"), maxDistance, FloatValue(0.f),
                     FloatValue(256.f), FloatValue(1.f));
    addSetting("shrinkWithDistance", LocalizeString::get("client.module.hitboxes.shrinkWithDistance.name"),
               LocalizeString::get("client.module.hitboxes.shrinkWithDistance.desc"), shrinkWithDistance);
    addSliderSetting("lineThickness", LocalizeString::get("client.module.hitboxes.lineThickness.name"),
                     LocalizeString::get("client.module.hitboxes.lineThickness.desc"), lineThickness, FloatValue(1.f),
                     FloatValue(10.f), FloatValue(0.5f));
    addSetting("showEyeLine", LocalizeString::get("client.module.hitboxes.showEyeLine.name"),
               LocalizeString::get("client.module.hitboxes.showEyeLine.desc"), this->showEyeLine);
    addSetting("eyeLine", LocalizeString::get("client.module.hitboxes.eyeLine.name"),
               LocalizeString::get("client.module.hitboxes.eyeLine.desc"), this->eyeColor, "showEyeLine"_istrue);
    addSetting("showLookingAt", LocalizeString::get("client.module.hitboxes.showLookingAt.name"),
               LocalizeString::get("client.module.hitboxes.showLookingAt.desc"), this->showLine);
    addSetting("lookingAt", LocalizeString::get("client.module.hitboxes.lookingAt.name"),
               LocalizeString::get("client.module.hitboxes.lookingAt.desc"), this->lineColor, "showLookingAt"_istrue);
    addSetting("items", LocalizeString::get("client.module.hitboxes.items.name"),
               LocalizeString::get("client.module.hitboxes.items.desc"), items);
    addSetting("hideEntities", LocalizeString::get("client.module.hitboxes.hideEntities.name"),
               LocalizeString::get("client.module.hitboxes.hideEntities.desc"), hideEntities);
    addSetting("showOwnThirdPerson", LocalizeString::get("client.module.hitboxes.showOwnThirdPerson.name"),
               LocalizeString::get("client.module.hitboxes.showOwnThirdPerson.desc"), showOwnThirdPerson);

    Eventing::get().listen<RenderLevelEvent, &Hitboxes::onRenderLevel>(this);
}

void Hitboxes::onRenderLevel(RenderLevelEvent& event) {
    auto material = std::get<BoolValue>(transparent) ? SDK::MaterialPtr::getSelectionOverlayMaterial()
                                                     : SDK::MaterialPtr::getSelectionBoxMaterial();

    auto dc = MCDrawUtil3D(SDK::ClientInstance::get()->levelRenderer, SDK::ScreenContext::instance3d, material);
    dc.setLineThickness(std::get<FloatValue>(lineThickness).value);

    auto lp = SDK::ClientInstance::get()->getLocalPlayer();
    auto level = SDK::ClientInstance::get()->minecraft->getLevel();
    auto* connectionInfo = SDK::RemoteConnectorComposite::getConnectionInfo();

    if (level == nullptr) return;

    // 0 = first person, 1 = third person (back), 2 = third person (front).
    int perspective = 0;
    if (Signatures::Options_getPerspective.result) {
        if (auto* options = SDK::ClientInstance::get()->getOptions()) {
            perspective = options->getPlayerViewPerspective();
        }
    }
    const bool drawSelf = std::get<BoolValue>(showOwnThirdPerson) && perspective != 0;

    // Where your eyes are this frame, to measure the distance to everything else.
    const float partial = SDK::ClientInstance::get()->minecraft->timer->alpha;
    Vec3 selfEye {};
    if (lp) {
        selfEye = { std::lerp(lp->getPosOld().x, lp->getPos().x, partial),
                    std::lerp(lp->getPosOld().y, lp->getPos().y, partial),
                    std::lerp(lp->getPosOld().z, lp->getPos().z, partial) };
    }
    const bool checkRange = std::get<BoolValue>(rangeColorOn) && lp != nullptr;
    const float reach = std::get<FloatValue>(hitRange).value;

    const bool hideOthers = std::get<BoolValue>(hideEntities);
    const float maxDist = std::get<FloatValue>(maxDistance).value;
    const bool limitDist = maxDist > 0.f && lp != nullptr;
    const bool shrinkFar = std::get<BoolValue>(shrinkWithDistance) && limitDist;

    for (const auto entt : level->getRuntimeActorList()) {
        if (entt->isInvisible()) continue;
        if (entt == lp && !drawSelf) continue;
        if (!std::get<BoolValue>(items) && entt->getEntityTypeID() == 64) continue;

        // "Hide Entities": no hitbox for projectiles, boats, minecarts, armor stands and the like. Players and
        // mobs always keep theirs. Dropped items have their own switch above.
        if (hideOthers && !entt->isPlayer() && entt->getEntityTypeID() != 64) {
            if (isNonMobEntity(entt->getEntityTypeName())) continue;
        }

        Vec3 newPos = {
            std::lerp(entt->getPosOld().x, entt->getPos().x, SDK::ClientInstance::get()->minecraft->timer->alpha),
            std::lerp(entt->getPosOld().y, entt->getPos().y, SDK::ClientInstance::get()->minecraft->timer->alpha),
            std::lerp(entt->getPosOld().z, entt->getPos().z, SDK::ClientInstance::get()->minecraft->timer->alpha)
        };

        AABB bb = entt->getBoundingBox();
        float eyeOffset = entt->getPos().y - bb.lower.y;
        Vec3 rebasePos =
            newPos.operator-({ 0.f, eyeOffset, 0.f }).operator+({ 0.f, (bb.higher.y - bb.lower.y) / 2.f, 0.f });
        bb.rebase(rebasePos);

        // In hit range: close enough that a hit would land. Measured from your eyes to the real box, before the
        // visual scaling below. Only for other players.
        const bool inRange = checkRange && entt != lp && entt->isPlayer() && distanceToBox(selfEye, bb) <= reach;

        // Max Distance: too far away, so not drawn at all.
        float distFade = 0.f; // 0 = right next to you, 1 = at the max distance
        if (limitDist && entt != lp) {
            const float dist = distanceToBox(selfEye, bb);
            if (dist > maxDist) continue;
            distFade = std::clamp(dist / maxDist, 0.f, 1.f);
        }

        // Visual scaling: width/depth grow around the box center, height grows upward from the feet.
        // The extra size fades out with distance, so a far box is the real size instead of looking huge.
        float wScale = std::get<FloatValue>(widthScale).value;
        float hScale = std::get<FloatValue>(heightScale).value;
        if (shrinkFar) {
            wScale = 1.f + (wScale - 1.f) * (1.f - distFade);
            hScale = 1.f + (hScale - 1.f) * (1.f - distFade);
        }
        const float origLowerY = bb.lower.y;
        if (wScale != 1.f || hScale != 1.f) {
            const Vec3 center = bb.getCenter();
            const float halfW = (bb.higher.x - bb.lower.x) * 0.5f * wScale;
            const float halfD = (bb.higher.z - bb.lower.z) * 0.5f * wScale;
            const float newHeight = (bb.higher.y - bb.lower.y) * hScale;
            bb.lower = { center.x - halfW, origLowerY, center.z - halfD };
            bb.higher = { center.x + halfW, origLowerY + newHeight, center.z + halfD };
        }

        bool willShowLine = std::get<BoolValue>(showLine) && (!entt->isPlayer() || !connectionInfo ||
                                                              connectionInfo->hostIpAddress.empty() || entt == lp);

        auto boxCol = inRange ? std::get<ColorValue>(rangeColor).getMainColor() : std::get<ColorValue>(boxColor).getMainColor();
        auto lineCol = std::get<ColorValue>(lineColor).getMainColor();
        auto eyeCol = std::get<ColorValue>(eyeColor).getMainColor();

        dc.drawBox(bb, boxCol);
        // Keep the eye line at the same relative height inside a height-scaled box.
        float eyePos = origLowerY + (newPos.y - origLowerY) * hScale;
        float eyeLine = eyePos;
        bool customEyeLine = false;

        if (customEyeLine = LatiteMath::aequals(bb.lower.y, eyePos)) {
            eyeLine = bb.lower.y + (bb.higher.y - bb.lower.y) * 0.85f;
        }

        if (std::get<BoolValue>(showEyeLine)) {
            dc.drawQuad(Vec3(bb.lower.x, eyeLine, bb.lower.z), Vec3(bb.higher.x, eyeLine, bb.lower.z),
                        Vec3(bb.higher.x, eyeLine, bb.higher.z), Vec3(bb.lower.x, eyeLine, bb.higher.z), eyeCol);
        }

        if (willShowLine) {
            float calcYaw = (entt->getRot().y + 90) * (pi_f / 180);
            float calcPitch = entt->getRot().x * -(pi_f / 180);
            float mod = 1.f;

            Vec3 offset;
            offset.x = cos(calcYaw) * cos(calcPitch) * mod;
            offset.y = sin(calcPitch) * mod;
            offset.z = sin(calcYaw) * cos(calcPitch) * mod;

            Vec3 begin = newPos;
            begin.y = customEyeLine ? eyeLine : eyePos;
            Vec3 end = begin + offset;

            BlockPos bp { static_cast<int>((end.x)), static_cast<int>((end.y)), static_cast<int>((end.z)) };

            dc.drawLine(begin, end, lineCol);
        }
        dc.flush();
    }
}
