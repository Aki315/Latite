#include "pch.h"
#include "ArmorNametag.h"

#include "client/event/events/AttackEvent.h"
#include "client/event/events/UpdateEvent.h"
#include "mc/Addresses.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/network/packet/ActorEventPacket.h"
#include "mc/common/network/Packet.h"
#include "mc/common/world/Minecraft.h"
#include "mc/common/entity/component/ActorEquipmentComponent.h"
#include "mc/common/world/Item.h"
#include "mc/common/world/ItemStack.h"
#include "mc/common/world/ItemStackBase.h"
#include "mc/common/world/actor/Actor.h"
#include "mc/common/world/level/Level.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace {
using namespace ArmorNametagDetail;

enum class DisplayMode : int { Percentage = 0, Points = 1 };
enum class DurabilitySource : int { Actual = 0, Estimated = 1 };

// "\302\247" is the UTF-8 encoding of the section sign. Octal escapes stop after
// three digits, so a color code can follow directly without splitting the literal.
constexpr std::string_view kSectionSign = "\302\247";

// Everything this module appends begins with this marker ("\n§8[§7"). It is the
// single source of truth for both building and stripping the line.
constexpr std::string_view kArmorMarker = "\n\302\2478[\302\2477";

constexpr std::array<char, kSlotCount> kSlotLabels{'H', 'C', 'L', 'B'};
constexpr int kGreenAtOrAbove = 60;
constexpr int kYellowAtOrAbove = 25;

void addColor(std::string& text, char color) {
    text += kSectionSign;
    text += color;
}

// Reads what this client currently believes each armor slot holds.
ArmorSnapshot readArmor(SDK::Actor* actor) {
    ArmorSnapshot snapshot{};

    // Actor::getArmor() dereferences the equipment component internally, so guard it first.
    auto* equipment = actor->tryGetComponent<SDK::ActorEquipmentComponent>();
    if (!equipment || !equipment->armorContainer) return snapshot;

    for (int slot = 0; slot < kSlotCount; ++slot) {
        auto* stack = actor->getArmor(slot);
        if (!stack) continue;

        auto* item = stack->getItem();
        if (!item) continue; // Empty armor slot.

        const int maxDamage = item->getMaxDamage();
        if (maxDamage <= 0) continue; // Item has no durability value.

        const int damage = static_cast<int>(stack->getDamageValue());
        snapshot[slot] = ArmorDurability{std::clamp(maxDamage - damage, 0, maxDamage), maxDamage};
    }
    return snapshot;
}

std::string makeArmorLine(ArmorSnapshot const& armor, SlotLosses const& losses, DisplayMode mode) {
    std::string text(kArmorMarker);
    text.reserve(56);

    for (int slot = 0; slot < kSlotCount; ++slot) {
        if (slot != 0) text += ' ';

        const auto& durability = armor[slot];
        if (!durability) {
            addColor(text, '7');
            text += kSlotLabels[slot];
            text += "--";
            continue;
        }

        const int shown = std::clamp(durability->remaining - losses[slot], 0, durability->maxDamage);
        // Integer division rounds down, so a slightly damaged piece never reads "100%".
        const int percent = shown * 100 / durability->maxDamage;

        addColor(text, percent >= kGreenAtOrAbove ? 'a' : percent >= kYellowAtOrAbove ? 'e' : 'c');
        text += kSlotLabels[slot];
        if (losses[slot] > 0) text += '~'; // Only marked when the number is an estimate.
        if (mode == DisplayMode::Points) {
            text += std::to_string(shown);
        } else {
            text += std::to_string(percent);
            text += '%';
        }
    }

    addColor(text, '8');
    text += ']';
    return text;
}

// Removes only this module's line, preserving other module-owned nametag lines.
std::string stripArmorLine(std::string_view tag) {
    const auto marker = tag.find(kArmorMarker);
    if (marker == std::string_view::npos) return std::string(tag);

    const auto nextLine = tag.find('\n', marker + kArmorMarker.size());
    std::string result(tag.substr(0, marker));
    if (nextLine != std::string_view::npos) result.append(tag.substr(nextLine));
    return result;
}

// These signatures are resolved by Latite at startup. Avoid calling SDK wrappers if one failed.
bool nameTagApiReady() {
    return Signatures::Actor_getNameTag.result && Signatures::Actor_setNameTag.result;
}

// Reads a float slider, tolerating a config that holds some other value type.
float getFloatOrDefault(ValueType const& value, float fallback) {
    if (!std::holds_alternative<FloatValue>(value)) return fallback;
    return std::get<FloatValue>(value).value;
}

auto* currentLevel() {
    auto* client = SDK::ClientInstance::get();
    return client && client->minecraft ? client->minecraft->getLevel() : nullptr;
}
} // namespace

ArmorNametag::ArmorNametag()
    : Module("ArmorNametag", LocalizeString::get("client.module.armorNametag.name"),
             LocalizeString::get("client.module.armorNametag.desc"), GAME) {
    durabilityDisplayMode.addEntry({static_cast<int>(DisplayMode::Percentage),
                                    LocalizeString::get("client.module.armorNametag.displayMode.percentage.name"),
                                    LocalizeString::get("client.module.armorNametag.displayMode.percentage.desc")});
    durabilityDisplayMode.addEntry({static_cast<int>(DisplayMode::Points),
                                    LocalizeString::get("client.module.armorNametag.displayMode.points.name"),
                                    LocalizeString::get("client.module.armorNametag.displayMode.points.desc")});
    addEnumSetting("displayMode", LocalizeString::get("client.module.armorNametag.displayMode.name"),
                   LocalizeString::get("client.module.armorNametag.displayMode.desc"), durabilityDisplayMode);

    durabilitySource.addEntry({static_cast<int>(DurabilitySource::Actual),
                               LocalizeString::get("client.module.armorNametag.durabilitySource.actual.name"),
                               LocalizeString::get("client.module.armorNametag.durabilitySource.actual.desc")});
    durabilitySource.addEntry({static_cast<int>(DurabilitySource::Estimated),
                               LocalizeString::get("client.module.armorNametag.durabilitySource.estimated.name"),
                               LocalizeString::get("client.module.armorNametag.durabilitySource.estimated.desc")});
    addEnumSetting("durabilitySource", LocalizeString::get("client.module.armorNametag.durabilitySource.name"),
                   LocalizeString::get("client.module.armorNametag.durabilitySource.desc"), durabilitySource);

    addSliderSetting("maxDistance", LocalizeString::get("client.module.armorNametag.maxDistance.name"),
                     LocalizeString::get("client.module.armorNametag.maxDistance.desc"), maxDistance,
                     FloatValue(0.f), FloatValue(128.f), FloatValue(1.f));

    addSliderSetting("weaponDamage", LocalizeString::get("client.module.armorNametag.weaponDamage.name"),
                     LocalizeString::get("client.module.armorNametag.weaponDamage.desc"), weaponDamage, FloatValue(1.f),
                     FloatValue(12.f), FloatValue(0.5f));
    addSetting("assumeSharpness", LocalizeString::get("client.module.armorNametag.assumeSharpness.name"),
               LocalizeString::get("client.module.armorNametag.assumeSharpness.desc"), assumeSharpness);
    addSliderSetting("strengthLevel", LocalizeString::get("client.module.armorNametag.strengthLevel.name"),
                     LocalizeString::get("client.module.armorNametag.strengthLevel.desc"), strengthLevel, FloatValue(0.f),
                     FloatValue(4.f), FloatValue(1.f));
    addSetting("assumeProtection", LocalizeString::get("client.module.armorNametag.assumeProtection.name"),
               LocalizeString::get("client.module.armorNametag.assumeProtection.desc"), assumeProtection);
    addSetting("assumeUnbreaking", LocalizeString::get("client.module.armorNametag.assumeUnbreaking.name"),
               LocalizeString::get("client.module.armorNametag.assumeUnbreaking.desc"), assumeUnbreaking);
    addSliderSetting("wearScale", LocalizeString::get("client.module.armorNametag.wearScale.name"),
                     LocalizeString::get("client.module.armorNametag.wearScale.desc"), wearScale, FloatValue(0.5f),
                     FloatValue(3.f), FloatValue(0.05f));
    addSetting("debugLog", LocalizeString::get("client.module.armorNametag.debugLog.name"),
               LocalizeString::get("client.module.armorNametag.debugLog.desc"), debugLog);

    listen<UpdateEvent>((EventListenerFunc)&ArmorNametag::onUpdate);
    listen<PacketReceiveEvent>((EventListenerFunc)&ArmorNametag::onPacketReceive);
    listen<AttackEvent>((EventListenerFunc)&ArmorNametag::onAttack);
}

void ArmorNametag::onPacketReceive(Event& evG) {
    auto& ev = reinterpret_cast<PacketReceiveEvent&>(evG);
    auto* pkt = ev.getPacket();
    if (!pkt) return;

    // When the server sends a player's whole armor again, the estimate for that player starts over from it. This
    // also handles re-equipping the same armor with the same durability (where comparing only the numeric damage
    // value is ambiguous).
    if (static_cast<DurabilitySource>(durabilitySource.getSelectedKey()) == DurabilitySource::Estimated) {
        if (const uint64_t id = ArmorEstimate::syncArmorPacket(pkt); id != 0) trackers.noteArmorSync(id);
    }
}

// Remember what you swung at them with, so the next health drop can be turned into armor wear.
void ArmorNametag::onAttack(Event& evG) {
    auto& ev = reinterpret_cast<AttackEvent&>(evG);
    auto* actor = ev.getActor();
    if (!actor || !actor->isPlayer()) return;

    auto* client = SDK::ClientInstance::get();
    trackers.noteOwnHit(actor->getRuntimeID(), ArmorEstimate::heldWeapon(client ? client->getLocalPlayer() : nullptr));
}

void ArmorNametag::onUpdate(Event&) {
    if (!nameTagApiReady() || !Signatures::ItemStackBase_getDamageValue.result) return;

    auto* client = SDK::ClientInstance::get();
    auto* level = currentLevel();
    auto* localPlayer = client ? client->getLocalPlayer() : nullptr;
    if (!level || !localPlayer) return;

    const auto mode = static_cast<DisplayMode>(durabilityDisplayMode.getSelectedKey());
    const bool estimating =
        static_cast<DurabilitySource>(durabilitySource.getSelectedKey()) == DurabilitySource::Estimated;
    const float maxDist = getFloatOrDefault(maxDistance, 32.f); // <= 0 means unlimited.
    const auto localPos = localPlayer->getPos();

    // What the estimate assumes about the armor, and the damage of a hit you did not throw.
    ArmorEstimate::Assumptions assume;
    assume.protection = std::get<BoolValue>(assumeProtection);
    assume.sharpness = std::get<BoolValue>(assumeSharpness);
    assume.unbreaking = std::get<BoolValue>(assumeUnbreaking);
    assume.strength = static_cast<int>(std::lround(getFloatOrDefault(strengthLevel, 0.f)));
    const float otherHitDamage = getFloatOrDefault(weaponDamage, 7.f) + (assume.sharpness ? 3.f : 0.f) +
                                 3.f * static_cast<float>(assume.strength);
    assume.wearScale = getFloatOrDefault(wearScale, 1.f);
    assume.debug = std::get<BoolValue>(debugLog);

    // Follow every player's armor, also the ones out of range of the nametag line.
    if (estimating) trackers.tick(level, localPlayer, assume, otherHitDamage);
    else trackers.forget();

    for (SDK::Actor* actor : level->getRuntimeActorList()) {
        if (!actor || actor == localPlayer || !actor->isPlayer()) continue;

        const std::string currentTag = actor->getNameTag();
        const std::string baseTag = stripArmorLine(currentTag);

        // Re-derive from the live tag every update, so legitimate name changes
        // from the server are picked up and our line is never stacked twice.
        std::string desiredTag(baseTag);

        // Don't turn a deliberately empty (hidden) nametag into a visible one,
        // don't leave a frozen readout on players who became invisible, and drop
        // the line once a player is farther away than the configured range.
        const bool inRange = maxDist <= 0.f || actor->getPos().distance(localPos) <= maxDist;
        if (!baseTag.empty() && !actor->isInvisible() && inRange) {
            const ArmorSnapshot armor = readArmor(actor);

            SlotLosses losses{};
            if (estimating) {
                if (auto const* tracker = trackers.find(actor->getRuntimeID())) {
                    for (int slot = 0; slot < kSlotCount; ++slot) {
                        const int guess = tracker->estimatedDamage(slot);
                        if (guess < 0 || !armor[slot]) continue;
                        const int serverDamage = armor[slot]->maxDamage - armor[slot]->remaining;
                        losses[slot] = std::max(0, guess - serverDamage);
                    }
                }
            }
            desiredTag += makeArmorLine(armor, losses, mode);
        }

        if (desiredTag != currentTag) {
            actor->setNameTag(&desiredTag);
        }
    }
}

void ArmorNametag::onDisable() {
    trackers.forget();

    auto* level = currentLevel();
    if (!level || !nameTagApiReady()) return;

    for (SDK::Actor* actor : level->getRuntimeActorList()) {
        if (!actor || !actor->isPlayer()) continue;

        const std::string currentTag = actor->getNameTag();
        const std::string baseTag = stripArmorLine(currentTag);
        if (baseTag.size() == currentTag.size()) continue; // Not ours / nothing to remove.

        std::string restoredTag(baseTag);
        actor->setNameTag(&restoredTag);
    }
}
