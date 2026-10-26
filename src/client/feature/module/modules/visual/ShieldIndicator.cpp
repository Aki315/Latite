#include "pch.h"
#include "ShieldIndicator.h"

#include <util/DrawUtil3D.h>
#include "client/event/events/PacketReceiveEvent.h"
#include "mc/Addresses.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/game/Options.h"
#include "mc/common/client/renderer/MaterialPtr.h"
#include "mc/common/entity/component/ActorEquipmentComponent.h"
#include "mc/common/entity/component/MobBodyRotationComponent.h"
#include "mc/common/network/Packet.h"
#include "mc/common/world/Item.h"
#include "mc/common/world/ItemStack.h"
#include "mc/common/world/actor/Actor.h"
#include "mc/common/world/actor/player/Inventory.h"
#include "mc/common/world/actor/player/Player.h"
#include "mc/common/world/actor/player/PlayerInventory.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
    // Status flags of an actor. 4 = using an item (holding right click on the shield, the addon's shield item has a
    // very long use time), 71 = blocking (vanilla shield).
    constexpr int kFlagUsingItem = 4;
    constexpr int kFlagBlocking = 71;

    // PlaySoundPacket: protocol packet id 86 (0x56), between Transfer (0x55) and SetTitle (0x58) in the PacketID enum.
    // Its fields follow the Packet base class like the other packets in this SDK (compare SetTitlePacket): the sound's
    // name, the position as a block position in eighths of a block, then volume and pitch.
    //
    // NOT verified in a running game: if this layout is wrong the reader below gives up quietly (the checks reject
    // anything that isn't a plausible sound name), no red is shown, and the Debug Log setting says what was seen.
    constexpr auto kPlaySoundPacketId = static_cast<SDK::PacketID>(0x56);
    struct PlaySoundPacketView : SDK::Packet {
        std::string name;
        int x;
        int y;
        int z;
        float volume;
        float pitch;
    };

    struct RawSound {
        char name[48];
        float pitch;
        int x, y, z;
    };

    // Reads the name, position and pitch of a PlaySoundPacket without trusting the layout: an access violation while
    // reading returns false instead of crashing. No objects with destructors in here, which __try requires.
    bool readPlaySound(SDK::Packet* pkt, RawSound& out) {
        __try {
            auto* view = reinterpret_cast<PlaySoundPacketView*>(pkt);
            const std::string& text = view->name;
            const size_t length = text.size();
            if (length == 0 || length >= sizeof(out.name) || text.capacity() < length || text.capacity() > 4096) {
                return false;
            }
            const char* data = text.data();
            for (size_t i = 0; i < length; ++i) {
                const char c = data[i];
                if (c < 32 || c > 126) return false; // Not a sound name.
                out.name[i] = c;
            }
            out.name[length] = '\0';
            out.pitch = view->pitch;
            out.x = view->x;
            out.y = view->y;
            out.z = view->z;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool isShieldItem(SDK::ItemStack* stack) {
        auto* item = stack ? stack->getItem() : nullptr;
        return item && item->namespacedId.getString().find("shield") != std::string::npos;
    }

    // Which hands hold a shield. The main hand is slot 0 of the hand container and the offhand slot 1; for yourself the
    // main hand is the selected hotbar slot.
    void heldShields(SDK::Actor* actor, bool isLocal, bool& mainHand, bool& offHand) {
        mainHand = offHand = false;
        auto* equipment = actor->tryGetComponent<SDK::ActorEquipmentComponent>();
        if (equipment && equipment->handContainer) {
            mainHand = isShieldItem(equipment->handContainer->getItem(0));
            offHand = isShieldItem(equipment->handContainer->getItem(1));
        }
        if (isLocal) {
            auto* me = static_cast<SDK::Player*>(actor);
            if (me->supplies && me->supplies->inventory) {
                mainHand = isShieldItem(me->supplies->inventory->getItem(me->supplies->selectedSlot));
            }
        }
    }

    // An angle between two values the short way round (angles wrap at 360).
    float lerpAngle(float from, float to, float t) {
        float delta = std::fmod(to - from, 360.f);
        if (delta > 180.f) delta -= 360.f;
        if (delta < -180.f) delta += 360.f;
        return from + delta * t;
    }

    Vec3 add(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    Vec3 scale(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }

    struct Seen {
        SDK::Actor* actor;
        uint64_t id;
        Vec3 feet;
        float yaw; // Body direction in degrees.
        bool offhand; // The shield is in the offhand (the left arm).
        bool holds;
        bool self;
    };
} // namespace

ShieldIndicator::ShieldIndicator()
    : Module("ShieldIndicator", LocalizeString::get("client.module.shieldIndicator.name"),
             LocalizeString::get("client.module.shieldIndicator.desc"), GAME) {
    addSetting("blockColor", LocalizeString::get("client.module.shieldIndicator.blockColor.name"),
               LocalizeString::get("client.module.shieldIndicator.blockColor.desc"), blockColor);
    addSetting("brokenColor", LocalizeString::get("client.module.shieldIndicator.brokenColor.name"),
               LocalizeString::get("client.module.shieldIndicator.brokenColor.desc"), brokenColor);
    addSliderSetting("cooldown", LocalizeString::get("client.module.shieldIndicator.cooldown.name"),
                     LocalizeString::get("client.module.shieldIndicator.cooldown.desc"), cooldown, FloatValue(1.f),
                     FloatValue(15.f), FloatValue(0.5f));
    addSetting("showReady", LocalizeString::get("client.module.shieldIndicator.showReady.name"),
               LocalizeString::get("client.module.shieldIndicator.showReady.desc"), showReady);
    addSetting("ownThirdPerson", LocalizeString::get("client.module.shieldIndicator.ownThirdPerson.name"),
               LocalizeString::get("client.module.shieldIndicator.ownThirdPerson.desc"), ownThirdPerson);
    addSliderSetting("maxDistance", LocalizeString::get("client.module.shieldIndicator.maxDistance.name"),
                     LocalizeString::get("client.module.shieldIndicator.maxDistance.desc"), maxDistance, FloatValue(8.f),
                     FloatValue(64.f), FloatValue(1.f));
    addSliderSetting("size", LocalizeString::get("client.module.shieldIndicator.size.name"),
                     LocalizeString::get("client.module.shieldIndicator.size.desc"), size, FloatValue(0.5f),
                     FloatValue(2.f), FloatValue(0.05f));
    addSliderSetting("offsetForward", LocalizeString::get("client.module.shieldIndicator.offsetForward.name"),
                     LocalizeString::get("client.module.shieldIndicator.offsetForward.desc"), offsetForward,
                     FloatValue(-0.5f), FloatValue(0.5f), FloatValue(0.01f));
    addSliderSetting("offsetSide", LocalizeString::get("client.module.shieldIndicator.offsetSide.name"),
                     LocalizeString::get("client.module.shieldIndicator.offsetSide.desc"), offsetSide, FloatValue(-0.5f),
                     FloatValue(0.5f), FloatValue(0.01f));
    addSliderSetting("offsetUp", LocalizeString::get("client.module.shieldIndicator.offsetUp.name"),
                     LocalizeString::get("client.module.shieldIndicator.offsetUp.desc"), offsetUp, FloatValue(-0.5f),
                     FloatValue(0.5f), FloatValue(0.01f));
    addSetting("debugLog", LocalizeString::get("client.module.shieldIndicator.debugLog.name"),
               LocalizeString::get("client.module.shieldIndicator.debugLog.desc"), debugLog);

    Eventing::get().listen<RenderLevelEvent, &ShieldIndicator::onRenderLevel>(this);
    listen<PacketReceiveEvent>((EventListenerFunc)&ShieldIndicator::onPacketReceive);
}

void ShieldIndicator::onPacketReceive(Event& evG) {
    auto& ev = reinterpret_cast<PacketReceiveEvent&>(evG);
    auto* pkt = ev.getPacket();
    if (!pkt || pkt->getID() != kPlaySoundPacketId) return;

    RawSound sound;
    if (!readPlaySound(pkt, sound)) return;
    // Only break sounds matter (a shield breaking, but also tools wearing out, which the debug log shows).
    if (std::string_view(sound.name).find("break") == std::string_view::npos) return;

    std::lock_guard lock(soundMutex);
    if (pendingSounds.size() >= 16) return;
    pendingSounds.push_back({ sound.name, sound.pitch, sound.x, sound.y, sound.z, std::chrono::steady_clock::now() });
}

void ShieldIndicator::onDisable() {
    tracker.clear();
    std::lock_guard lock(soundMutex);
    pendingSounds.clear();
}

void ShieldIndicator::onRenderLevel(RenderLevelEvent&) {
    auto* ci = SDK::ClientInstance::get();
    if (!ci || !ci->minecraft || !ci->minecraft->timer) return;
    auto* level = ci->minecraft->getLevel();
    auto* lp = ci->getLocalPlayer();
    if (!level || !lp) return;

    const auto now = std::chrono::steady_clock::now();
    const float partial = ci->minecraft->timer->alpha;

    // 0 = first person, 1 = third person (back), 2 = third person (front).
    int perspective = 0;
    if (Signatures::Options_getPerspective.result) {
        if (auto* options = ci->getOptions()) perspective = options->getPlayerViewPerspective();
    }
    const bool drawSelf = std::get<BoolValue>(ownThirdPerson) && perspective != 0;

    // ---- Everyone with a shield, where they are.
    std::vector<Seen> players;
    std::vector<ShieldState::Candidate> holders;
    for (SDK::Actor* actor : level->getRuntimeActorList()) {
        if (!actor || !actor->isPlayer()) continue;
        const bool self = actor == lp;

        bool mainHand = false, offHand = false;
        heldShields(actor, self, mainHand, offHand);

        Seen seen {};
        seen.actor = actor;
        seen.id = actor->getRuntimeID();
        seen.self = self;
        seen.holds = mainHand || offHand;
        seen.offhand = offHand;

        const Vec3 posOld = actor->getPosOld();
        const Vec3 posNow = actor->getPos();
        const Vec3 pos { std::lerp(posOld.x, posNow.x, partial), std::lerp(posOld.y, posNow.y, partial),
                         std::lerp(posOld.z, posNow.z, partial) };
        const float eyeOffset = posNow.y - actor->getBoundingBox().lower.y; // getPos() is at the eyes.
        seen.feet = { pos.x, pos.y - eyeOffset, pos.z };

        if (auto* body = actor->tryGetComponent<SDK::MobBodyRotationComponent>()) {
            seen.yaw = lerpAngle(body->yBodyRotOld, body->yBodyRot, partial);
        } else {
            seen.yaw = actor->getRot().y;
        }

        if (seen.holds) holders.push_back({ seen.id, seen.feet.x, seen.feet.y, seen.feet.z });
        players.push_back(seen);
    }

    // ---- Break sounds that came in since last frame: which shield did each one break?
    std::vector<BreakSound> sounds;
    {
        std::lock_guard lock(soundMutex);
        sounds.swap(pendingSounds);
    }
    const bool debug = std::get<BoolValue>(debugLog);
    const float cooldownSeconds = std::get<FloatValue>(cooldown).value;
    for (auto const& sound : sounds) {
        if (now - sound.when > std::chrono::seconds(2)) continue; // Too old to mean anything.
        const bool isBreak = ShieldState::isShieldBreakSound(sound.name, sound.pitch);
        std::optional<uint64_t> who;
        if (isBreak) who = ShieldState::attribute(holders, sound.x, sound.y, sound.z);
        if (who) tracker.noteBroken(*who, sound.when, cooldownSeconds);
        if (debug) {
            Logger::Info("[ShieldIndicator] sound '{}' pitch {} at raw ({}, {}, {}): {}{}", sound.name, sound.pitch, sound.x,
                         sound.y, sound.z, isBreak ? "shield break" : "not a shield break (needs pitch 1.2)",
                         isBreak ? (who ? ", matched a shield holder" : ", NOT matched to any shield holder") : "");
        }
    }

    // ---- Update every shield holder, and draw.
    const float maxDist = std::get<FloatValue>(maxDistance).value;
    const bool tinted = std::get<BoolValue>(showReady);
    const d2d::Color blockCol = d2d::Color(std::get<ColorValue>(blockColor).getMainColor());
    const d2d::Color brokenCol = d2d::Color(std::get<ColorValue>(brokenColor).getMainColor());
    const float sizeScale = std::get<FloatValue>(size).value;
    const float offF = std::get<FloatValue>(offsetForward).value;
    const float offS = std::get<FloatValue>(offsetSide).value;
    const float offU = std::get<FloatValue>(offsetUp).value;
    const Vec3 lpPos = lp->getPos();

    auto fill = MCDrawUtil3D(ci->levelRenderer, SDK::ScreenContext::instance3d, SDK::MaterialPtr::getSelectionOverlayMaterial());
    auto edge = MCDrawUtil3D(ci->levelRenderer, SDK::ScreenContext::instance3d, SDK::MaterialPtr::getSelectionBoxMaterial());
    bool drew = false;

    for (auto const& p : players) {
        const bool usingItem = p.actor->getStatusFlag(kFlagUsingItem) || p.actor->getStatusFlag(kFlagBlocking);
        tracker.observe(p.id, p.holds, usingItem, now);

        if (!p.holds || p.actor->isInvisible()) continue;
        if (p.self && !drawSelf) continue;
        if (!p.self) {
            const float dx = p.feet.x - lpPos.x, dz = p.feet.z - lpPos.z;
            if (std::sqrt(dx * dx + dz * dz) > maxDist) continue;
        }

        const auto state = tracker.state(p.id, now);
        d2d::Color color;
        bool raised = false;
        switch (state) {
        case ShieldState::State::Broken:
            color = brokenCol; // A broken shield is lowered, whatever the player's hand is doing.
            break;
        case ShieldState::State::Blocking:
            color = blockCol;
            raised = true;
            break;
        case ShieldState::State::Ready:
            if (!tinted) continue;
            color = d2d::Color(blockCol.r, blockCol.g, blockCol.b, blockCol.a * 0.6f);
            break;
        default:
            continue;
        }

        // The panel sits where the shield is: in front of the body when raised, against the side of the body when not.
        const float yawRad = p.yaw * (pi_f / 180.f);
        const Vec3 forward { -std::sin(yawRad), 0.f, std::cos(yawRad) };
        const Vec3 left { std::cos(yawRad), 0.f, std::sin(yawRad) };
        const float side = p.offhand ? 1.f : -1.f; // The offhand is the left arm.

        Vec3 center, across;
        float halfWidth, halfHeight;
        if (raised) {
            center = add(p.feet, add(scale(forward, 0.5f + offF), scale(left, side * (0.28f + offS))));
            center.y += 1.05f + offU;
            across = left;
            halfWidth = 0.30f * sizeScale;
            halfHeight = 0.45f * sizeScale;
        } else {
            center = add(p.feet, add(scale(forward, 0.05f + offF), scale(left, side * (0.42f + offS))));
            center.y += 0.85f + offU;
            across = forward;
            halfWidth = 0.28f * sizeScale;
            halfHeight = 0.45f * sizeScale;
        }

        const Vec3 w = scale(across, halfWidth);
        const Vec3 h { 0.f, halfHeight, 0.f };
        const Vec3 a = { center.x - w.x - h.x, center.y - w.y - h.y, center.z - w.z - h.z };
        const Vec3 b = { center.x + w.x - h.x, center.y + w.y - h.y, center.z + w.z - h.z };
        const Vec3 c = { center.x + w.x + h.x, center.y + w.y + h.y, center.z + w.z + h.z };
        const Vec3 d = { center.x - w.x + h.x, center.y - w.y + h.y, center.z - w.z + h.z };

        // Both sides, so it shows whichever way the quad faces the camera.
        fill.fillQuad(a, b, c, d, color);
        fill.fillQuad(d, c, b, a, color);
        edge.drawQuad(a, b, c, d, d2d::Color(color.r, color.g, color.b, 1.f));
        drew = true;
    }

    if (drew) {
        fill.flush();
        edge.flush();
    }
    tracker.prune(now);
}
