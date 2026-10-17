#include "pch.h"
#include "EffectStatus.h"

#include "client/event/events/RenderLayerEvent.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/gui/ScreenView.h"
#include "mc/common/client/gui/controls/UIControl.h"
#include "mc/common/client/gui/controls/VisualTree.h"
#include "mc/common/entity/component/MobEffectsComponent.h"
#include "mc/common/world/actor/player/Player.h"

#include <algorithm>
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

    // File name (without extension) of the game's own effect icon, textures/ui/<name>, for an effect id.
    // Returns nullptr when the game has no icon for it (instant effects never stay in the list, and saturation has
    // none), in which case that line just gets an empty icon slot so the text stays aligned.
    const char* iconName(int id) {
        switch (id) {
        case 1: return "speed_effect";
        case 2: return "slowness_effect";
        case 3: return "haste_effect";
        case 4: return "mining_fatigue_effect";
        case 5: return "strength_effect";
        case 8: return "jump_boost_effect";
        case 9: return "nausea_effect";
        case 10: return "regeneration_effect";
        case 11: return "resistance_effect";
        case 12: return "fire_resistance_effect";
        case 13: return "water_breathing_effect";
        case 14: return "invisibility_effect";
        case 15: return "blindness_effect";
        case 16: return "night_vision_effect";
        case 17: return "hunger_effect";
        case 18: return "weakness_effect";
        case 19: return "poison_effect";
        case 20: return "wither_effect";
        case 21: return "health_boost_effect";
        case 22: return "absorption_effect";
        case 24: return "levitation_effect";
        case 25: return "poison_effect"; // fatal poison has no icon of its own
        case 26: return "conduit_power_effect";
        case 27: return "slow_falling_effect";
        case 28: return "bad_omen_effect";
        case 29: return "village_hero_effect";
        case 30: return "darkness_effect";
        case 31: return "trial_omen_effect";
        case 32: return "wind_charged_effect";
        case 33: return "weaving_effect";
        case 34: return "oozing_effect";
        case 35: return "infested_effect";
        case 36: return "raid_omen_effect";
        default: return nullptr;
        }
    }

    // Reads the player's effects. In the HUD editor (and as the default preview) it returns sample effects so there is
    // always something to move. The debug text, when asked for, is appended to *dbg.
    std::vector<SDK::EffectData> collectEffects(bool sample, bool debugMode, std::wstringstream* dbg) {
        if (sample) {
            return { { 1, 20 * 90, 1 }, { 5, 20 * 45, 1 }, { 10, 20 * 12, 0 } };
        }

        auto* ci = SDK::ClientInstance::get();
        auto* plr = ci ? ci->getLocalPlayer() : nullptr;
        if (!plr) return {};

        auto* comp = plr->tryGetComponent<SDK::MobEffectsComponent>();
        if (!comp) {
            if (debugMode && dbg) *dbg << L"[debug] no MobEffectsComponent (normal while you have no effects)";
            return {};
        }

        auto effects = comp->read();

        if (debugMode && dbg) {
            const auto layout = comp->detectLayout();
            *dbg << std::format(L"[debug] bytes=0x{:X} stride=0x{:X} ampOff=0x{:X} active={} effects={}\n",
                                comp->byteSize(), layout.stride, layout.ampOffset, layout.active, effects.size());
            *dbg << L"[debug] first element ints:";
            for (size_t i = 0; i < 12; ++i) *dbg << std::format(L" {}", comp->rawInt(i));
            *dbg << L"\n[debug] id+duration candidates:";
            const auto hits = comp->findCandidates(6);
            if (hits.empty()) *dbg << L" none";
            for (size_t off : hits) {
                *dbg << std::format(L" +0x{:X}(id={} dur={})", off, comp->rawInt(off / 4), comp->rawInt(off / 4 + 1));
            }
            if (!hits.empty()) {
                *dbg << L"\n[debug] ints at first candidate:";
                for (size_t i = 0; i < 12; ++i) *dbg << std::format(L" {}", comp->rawInt(hits[0] / 4 + i));
            }
        }
        return effects;
    }
} // namespace

EffectStatus::EffectStatus()
    : TextModule("EffectStatus", LocalizeString::get("client.textmodule.effectStatus.name"),
                 LocalizeString::get("client.textmodule.effectStatus.desc"), HUD) {
    addSetting("showLevel", LocalizeString::get("client.textmodule.effectStatus.showLevel.name"),
               LocalizeString::get("client.textmodule.effectStatus.showLevel.desc"), showLevel);
    addSetting("showTime", LocalizeString::get("client.textmodule.effectStatus.showTime.name"),
               LocalizeString::get("client.textmodule.effectStatus.showTime.desc"), showTime);
    addSetting("showIcons", LocalizeString::get("client.textmodule.effectStatus.showIcons.name"),
               LocalizeString::get("client.textmodule.effectStatus.showIcons.desc"), showIcons);
    addSliderSetting("iconSize", LocalizeString::get("client.textmodule.effectStatus.iconSize.name"),
                     LocalizeString::get("client.textmodule.effectStatus.iconSize.desc"), iconSize, FloatValue(8.f),
                     FloatValue(128.f), FloatValue(2.f), "showIcons"_istrue);
    addSliderSetting("rowSpacing", LocalizeString::get("client.textmodule.effectStatus.rowSpacing.name"),
                     LocalizeString::get("client.textmodule.effectStatus.rowSpacing.desc"), rowSpacing, FloatValue(0.f),
                     FloatValue(40.f), FloatValue(1.f), "showIcons"_istrue);
    addSetting("hideVanilla", LocalizeString::get("client.textmodule.effectStatus.hideVanilla.name"),
               LocalizeString::get("client.textmodule.effectStatus.hideVanilla.desc"), hideVanilla);
    addSetting("debug", LocalizeString::get("client.textmodule.effectStatus.debug.name"),
               LocalizeString::get("client.textmodule.effectStatus.debug.desc"), debug);

    // Same technique as MovablePaperdoll / MovableBossbar: after the HUD screen has been set up, adjust its controls.
    // It has to keep running while the module is off, so that Minecraft's display can be given back.
    listen<RenderLayerEvent>(static_cast<EventListenerFunc>(&EffectStatus::onRenderLayer), true,
                             10 /*need to overpower the hud renderer*/);
}

bool EffectStatus::forceMinecraftRenderer() {
    const bool icons = std::get<BoolValue>(showIcons);
    return icons || TextModule::forceMinecraftRenderer();
}

void EffectStatus::onRenderLayer(Event& evG) {
    auto& ev = reinterpret_cast<RenderLayerEvent&>(evG);

    auto* view = ev.getScreenView();
    if (!view || !view->visualTree || !view->visualTree->rootControl) return;

    auto* root = view->visualTree->rootControl;
    if (root->name != "hud_screen") return;

    // "mob_effects_renderer" is the control Minecraft draws its own effect icons with.
    auto* vanilla = root->findFirstDescendantWithName("mob_effects_renderer");
    if (!vanilla) return;

    constexpr float hiddenPos = 9999.f; // very scuffed, but it is how the other movable HUD modules do it
    const bool hide = isEnabled() && isActive() && std::get<BoolValue>(hideVanilla);

    if (hide) {
        // Remember where Minecraft put it, so it can be restored later.
        if (vanilla->position.x < hiddenPos) savedVanillaPos = vanilla->position;
        vanilla->position = { hiddenPos, hiddenPos };
        vanillaHidden = true;
    } else if (vanillaHidden) {
        if (vanilla->position.x >= hiddenPos) vanilla->position = savedVanillaPos;
        vanillaHidden = false;
    }
}

void EffectStatus::render(DrawUtil& dc, bool isDefault, bool inEditor) {
    const bool icons = std::get<BoolValue>(showIcons);
    const bool debugMode = std::get<BoolValue>(debug);

    // Debug mode prints plain text lines, and without the Minecraft renderer there are no game textures to draw.
    if (!icons || debugMode || !dc.isMinecraft()) {
        TextModule::render(dc, isDefault, inEditor);
        return;
    }

    renderWithIcons(static_cast<MCDrawUtil&>(dc), isDefault, inEditor);
}

void EffectStatus::renderWithIcons(MCDrawUtil& dc, bool isDefault, bool inEditor) {
    const bool level = std::get<BoolValue>(showLevel);
    const bool time = std::get<BoolValue>(showTime);
    const bool fill = std::get<BoolValue>(fillBg);
    const bool outline = std::get<BoolValue>(showOutline);
    const bool fixedSize = std::get<BoolValue>(customSize);

    const float textSize = std::get<FloatValue>(textSizeS);
    const float padXv = fill ? static_cast<float>(std::get<FloatValue>(padX)) : 0.f;
    const float padYv = fill ? static_cast<float>(std::get<FloatValue>(padY)) * 2.f : 0.f;

    d2d::Color bgCol = std::get<ColorValue>(bgColor).getMainColor();
    d2d::Color textCol = std::get<ColorValue>(textColor).getMainColor();
    d2d::Color outlineCol = std::get<ColorValue>(outlineColor).getMainColor();

    const auto effects = collectEffects(isDefault || inEditor, false, nullptr);

    // The text is measured; the icon size and the spacing are the user's settings.
    //   contentH = the tallest thing in a row (the icon or the text)
    //   rowH     = contentH plus the free space above AND below it, so two effects end up 2 * spacing apart
    const auto font = Renderer::FontSelection::SecondaryLight;
    const float textH = std::max(dc.getTextSize(L"Ag", font, textSize, false).y, 8.f);
    // Clamped to the slider ranges, so a value hand-edited into the config can't blow up the layout.
    const float icon = std::clamp(static_cast<float>(std::get<FloatValue>(iconSize)), 8.f, 128.f);
    const float spacing = std::clamp(static_cast<float>(std::get<FloatValue>(rowSpacing)), 0.f, 40.f);
    const float iconGap = std::max(4.f, icon * 0.25f);
    const float contentH = std::max(textH, icon);
    const float rowH = contentH + spacing * 2.f;

    std::vector<std::wstring> lines;
    lines.reserve(effects.size());
    float widestText = 0.f;
    for (auto const& e : effects) {
        lines.push_back(formatLine(e, level, time));
        widestText = std::max(widestText, dc.getTextSize(lines.back(), font, textSize, false).x);
    }

    const float rowCount = static_cast<float>(std::max<size_t>(lines.size(), 1));
    const float width = fixedSize ? static_cast<float>(std::get<FloatValue>(bgX))
                                  : icon + iconGap + widestText + padXv * 2.f;
    const float height = fixedSize ? static_cast<float>(std::get<FloatValue>(bgY))
                                   : rowCount * rowH + padYv * 2.f;

    d2d::Rect rc = d2d::Rect(0.f, 0.f, width, height);
    const float rad = (static_cast<float>(std::get<FloatValue>(radius)) / 10.f) * (rc.getHeight() / 2.f);

    if (!lines.empty()) {
        if (outline) dc.drawRoundedRectangle(rc, outlineCol, rad, std::get<FloatValue>(outlineThickness));
        if (fill) dc.fillRoundedRectangle(rc, bgCol, rad);

        for (size_t i = 0; i < lines.size(); ++i) {
            const float rowTop = padYv + static_cast<float>(i) * rowH;
            const float contentTop = rowTop + spacing; // free space above the effect

            if (const char* name = iconName(effects[i].id)) {
                // Same form the game's own UI uses for these icons ("textures/ui/speed_effect"), looked up through the
                // resource pack stack, so a resource pack that replaces the effect icons is picked up too.
                SDK::TexturePtr tex {};
                dc.renderCtx->getTexture(
                    &tex, SDK::ResourceLocation(std::string("textures/ui/") + name, SDK::ResourceFileSystem::UserPackage),
                    false);
                if (tex.textureData) {
                    const float iconTop = contentTop + (contentH - icon) / 2.f;
                    dc.drawImage(tex, { padXv, iconTop }, { icon, icon }, d2d::Colors::WHITE);
                }
            }

            d2d::Rect textRc = d2d::Rect(padXv + icon + iconGap, contentTop, width - padXv, contentTop + contentH);
            dc.drawText(textRc, lines[i], textCol, font, textSize, DWRITE_TEXT_ALIGNMENT_LEADING,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    this->rect.right = rect.left + width;
    this->rect.bottom = rect.top + height;

    dc.flush();
}

std::wstringstream EffectStatus::text(bool isDefault, bool inEditor) {
    std::wstringstream out;
    const bool level = std::get<BoolValue>(showLevel);
    const bool time = std::get<BoolValue>(showTime);
    const bool debugMode = std::get<BoolValue>(debug);

    std::wstringstream dbg;
    const auto effects = collectEffects(isDefault || inEditor, debugMode, &dbg);

    const std::wstring debugText = dbg.str();
    out << debugText;
    if (!debugText.empty() && !effects.empty()) out << L"\n";

    bool firstLine = true;
    for (auto const& e : effects) {
        if (!firstLine) out << L"\n";
        out << formatLine(e, level, time);
        firstLine = false;
    }
    return out;
}
