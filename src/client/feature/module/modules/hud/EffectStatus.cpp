#include "pch.h"
#include "EffectStatus.h"

#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/entity/component/MobEffectsComponent.h"
#include "mc/common/world/actor/player/Player.h"

#include <format>
#include <string>
#include <vector>

namespace {
    std::wstring effectName(int id) {
        static const wchar_t* names[] = {
            L"Unknown",       L"Speed",          L"Slowness",      L"Haste",         L"Mining Fatigue",
            L"Strength",      L"Instant Health", L"Instant Damage", L"Jump Boost",   L"Nausea",
            L"Regeneration",  L"Resistance",     L"Fire Resistance", L"Water Breathing", L"Invisibility",
            L"Blindness",     L"Night Vision",   L"Hunger",        L"Weakness",      L"Poison",
            L"Wither",        L"Health Boost",   L"Absorption",    L"Saturation",    L"Levitation",
            L"Fatal Poison",  L"Conduit Power",  L"Slow Falling",  L"Bad Omen",      L"Hero of the Village",
            L"Darkness",      L"Trial Omen",     L"Wind Charged",  L"Weaving",       L"Oozing",
            L"Infested",      L"Raid Omen",
        };
        constexpr int count = static_cast<int>(sizeof(names) / sizeof(names[0]));
        if (id >= 1 && id < count) return names[id];
        return std::format(L"Effect {}", id);
    }

    std::wstring roman(int level) {
        static const wchar_t* numerals[] = { L"", L"I", L"II", L"III", L"IV", L"V", L"VI", L"VII", L"VIII", L"IX", L"X" };
        if (level >= 1 && level <= 10) return numerals[level];
        return std::to_wstring(level);
    }

    std::wstring formatTime(int ticks) {
        if (ticks <= 0) return L"Inf";
        const int total = ticks / 20;
        return std::format(L"{}:{:02}", total / 60, total % 60);
    }

    std::wstring formatLine(SDK::EffectData const& e, bool level, bool time) {
        std::wstring line = effectName(e.id);
        if (level) line += L" " + roman(e.amplifier + 1);
        if (time) line += L" (" + formatTime(e.duration) + L")";
        return line;
    }
} // namespace

EffectStatus::EffectStatus()
    : TextModule("EffectStatus", LocalizeString::get("client.textmodule.effectStatus.name"),
                 LocalizeString::get("client.textmodule.effectStatus.desc"), HUD) {
    addSetting("showLevel", LocalizeString::get("client.textmodule.effectStatus.showLevel.name"),
               LocalizeString::get("client.textmodule.effectStatus.showLevel.desc"), showLevel);
    addSetting("showTime", LocalizeString::get("client.textmodule.effectStatus.showTime.name"),
               LocalizeString::get("client.textmodule.effectStatus.showTime.desc"), showTime);
    addSetting("debug", LocalizeString::get("client.textmodule.effectStatus.debug.name"),
               LocalizeString::get("client.textmodule.effectStatus.debug.desc"), debug);
}

std::wstringstream EffectStatus::text(bool isDefault, bool inEditor) {
    std::wstringstream out;
    const bool level = std::get<BoolValue>(showLevel);
    const bool time = std::get<BoolValue>(showTime);

    // In the HUD editor (and as the default preview) show sample lines so there is always something to move.
    if (isDefault || inEditor) {
        const SDK::EffectData samples[] = { { 1, 20 * 90, 1 }, { 5, 20 * 45, 1 }, { 10, 20 * 12, 0 } };
        bool firstLine = true;
        for (auto const& s : samples) {
            if (!firstLine) out << L"\n";
            out << formatLine(s, level, time);
            firstLine = false;
        }
        return out;
    }

    auto* ci = SDK::ClientInstance::get();
    auto* plr = ci ? ci->getLocalPlayer() : nullptr;
    if (!plr) return out;

    auto* comp = plr->tryGetComponent<SDK::MobEffectsComponent>();
    const bool debugMode = std::get<BoolValue>(debug);

    if (!comp) {
        if (debugMode) out << L"[debug] no MobEffectsComponent (normal while you have no effects)";
        return out;
    }

    const auto effects = comp->read();

    if (debugMode) {
        out << std::format(L"[debug] bytes=0x{:X} stride=0x{:X} effects={}\n", comp->byteSize(),
                           comp->detectStride(), effects.size());
        out << L"[debug] first element ints:";
        for (size_t i = 0; i < 12; ++i) out << std::format(L" {}", comp->rawInt(i));
        if (!effects.empty()) out << L"\n";
    }

    bool firstLine = true;
    for (auto const& e : effects) {
        if (!firstLine) out << L"\n";
        out << formatLine(e, level, time);
        firstLine = false;
    }
    return out;
}
