#include "pch.h"
#include "Hitboxes.h"
#include <util/DrawUtil3D.h>
#include "mc/Addresses.h"
#include "mc/common/client/game/Options.h"
#include "mc/common/world/level/BlockSource.h"
#include "mc/common/world/level/block/Block.h"
#include "mc/common/world/level/block/BlockLegacy.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <unordered_map>

namespace {
    // How far a point is from the closest point of a box (0 when the point is inside it).
    float distanceToBox(Vec3 const& p, AABB const& bb) {
        const float dx = p.x - std::clamp(p.x, bb.lower.x, bb.higher.x);
        const float dy = p.y - std::clamp(p.y, bb.lower.y, bb.higher.y);
        const float dz = p.z - std::clamp(p.z, bb.lower.z, bb.higher.z);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    enum class HitBlock { Passes, Stops, Web };

    // What a block does to a hit: air, liquids and small plants let it through, a cobweb is told apart (the setting
    // decides), and anything else (cubes, slabs, stairs, ...) stops it. Judged from the block's name, once per kind.
    HitBlock classify(SDK::Block* block) {
        static std::unordered_map<const void*, HitBlock> cache;
        SDK::BlockLegacy* legacy = block ? block->legacyBlock : nullptr;
        if (!legacy) return HitBlock::Passes;
        if (auto it = cache.find(legacy); it != cache.end()) return it->second;

        std::string id = legacy->namespacedId.getString();
        if (id.rfind("minecraft:", 0) == 0) id.erase(0, 10);

        auto has = [&](std::string_view part) { return id.find(part) != std::string::npos; };
        HitBlock result = HitBlock::Stops;
        if (id == "web") {
            result = HitBlock::Web;
        } else if (id == "air" || id == "cave_air" || id == "void_air" || id == "water" || id == "flowing_water" ||
                   id == "lava" || id == "flowing_lava" || id == "bubble_column" || id == "fire" || id == "soul_fire" ||
                   id == "structure_void" || id == "redstone_wire" || id == "tripwire" || id == "tripwire_hook" ||
                   id == "lever" || id == "snow_layer" || id == "vine" || id == "glow_lichen" || id == "sculk_vein" ||
                   id == "hanging_roots" || id == "tallgrass" || id == "short_grass" || id == "tall_grass" ||
                   id == "fern" || id == "large_fern" || id == "double_plant" || id == "deadbush" ||
                   id == "seagrass" || id == "kelp" || id == "brown_mushroom" || id == "red_mushroom" ||
                   has("light_block") || has("torch") || has("button") || has("_sign") || has("rail") ||
                   has("carpet") || has("flower") || has("tulip") || has("orchid") || has("sapling") ||
                   has("poppy") || has("dandelion") || has("allium") || has("bluet") || has("cornflower") ||
                   has("lily_of_the_valley") || has("daisy") || has("rose") || has("petals") ||
                   has("coral_fan") || has("pressure_plate")) {
            result = HitBlock::Passes;
        }
        cache.emplace(legacy, result);
        return result;
    }

    // Whether something stops a hit going from `from` to `to`: walks the blocks the line crosses, in order, starting
    // after the one your eyes are in. A block entered right at the end point is not counted.
    bool lineBlocked(SDK::BlockSource* region, Vec3 const& from, Vec3 const& to, bool websStop) {
        const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
        if (dx * dx + dy * dy + dz * dz < 1e-6f) return false;

        int x = static_cast<int>(std::floor(from.x));
        int y = static_cast<int>(std::floor(from.y));
        int z = static_cast<int>(std::floor(from.z));
        const float inf = 1e30f;
        const int sx = dx > 0.f ? 1 : -1, sy = dy > 0.f ? 1 : -1, sz = dz > 0.f ? 1 : -1;
        const float tdx = dx != 0.f ? std::abs(1.f / dx) : inf;
        const float tdy = dy != 0.f ? std::abs(1.f / dy) : inf;
        const float tdz = dz != 0.f ? std::abs(1.f / dz) : inf;
        float tx = dx > 0.f ? (x + 1 - from.x) / dx : dx < 0.f ? (from.x - x) / -dx : inf;
        float ty = dy > 0.f ? (y + 1 - from.y) / dy : dy < 0.f ? (from.y - y) / -dy : inf;
        float tz = dz > 0.f ? (z + 1 - from.z) / dz : dz < 0.f ? (from.z - z) / -dz : inf;

        for (int i = 0; i < 48; ++i) {
            float t;
            if (tx <= ty && tx <= tz) {
                t = tx;
                x += sx;
                tx += tdx;
            } else if (ty <= tz) {
                t = ty;
                y += sy;
                ty += tdy;
            } else {
                t = tz;
                z += sz;
                tz += tdz;
            }
            if (t >= 0.9995f) break; // Reached the end point.

            const HitBlock kind = classify(region->getBlock(x, y, z));
            if (kind == HitBlock::Stops || (kind == HitBlock::Web && websStop)) return true;
        }
        return false;
    }

    // Can a hit land on this box from here? True when at least one of a few points on it (the closest point, the middle,
    // the head) can be reached without something stopping the line.
    bool canReach(SDK::BlockSource* region, Vec3 const& eye, AABB const& bb, bool websStop) {
        const Vec3 closest { std::clamp(eye.x, bb.lower.x, bb.higher.x), std::clamp(eye.y, bb.lower.y, bb.higher.y),
                             std::clamp(eye.z, bb.lower.z, bb.higher.z) };
        const Vec3 mid { (bb.lower.x + bb.higher.x) * 0.5f, (bb.lower.y + bb.higher.y) * 0.5f,
                         (bb.lower.z + bb.higher.z) * 0.5f };
        const Vec3 head { mid.x, bb.higher.y - 0.15f, mid.z };
        return !lineBlocked(region, eye, closest, websStop) || !lineBlocked(region, eye, mid, websStop) ||
               !lineBlocked(region, eye, head, websStop);
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
    addSetting("needClearLine", LocalizeString::get("client.module.hitboxes.needClearLine.name"),
               LocalizeString::get("client.module.hitboxes.needClearLine.desc"), needClearLine, "rangeColorOn"_istrue);
    addSetting("websBlock", LocalizeString::get("client.module.hitboxes.websBlock.name"),
               LocalizeString::get("client.module.hitboxes.websBlock.desc"), websBlock, "rangeColorOn"_istrue);
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
    addSetting("thinWithDistance", LocalizeString::get("client.module.hitboxes.thinWithDistance.name"),
               LocalizeString::get("client.module.hitboxes.thinWithDistance.desc"), thinWithDistance);
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
    addSetting("mobs", LocalizeString::get("client.module.hitboxes.mobs.name"),
               LocalizeString::get("client.module.hitboxes.mobs.desc"), mobs);
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
    // Full thickness up to 3 blocks away, then thinner the further it is, so far hitboxes look like near ones.
    dc.setThicknessFalloff(std::get<BoolValue>(thinWithDistance) ? 3.f : 0.f);

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
    const bool needLine = std::get<BoolValue>(needClearLine);
    const bool websStop = std::get<BoolValue>(websBlock);
    SDK::BlockSource* region = SDK::ClientInstance::get()->getRegion();

    const bool hideOthers = std::get<BoolValue>(hideEntities);
    const bool showMobsOn = std::get<BoolValue>(mobs);
    const float maxDist = std::get<FloatValue>(maxDistance).value;
    const bool limitDist = maxDist > 0.f && lp != nullptr;
    const bool shrinkFar = std::get<BoolValue>(shrinkWithDistance) && limitDist;

    for (const auto entt : level->getRuntimeActorList()) {
        if (entt->isInvisible()) continue;
        if (entt == lp && !drawSelf) continue;
        if (!std::get<BoolValue>(items) && entt->getEntityTypeID() == 64) continue;

        // "Show Mobs" off: no hitbox on mobs. A mob is anything that is not a player, a dropped item or one of the
        // non-mob things above (those have their own switches).
        if (!showMobsOn && !entt->isPlayer() && entt->getEntityTypeID() != 64 &&
            !isNonMobEntity(entt->getEntityTypeName())) {
            continue;
        }

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
        bool inRange = checkRange && entt != lp && entt->isPlayer() && distanceToBox(selfEye, bb) <= reach;
        // And a hit has to be able to land: a player who covered himself with blocks (or cobwebs, if those count) is
        // in reach but not hittable, so the box keeps its normal color.
        if (inRange && needLine && region) inRange = canReach(region, selfEye, bb, websStop);

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
