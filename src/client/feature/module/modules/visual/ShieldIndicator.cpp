#include "pch.h"
#include "ShieldIndicator.h"

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
#include <unordered_set>
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

    struct Seen {
        SDK::Actor* actor;
        uint64_t id;
        Vec3 feet;
        bool holds;
        bool self;
    };

    // How a disabled shield is told to the resource pack. The pack's render controller can read the player's native
    // queries (query.is_using_item is what turns a raised shield green), but it can't read anything custom: a Molang
    // variable it doesn't know makes the shield invisible. So a disabled shield sets the "saddled" status flag on the
    // player, which the pack reads as query.is_saddled. Nothing renders or behaves differently on a player for that
    // flag, and it is the module that sets and clears it, every frame (the server resends the flags now and then).
    constexpr int kFlagDisabledMarker = 8; // ActorFlags::SADDLED

    enum StateValue : int {
        StateNormal = 0,
        StateBlocking = 1,
        StateDisabled = 2,
    };

    void publishState(SDK::Actor* actor, int value) {
        const bool disabled = value == StateDisabled;
        if (actor->getStatusFlag(kFlagDisabledMarker) != disabled) actor->setStatusFlag(kFlagDisabledMarker, disabled);
    }
} // namespace

ShieldIndicator::ShieldIndicator()
    : Module("ShieldIndicator", LocalizeString::get("client.module.shieldIndicator.name"),
             LocalizeString::get("client.module.shieldIndicator.desc"), GAME) {
    addSliderSetting("cooldown", LocalizeString::get("client.module.shieldIndicator.cooldown.name"),
                     LocalizeString::get("client.module.shieldIndicator.cooldown.desc"), cooldown, FloatValue(1.f),
                     FloatValue(15.f), FloatValue(0.1f));
    addSetting("includeSelf", LocalizeString::get("client.module.shieldIndicator.includeSelf.name"),
               LocalizeString::get("client.module.shieldIndicator.includeSelf.desc"), includeSelf);
    addSetting("debugLog", LocalizeString::get("client.module.shieldIndicator.debugLog.name"),
               LocalizeString::get("client.module.shieldIndicator.debugLog.desc"), debugLog);

    // Also called while the module is off, so that it can put the shields back to normal.
    Eventing::get().listen<RenderLevelEvent, &ShieldIndicator::onRenderLevel>(this, 0, true);
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
    // The shields are put back to normal by the next world render (it runs on the render thread, where the players
    // are safe to touch).
}

void ShieldIndicator::onRenderLevel(RenderLevelEvent&) {
    auto* ci = SDK::ClientInstance::get();
    if (!ci || !ci->minecraft || !ci->minecraft->timer) return;
    auto* level = ci->minecraft->getLevel();
    auto* lp = ci->getLocalPlayer();
    if (!level || !lp) {
        published.clear(); // The world is gone, and the players with it.
        return;
    }

    if (!announced && isEnabled()) {
        announced = true;
        Logger::Info("[ShieldIndicator] running (a disabled shield is marked with the saddled flag for the resource pack)");
    }

    // Switched off: every shield this module touched goes back to its normal texture, once.
    if (!isEnabled()) {
        if (published.empty()) return;
        for (SDK::Actor* actor : level->getRuntimeActorList()) {
            if (actor && actor->isPlayer() && published.contains(actor->getRuntimeID())) publishState(actor, StateNormal);
        }
        published.clear();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool withSelf = std::get<BoolValue>(includeSelf);

    // ---- Everyone with a shield, where they are.
    std::vector<Seen> players;
    std::vector<ShieldState::Candidate> holders;
    const float partial = ci->minecraft->timer->alpha;
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

        const Vec3 posOld = actor->getPosOld();
        const Vec3 posNow = actor->getPos();
        const Vec3 pos { std::lerp(posOld.x, posNow.x, partial), std::lerp(posOld.y, posNow.y, partial),
                         std::lerp(posOld.z, posNow.z, partial) };
        const float eyeOffset = posNow.y - actor->getBoundingBox().lower.y; // getPos() is at the eyes.
        seen.feet = { pos.x, pos.y - eyeOffset, pos.z };

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

    // ---- Tell each shield what state it is in. The resource pack's render controller picks the texture from this.
    std::unordered_set<uint64_t> present;
    for (auto const& p : players) {
        present.insert(p.id);
        const bool usingItem = p.actor->getStatusFlag(kFlagUsingItem) || p.actor->getStatusFlag(kFlagBlocking);
        tracker.observe(p.id, p.holds, usingItem, now);

        const bool wanted = p.holds && (!p.self || withSelf);
        const auto known = published.find(p.id);

        if (!wanted) {
            if (known != published.end()) { // Put the shield back to normal, once.
                publishState(p.actor, StateNormal);
                published.erase(known);
            }
            continue;
        }

        int value = StateNormal;
        switch (tracker.state(p.id, now)) {
        case ShieldState::State::Broken:
            value = StateDisabled; // Disabled, whatever the player's hand is doing.
            break;
        case ShieldState::State::Blocking:
            value = StateBlocking;
            break;
        default:
            break;
        }

        publishState(p.actor, value);
        if (debug && (known == published.end() || known->second != value)) {
            Logger::Info("[ShieldIndicator] shield state of player {} -> {} ({})", p.id, value,
                         value == StateDisabled ? "disabled" : value == StateBlocking ? "blocking" : "normal");
        }
        published[p.id] = value;
    }

    // Players that left: nothing to put back, their entries just go.
    std::erase_if(published, [&](auto const& entry) { return !present.contains(entry.first); });
    tracker.prune(now);
}
