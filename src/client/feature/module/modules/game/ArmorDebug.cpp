#include "pch.h"
#include "ArmorDebug.h"

#include "client/event/events/AttackEvent.h"
#include "client/event/events/UpdateEvent.h"
#include "mc/Addresses.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/entity/component/ActorEquipmentComponent.h"
#include "mc/common/network/Packet.h"
#include "mc/common/network/packet/ActorEventPacket.h"
#include "mc/common/world/Item.h"
#include "mc/common/world/ItemStack.h"
#include "mc/common/world/ItemStackBase.h"
#include "mc/common/world/Minecraft.h"
#include "mc/common/world/actor/Actor.h"
#include "mc/common/world/level/Level.h"
#include "util/ArmorEstimate.h"
#include "util/Util.h"

#include <format>

namespace {
    // Packets that start with the target's runtime id and are about equipment, armor or health.
    // These numbers are from the 1.26.x protocol; the Packet class here only names a few of them.
    constexpr uint8_t kMobEquipment = 0x1F;
    constexpr uint8_t kMobArmorEquipment = 0x20;
    constexpr uint8_t kUpdateAttributes = 0x1D;
    constexpr uint8_t kInventoryContent = 0x31;
    constexpr uint8_t kInventorySlot = 0x32;
    constexpr uint8_t kSetActorData = 0x27;

    struct RuntimeIdView : SDK::Packet {
        uint64_t runtimeID;
    };

    bool wantedPacket(uint8_t id) {
        switch (id) {
        case kMobEquipment:
        case kMobArmorEquipment:
        case kUpdateAttributes:
        case kInventoryContent:
        case kInventorySlot:
        case kSetActorData:
        case static_cast<uint8_t>(SDK::PacketID::ACTOR_EVENT):
        case static_cast<uint8_t>(SDK::PacketID::TEXT):
        case static_cast<uint8_t>(SDK::PacketID::SET_TITLE):
            return true;
        default:
            return false;
        }
    }

    // Makes a nametag readable on one line: newlines and control characters are written out.
    std::string escaped(std::string const& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (unsigned char c : s) {
            if (c == '\n') out += "\\n";
            else if (c == '\r') continue;
            else if (c < 0x20) out += std::format("\\x{:02X}", c);
            else out += static_cast<char>(c);
        }
        return out;
    }

    std::string hexDump(const void* p, size_t n) {
        std::string out;
        std::string bytes(n, '\0');
        if (!SDK::BaseAttributeMap::safeRead(p, bytes.data(), n)) return "(unreadable)";
        for (size_t i = 0; i < n; ++i) {
            if (i && i % 16 == 0) out += " | ";
            out += std::format("{:02X} ", static_cast<unsigned char>(bytes[i]));
        }
        return out;
    }

    float readFloat(ValueType const& v, float fallback) {
        return std::holds_alternative<FloatValue>(v) ? std::get<FloatValue>(v).value : fallback;
    }
} // namespace

ArmorDebug::ArmorDebug()
    : Module("ArmorDebug", LocalizeString::get("client.module.armorDebug.name"),
             LocalizeString::get("client.module.armorDebug.desc"), GAME) {
    addSliderSetting("logDistance", LocalizeString::get("client.module.armorDebug.logDistance.name"),
                     LocalizeString::get("client.module.armorDebug.logDistance.desc"), logDistance, FloatValue(0.f),
                     FloatValue(128.f), FloatValue(1.f));
    addSliderSetting("interval", LocalizeString::get("client.module.armorDebug.interval.name"),
                     LocalizeString::get("client.module.armorDebug.interval.desc"), interval, FloatValue(1.f),
                     FloatValue(40.f), FloatValue(1.f));
    addSetting("logPackets", LocalizeString::get("client.module.armorDebug.logPackets.name"),
               LocalizeString::get("client.module.armorDebug.logPackets.desc"), logPackets);
    addSetting("dumpPackets", LocalizeString::get("client.module.armorDebug.dumpPackets.name"),
               LocalizeString::get("client.module.armorDebug.dumpPackets.desc"), dumpPackets, "logPackets"_istrue);

    listen<UpdateEvent>((EventListenerFunc)&ArmorDebug::onUpdate);
    listen<PacketReceiveEvent>((EventListenerFunc)&ArmorDebug::onPacketReceive);
    listen<AttackEvent>((EventListenerFunc)&ArmorDebug::onAttack);
}

void ArmorDebug::write(std::string const& line) {
    std::lock_guard lock(fileMutex);
    if (!file.is_open()) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    file << std::format("[{:>8}ms] ", ms) << line << "\n";
    file.flush();
}

void ArmorDebug::onEnable() {
    {
        std::lock_guard lock(fileMutex);
        auto dir = util::GetLatitePath() / "Logs";
        std::filesystem::create_directories(dir);
        file.open(dir / "ArmorDebug.log", std::ios::trunc);
        started = std::chrono::steady_clock::now();
    }
    tick = 0;
    lastState.clear();
    lastHealth.clear();
    packetCounts.clear();
    write("=== Armor Debug started. Turn Latite's Armor Nametag off, so the nametags below are untouched. ===");
    write("Columns: PLAYER id name | TAG raw nametag | ARMOR slot item damage/max (aux, nbt) | HP health+golden hearts");
}

void ArmorDebug::onDisable() {
    {
        std::string counts;
        {
            std::lock_guard lock(fileMutex);
            for (auto const& [id, n] : packetCounts) counts += std::format(" 0x{:02X}={}", id, n);
        }
        write("=== Packets seen by id:" + (counts.empty() ? std::string(" none") : counts));
    }
    write("=== Armor Debug stopped ===");
    std::lock_guard lock(fileMutex);
    file.close();
}

void ArmorDebug::onAttack(Event& evG) {
    auto& ev = reinterpret_cast<AttackEvent&>(evG);
    auto* actor = ev.getActor();
    if (!actor) return;

    auto* client = SDK::ClientInstance::get();
    auto* me = client ? client->getLocalPlayer() : nullptr;
    const auto weapon = ArmorEstimate::heldWeapon(me);
    std::string name = actor->isPlayer() ? static_cast<SDK::Player*>(actor)->playerName : actor->getEntityTypeName();
    write(std::format("YOU HIT id={} name={} weaponBase={} sharpable={}", actor->getRuntimeID(), name, weapon.base,
                      weapon.sharpable));
}

void ArmorDebug::onPacketReceive(Event& evG) {
    if (!std::get<BoolValue>(logPackets)) return;

    auto& ev = reinterpret_cast<PacketReceiveEvent&>(evG);
    auto* pkt = ev.getPacket();
    if (!pkt) return;

    const auto id = static_cast<uint8_t>(pkt->getID());
    {
        std::lock_guard lock(fileMutex);
        ++packetCounts[id];
    }
    if (!wantedPacket(id)) return;

    std::string line = std::format("PACKET 0x{:02X} {}", id, pkt->getName());
    if (id == static_cast<uint8_t>(SDK::PacketID::ACTOR_EVENT)) {
        auto* p = static_cast<SDK::ActorEventPacket*>(pkt);
        line += std::format(" runtimeId={} event={} data={}", p->runtimeID, static_cast<int>(p->eventID), p->data);
    } else if (id == kMobArmorEquipment || id == kMobEquipment || id == kSetActorData || id == kUpdateAttributes) {
        line += std::format(" runtimeId={}", reinterpret_cast<RuntimeIdView*>(pkt)->runtimeID);
    }
    write(line);

    if (std::get<BoolValue>(dumpPackets) && (id == kMobArmorEquipment || id == kMobEquipment || id == kInventorySlot)) {
        write("  BYTES " + hexDump(pkt, 0x100));
    }
}

void ArmorDebug::onUpdate(Event&) {
    ++tick;
    if (tick % static_cast<uint64_t>(std::max(1.f, readFloat(interval, 5.f))) != 0) return;

    auto* client = SDK::ClientInstance::get();
    auto* level = client && client->minecraft ? client->minecraft->getLevel() : nullptr;
    auto* me = client ? client->getLocalPlayer() : nullptr;
    if (!level || !me) return;

    const float maxDist = readFloat(logDistance, 24.f);
    const auto myPos = me->getPos();

    for (SDK::Actor* actor : level->getRuntimeActorList()) {
        if (!actor || actor == me || !actor->isPlayer()) continue;
        if (maxDist > 0.f && actor->getPos().distance(myPos) > maxDist) continue;

        const uint64_t id = actor->getRuntimeID();
        auto* player = static_cast<SDK::Player*>(actor);
        const auto vitals = ArmorEstimate::readVitals(actor);
        const float hp = vitals.health < 0.f ? -1.f : vitals.health + vitals.absorption;

        // What this player looks like to the client right now, as one string. Written only when it changes.
        std::string state = std::format("PLAYER id={} name={}\n   TAG '{}'\n", id, player->playerName,
                                        escaped(actor->getNameTag()));
        if (Signatures::ItemStackBase_getDamageValue.result) {
            static constexpr const char* kSlots[4] = { "head", "chest", "legs", "feet" };
            if (auto* equipment = actor->tryGetComponent<SDK::ActorEquipmentComponent>(); equipment && equipment->armorContainer) {
                for (int slot = 0; slot < 4; ++slot) {
                    auto* stack = actor->getArmor(slot);
                    auto* item = stack ? stack->getItem() : nullptr;
                    if (!item) {
                        state += std::format("   ARMOR {:<5} -\n", kSlots[slot]);
                        continue;
                    }
                    state += std::format("   ARMOR {:<5} {} damage={}/{} aux={} nbt={}\n", kSlots[slot],
                                         item->namespacedId.getString(), static_cast<int>(stack->getDamageValue()),
                                         item->getMaxDamage(), static_cast<int>(stack->aux), stack->tag ? "yes" : "no");
                }
            }
        }
        state += std::format("   HP {:.2f}  invulnerableTime={}", hp, actor->invulnerableTime);

        auto it = lastState.find(id);
        // Health on its own changes a lot; the line is written when anything else changes, or when it drops.
        const float before = lastHealth.count(id) ? lastHealth[id] : hp;
        const bool dropped = hp >= 0.f && before >= 0.f && hp < before - 0.05f;
        std::string withoutHp = state.substr(0, state.rfind("   HP"));
        const bool changed = it == lastState.end() || it->second != withoutHp;
        lastHealth[id] = hp;
        if (!changed && !dropped) continue;

        lastState[id] = withoutHp;
        write(state + (dropped ? std::format("  <-- HEALTH DROP {:.2f} -> {:.2f}", before, hp) : std::string {}));
    }
}
