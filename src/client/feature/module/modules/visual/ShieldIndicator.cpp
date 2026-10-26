#include "pch.h"
#include "ShieldIndicator.h"

#include "client/Latite.h"
#include "client/event/events/OverlayColorEvent.h"
#include "client/event/events/RenderOverlayEvent.h"
#include "client/feature/setting/Setting.h"
#include "client/render/asset/Assets.h"
#include "util/ObjectStrings.h"
#include "util/WorldToScreen.h"
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
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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

    // Reads game memory without risking a crash. Only committed, readable memory that holds the whole read is touched,
    // and a read that faults anyway is caught. No objects with destructors in here, which __try requires.
    bool safeRead(const void* from, void* to, size_t bytes) {
        if (!from || bytes == 0) return false;

        MEMORY_BASIC_INFORMATION info {};
        if (VirtualQuery(from, &info, sizeof(info)) == 0) return false;
        if (info.State != MEM_COMMIT) return false;
        if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(info.Protect & readable)) return false;

        const auto begin = reinterpret_cast<uintptr_t>(from);
        const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (begin + bytes > regionEnd) return false; // Would run into the next region, which may not be readable.

        __try {
            std::memcpy(to, from, bytes);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    struct ControllerText {
        bool shield = false;                // Does it say it is the shield's render controller?
        std::vector<std::string> names;     // Every text with "shield" in it that was found (for the debug log).
    };

    // Looks through a render controller object for its name. The layout of that object is not known, so this reads the
    // text out of it instead of a field at a fixed offset (see util/ObjectStrings.h).
    ControllerText inspectController(const void* controller) {
        ControllerText result;
        result.names = ObjectStrings::findStrings(controller, "shield", safeRead);
        for (auto const& name : result.names) {
            if (ObjectStrings::looksLikeShieldName(name)) result.shield = true;
        }
        return result;
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
    addSetting("tintShield", LocalizeString::get("client.module.shieldIndicator.tintShield.name"),
               LocalizeString::get("client.module.shieldIndicator.tintShield.desc"), tintShield);
    addSetting("imageFallback", LocalizeString::get("client.module.shieldIndicator.imageFallback.name"),
               LocalizeString::get("client.module.shieldIndicator.imageFallback.desc"), imageFallback,
               "tintShield"_istrue);
    addSliderSetting("opacity", LocalizeString::get("client.module.shieldIndicator.opacity.name"),
                     LocalizeString::get("client.module.shieldIndicator.opacity.desc"), opacity, FloatValue(0.2f),
                     FloatValue(1.f), FloatValue(0.05f));
    addSliderSetting("cooldown", LocalizeString::get("client.module.shieldIndicator.cooldown.name"),
                     LocalizeString::get("client.module.shieldIndicator.cooldown.desc"), cooldown, FloatValue(1.f),
                     FloatValue(15.f), FloatValue(0.5f));
    addSetting("markRaised", LocalizeString::get("client.module.shieldIndicator.markRaised.name"),
               LocalizeString::get("client.module.shieldIndicator.markRaised.desc"), markRaised);
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
    listen<RenderOverlayEvent>(&ShieldIndicator::onRenderOverlay);
    listen<OverlayColorEvent>((EventListenerFunc)&ShieldIndicator::onOverlayColor);
}

// Called by the game for every model it draws, to ask what overlay color to put over it. For the shield of a player whose
// shield is disabled (red) or raised (green) the answer is that color.
void ShieldIndicator::onOverlayColor(Event& evG) {
    if (!anyTint.load(std::memory_order_relaxed)) return;

    auto& ev = reinterpret_cast<OverlayColorEvent&>(evG);
    const SDK::Actor* actor = ev.getActor();
    const void* controller = ev.getController();
    if (!actor || !controller) return;

    const auto now = std::chrono::steady_clock::now();

    bool broken = false;
    bool known = false;
    bool isShield = false;
    {
        std::lock_guard lock(tintMutex);
        auto target = tintTargets.find(actor);
        if (target == tintTargets.end()) return; // Not a player whose shield gets a color.
        broken = target->second;

        // Resource packs can be reloaded, and then a controller's memory can be used for something else: look again now
        // and then instead of trusting an old answer forever.
        auto guess = controllers.find(controller);
        if (guess != controllers.end() && now - guess->second.checked < std::chrono::seconds(20)) {
            known = true;
            isShield = guess->second.shield;
        }
    }

    if (!known) {
        const ControllerText text = inspectController(controller);
        isShield = text.shield;
        {
            std::lock_guard lock(tintMutex);
            if (controllers.size() > 512) controllers.clear();
            controllers[controller] = { isShield, now };
        }
        if (std::get<BoolValue>(debugLog)) {
            std::string names;
            for (auto const& name : text.names) names += (names.empty() ? "'" : ", '") + name + "'";
            Logger::Info("[ShieldIndicator] render controller at {:x}: {} (text with \"shield\" in it: {})",
                         reinterpret_cast<uintptr_t>(controller), isShield ? "THE SHIELD, recolored" : "not the shield",
                         names.empty() ? "none found" : names);
        }
    }
    if (!isShield) return;

    const float alpha = std::get<FloatValue>(opacity).value;
    ev.getColor() = broken ? Color(0.92f, 0.12f, 0.12f, alpha) : Color(0.18f, 0.85f, 0.32f, alpha);

    std::lock_guard lock(tintMutex);
    tintedAt[actor] = now;
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
    {
        std::lock_guard panelLock(panelMutex);
        panels.clear();
    }
    {
        std::lock_guard lock(tintMutex);
        tintTargets.clear();
        tintedAt.clear();
        controllers.clear();
        anyTint = false;
    }
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
    const float opacityValue = std::get<FloatValue>(opacity).value;
    const float sizeScale = std::get<FloatValue>(size).value;
    const float offF = std::get<FloatValue>(offsetForward).value;
    const float offS = std::get<FloatValue>(offsetSide).value;
    const float offU = std::get<FloatValue>(offsetUp).value;
    const Vec3 lpPos = lp->getPos();
    const bool tintOn = std::get<BoolValue>(tintShield);
    const bool imageOn = !tintOn || std::get<BoolValue>(imageFallback);

    std::vector<Panel> fresh;
    std::vector<std::pair<const SDK::Actor*, bool>> freshTints; // Whose shield to recolor: actor, disabled (else raised).

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
        bool broken = false;
        bool raised = false;
        float alpha = opacityValue;
        switch (state) {
        case ShieldState::State::Broken:
            broken = true; // A broken shield is lowered, whatever the player's hand is doing.
            break;
        case ShieldState::State::Blocking:
            if (!std::get<BoolValue>(markRaised)) continue; // A resource pack greens the real shield instead.
            raised = true;
            break;
        case ShieldState::State::Ready:
            if (!tinted) continue;
            alpha *= 0.6f;
            break;
        default:
            continue;
        }

        // Ask for the real shield to be recolored. The image below is then only a backup for a shield that doesn't get
        // recolored (the overlay hook records which ones did, see onRenderOverlay).
        if (tintOn && (broken || raised)) {
            freshTints.emplace_back(p.actor, broken);
            if (!imageOn) continue;
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

        // A raised shield is in front of the body: from behind the player it isn't visible.
        if (raised && !p.self) {
            const float toCamX = lpPos.x - p.feet.x, toCamZ = lpPos.z - p.feet.z;
            if (toCamX * forward.x + toCamZ * forward.z < -0.2f) continue;
        }

        const Vec3 w = scale(across, halfWidth);
        const Vec3 h { 0.f, halfHeight, 0.f };
        const Vec3 a = { center.x - w.x - h.x, center.y - w.y - h.y, center.z - w.z - h.z };
        const Vec3 b = { center.x + w.x - h.x, center.y + w.y - h.y, center.z + w.z - h.z };
        const Vec3 c = { center.x + w.x + h.x, center.y + w.y + h.y, center.z + w.z + h.z };
        const Vec3 d = { center.x - w.x + h.x, center.y - w.y + h.y, center.z - w.z + h.z };

        const float dx = center.x - lpPos.x, dy = center.y - lpPos.y, dz = center.z - lpPos.z;
        fresh.push_back({ a, b, c, d, std::sqrt(dx * dx + dy * dy + dz * dz), alpha, broken, p.actor });
    }

    {
        std::lock_guard panelLock(panelMutex);
        panels.swap(fresh);
        panelsTime = now;
    }
    {
        std::lock_guard lock(tintMutex);
        tintTargets.clear();
        for (auto const& [actor, isBroken] : freshTints) tintTargets[actor] = isBroken;
        for (auto it = tintedAt.begin(); it != tintedAt.end();) {
            it = now - it->second > std::chrono::seconds(2) ? tintedAt.erase(it) : std::next(it);
        }
        anyTint.store(!tintTargets.empty(), std::memory_order_relaxed);
    }
    tracker.prune(now);
}

void ShieldIndicator::onRenderOverlay(Event& evG) {
    auto* ci = SDK::ClientInstance::get();
    if (!ci || !ci->minecraftGame || !ci->minecraftGame->isCursorGrabbed()) return; // Not in a menu.

    std::vector<Panel> toDraw;
    {
        std::lock_guard panelLock(panelMutex);
        // The panels come from this frame's world render; don't keep drawing old ones if that stops running.
        if (std::chrono::steady_clock::now() - panelsTime > std::chrono::milliseconds(200)) return;
        toDraw = this->panels;
    }
    if (toDraw.empty()) return;

    auto& ev = reinterpret_cast<RenderOverlayEvent&>(evG);
    ID2D1DeviceContext* ctx = ev.getDeviceContext();
    if (!ctx) return;

    ID2D1Bitmap* working = Latite::getAssets().shieldOn.getBitmap();
    ID2D1Bitmap* disabled = Latite::getAssets().shieldOff.getBitmap();
    if (!working || !disabled) return;

    // Far to near, so a closer shield is drawn over a further one.
    std::sort(toDraw.begin(), toDraw.end(), [](Panel const& l, Panel const& r) { return l.distance > r.distance; });

    // Shields that were recolored this frame don't need the image on top.
    std::unordered_map<const SDK::Actor*, std::chrono::steady_clock::time_point> recolored;
    if (std::get<BoolValue>(tintShield)) {
        std::lock_guard lock(tintMutex);
        recolored = tintedAt;
    }
    const auto drawTime = std::chrono::steady_clock::now();

    D2D1::Matrix3x2F oldTransform;
    ctx->GetTransform(&oldTransform);

    for (auto const& panel : toDraw) {
        if (auto done = recolored.find(panel.actor);
            done != recolored.end() && drawTime - done->second < std::chrono::milliseconds(250)) {
            continue;
        }

        const auto a = WorldToScreen::convert(panel.a);
        const auto b = WorldToScreen::convert(panel.b);
        const auto c = WorldToScreen::convert(panel.c);
        const auto d = WorldToScreen::convert(panel.d);
        if (!a || !b || !c || !d) continue; // Partly behind the camera.

        ID2D1Bitmap* bitmap = panel.broken ? disabled : working;
        const D2D1_SIZE_F size = bitmap->GetSize();
        if (size.width <= 0.f || size.height <= 0.f) continue;

        // The image's top-left corner goes to the panel's top-left (d), its top-right to c and its bottom-left to a.
        // That is a skewed rectangle, which is how a flat panel looks from the side, so the shield turns with the player.
        const D2D1::Matrix3x2F place(
            (c->x - d->x) / size.width, (c->y - d->y) / size.width,
            (a->x - d->x) / size.height, (a->y - d->y) / size.height,
            d->x, d->y);
        ctx->SetTransform(place * oldTransform);

        const D2D1_RECT_F whole = D2D1::RectF(0.f, 0.f, size.width, size.height);
        ctx->DrawBitmap(bitmap, &whole, panel.alpha);
    }

    ctx->SetTransform(oldTransform);
}
