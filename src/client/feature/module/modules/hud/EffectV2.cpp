#include "pch.h"
#include "EffectV2.h"

#include "client/event/events/RenderLayerEvent.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/gui/ScreenView.h"
#include "mc/common/client/gui/controls/UIControl.h"
#include "mc/common/client/gui/controls/VisualTree.h"
#include "mc/common/entity/component/MobEffectsComponent.h"
#include "mc/common/world/actor/player/Player.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace {
    // File name (without extension) of the game's own effect icon, textures/ui/<name>. nullptr = no icon.
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

    bool isHarmful(int id) {
        switch (id) {
        case 2:  // slowness
        case 4:  // mining fatigue
        case 7:  // instant damage
        case 9:  // nausea
        case 15: // blindness
        case 17: // hunger
        case 18: // weakness
        case 19: // poison
        case 20: // wither
        case 24: // levitation
        case 25: // fatal poison
        case 28: // bad omen
        case 30: // darkness
        case 31: // trial omen
        case 33: // weaving
        case 34: // oozing
        case 35: // infested
        case 36: // raid omen
            return true;
        default:
            return false;
        }
    }

    d2d::Color alpha(d2d::Color c, float a) {
        c.a *= std::clamp(a, 0.f, 1.f);
        return c;
    }

    d2d::Color mix(d2d::Color const& a, d2d::Color const& b, float t) {
        t = std::clamp(t, 0.f, 1.f);
        return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
    }

    d2d::Rect shrink(d2d::Rect const& r, float by) { return { r.left + by, r.top + by, r.right - by, r.bottom - by }; }

    // A filled rectangle with its four corners cut by one pixel, which is how the game's own pixel panels look.
    void stepRect(MCDrawUtil& dc, d2d::Rect const& r, float px, d2d::Color const& color) {
        if (r.right - r.left <= px * 2.f || r.bottom - r.top <= px * 2.f) {
            dc.fillRectangle(r, color);
            return;
        }
        dc.fillRectangle({ r.left + px, r.top, r.right - px, r.bottom }, color);
        dc.fillRectangle({ r.left, r.top + px, r.right, r.bottom - px }, color);
    }

    // Dark slot: outline, a lighter edge inside it, then the fill.
    void pixelSlot(MCDrawUtil& dc, d2d::Rect const& r, float px, d2d::Color const& outline, d2d::Color const& edge,
                   d2d::Color const& fill) {
        stepRect(dc, r, px, outline);
        stepRect(dc, shrink(r, px), px, edge);
        stepRect(dc, shrink(r, px * 2.f), px, fill);
    }

    // Gray pill: outline, the fill, a light line along the top and a dark one along the bottom.
    void pixelPill(MCDrawUtil& dc, d2d::Rect const& r, float px, d2d::Color const& outline, d2d::Color const& fill) {
        stepRect(dc, r, px, outline);
        const d2d::Rect in = shrink(r, px);
        stepRect(dc, in, px, fill);
        dc.fillRectangle({ in.left + px, in.top, in.right - px, in.top + px }, mix(fill, d2d::Color(1.f, 1.f, 1.f, fill.a), 0.35f));
        dc.fillRectangle({ in.left + px, in.bottom - px, in.right - px, in.bottom },
                         mix(fill, d2d::Color(0.f, 0.f, 0.f, fill.a), 0.4f));
    }

    // Green while most of the time is left, yellow around the middle, red at the end.
    d2d::Color timeColor(float fraction) {
        const d2d::Color green(0.2f, 0.83f, 0.4f, 1.f);
        const d2d::Color yellow(0.98f, 0.8f, 0.2f, 1.f);
        const d2d::Color red(0.93f, 0.26f, 0.26f, 1.f);
        fraction = std::clamp(fraction, 0.f, 1.f);
        if (fraction >= 0.5f) return mix(yellow, green, (fraction - 0.5f) / 0.5f);
        return mix(red, yellow, fraction / 0.5f);
    }

    std::wstring formatTime(int ticks, bool padMinutes) {
        if (ticks <= 0) return L"xx:xx"; // no counter: infinite
        const int total = (ticks + 19) / 20; // round up, like the game does
        if (total >= 3600) return std::format(L"{}:{:02}:{:02}", total / 3600, (total / 60) % 60, total % 60);
        if (padMinutes) return std::format(L"{:02}:{:02}", total / 60, total % 60);
        return std::format(L"{}:{:02}", total / 60, total % 60);
    }

    struct Entry {
        SDK::EffectData data {};
        float fraction = 1.f; // how much of the effect is left, 0 - 1
    };
} // namespace

EffectV2::EffectV2()
    : HUDModule("EffectV2", LocalizeString::get("client.hudmodule.effectV2.name"),
                LocalizeString::get("client.hudmodule.effectV2.desc"), HUD) {
    // ---- Look ----
    style.addEntry(EnumEntry(StyleWide, LocalizeString::get("client.hudmodule.effectV2.style.wide.name"),
                             LocalizeString::get("client.hudmodule.effectV2.style.wide.desc")));
    style.addEntry(EnumEntry(StyleCompact, LocalizeString::get("client.hudmodule.effectV2.style.compact.name"),
                             LocalizeString::get("client.hudmodule.effectV2.style.compact.desc")));
    style.addEntry(EnumEntry(StyleMini, LocalizeString::get("client.hudmodule.effectV2.style.mini.name"),
                             LocalizeString::get("client.hudmodule.effectV2.style.mini.desc")));
    addEnumSetting("style", LocalizeString::get("client.hudmodule.effectV2.style.name"),
                   LocalizeString::get("client.hudmodule.effectV2.style.desc"), style);

    stacking.addEntry(EnumEntry(StackVertical, LocalizeString::get("client.hudmodule.effectV2.stack.vertical.name"),
                                LocalizeString::get("client.hudmodule.effectV2.stack.vertical.desc")));
    stacking.addEntry(EnumEntry(StackHorizontal, LocalizeString::get("client.hudmodule.effectV2.stack.horizontal.name"),
                                LocalizeString::get("client.hudmodule.effectV2.stack.horizontal.desc")));
    addEnumSetting("stacking", LocalizeString::get("client.hudmodule.effectV2.stack.name"),
                   LocalizeString::get("client.hudmodule.effectV2.stack.desc"), stacking);

    sortOrder.addEntry(EnumEntry(SortGame, LocalizeString::get("client.hudmodule.effectV2.sort.game.name"),
                                 LocalizeString::get("client.hudmodule.effectV2.sort.game.desc")));
    sortOrder.addEntry(EnumEntry(SortLongest, LocalizeString::get("client.hudmodule.effectV2.sort.longest.name"),
                                 LocalizeString::get("client.hudmodule.effectV2.sort.longest.desc")));
    sortOrder.addEntry(EnumEntry(SortShortest, LocalizeString::get("client.hudmodule.effectV2.sort.shortest.name"),
                                 LocalizeString::get("client.hudmodule.effectV2.sort.shortest.desc")));
    addEnumSetting("sort", LocalizeString::get("client.hudmodule.effectV2.sort.name"),
                   LocalizeString::get("client.hudmodule.effectV2.sort.desc"), sortOrder);

    addSliderSetting("slotSize", LocalizeString::get("client.hudmodule.effectV2.slotSize.name"),
                     LocalizeString::get("client.hudmodule.effectV2.slotSize.desc"), slotSize, FloatValue(16.f),
                     FloatValue(96.f), FloatValue(1.f));
    addSliderSetting("spacing", LocalizeString::get("client.hudmodule.effectV2.spacing.name"),
                     LocalizeString::get("client.hudmodule.effectV2.spacing.desc"), spacing, FloatValue(0.f),
                     FloatValue(40.f), FloatValue(1.f));

    // ---- What is shown ----
    addSetting("showLevel", LocalizeString::get("client.hudmodule.effectV2.showLevel.name"),
               LocalizeString::get("client.hudmodule.effectV2.showLevel.desc"), showLevel);
    addSetting("showLevelOne", LocalizeString::get("client.hudmodule.effectV2.showLevelOne.name"),
               LocalizeString::get("client.hudmodule.effectV2.showLevelOne.desc"), showLevelOne, "showLevel"_istrue);
    addSetting("showTime", LocalizeString::get("client.hudmodule.effectV2.showTime.name"),
               LocalizeString::get("client.hudmodule.effectV2.showTime.desc"), showTime);
    addSetting("padMinutes", LocalizeString::get("client.hudmodule.effectV2.padMinutes.name"),
               LocalizeString::get("client.hudmodule.effectV2.padMinutes.desc"), padMinutes, "showTime"_istrue);
    addSliderSetting("textSizePx", LocalizeString::get("client.hudmodule.effectV2.textSize.name"),
                     LocalizeString::get("client.hudmodule.effectV2.textSize.desc"), textSize, FloatValue(8.f),
                     FloatValue(48.f), FloatValue(1.f));

    // ---- Time bar ----
    barPosition.addEntry(EnumEntry(BarLeft, LocalizeString::get("client.hudmodule.effectV2.bar.left.name"),
                                   LocalizeString::get("client.hudmodule.effectV2.bar.left.desc")));
    barPosition.addEntry(EnumEntry(BarTop, LocalizeString::get("client.hudmodule.effectV2.bar.top.name"),
                                   LocalizeString::get("client.hudmodule.effectV2.bar.top.desc")));
    barPosition.addEntry(EnumEntry(BarBottom, LocalizeString::get("client.hudmodule.effectV2.bar.bottom.name"),
                                   LocalizeString::get("client.hudmodule.effectV2.bar.bottom.desc")));
    barPosition.addEntry(EnumEntry(BarOff, LocalizeString::get("client.hudmodule.effectV2.bar.off.name"),
                                   LocalizeString::get("client.hudmodule.effectV2.bar.off.desc")));
    addEnumSetting("barPosition", LocalizeString::get("client.hudmodule.effectV2.bar.name"),
                   LocalizeString::get("client.hudmodule.effectV2.bar.desc"), barPosition);
    addSetting("barWithTime", LocalizeString::get("client.hudmodule.effectV2.barWithTime.name"),
               LocalizeString::get("client.hudmodule.effectV2.barWithTime.desc"), barWithTime);
    addSetting("colorBarByTime", LocalizeString::get("client.hudmodule.effectV2.colorBarByTime.name"),
               LocalizeString::get("client.hudmodule.effectV2.colorBarByTime.desc"), colorBarByTime);
    addSetting("barColor", LocalizeString::get("client.hudmodule.effectV2.barColor.name"),
               LocalizeString::get("client.hudmodule.effectV2.barColor.desc"), barColor, "colorBarByTime"_isfalse);
    addSliderSetting("warnAt", LocalizeString::get("client.hudmodule.effectV2.flashSeconds.name"),
                     LocalizeString::get("client.hudmodule.effectV2.flashSeconds.desc"), flashSeconds, FloatValue(0.f),
                     FloatValue(60.f), FloatValue(1.f));

    // ---- Colors ----
    addSetting("slotColorPx", LocalizeString::get("client.hudmodule.effectV2.slotColor.name"),
               LocalizeString::get("client.hudmodule.effectV2.slotColor.desc"), slotColor);
    addSetting("pillColorPx", LocalizeString::get("client.hudmodule.effectV2.pillColor.name"),
               LocalizeString::get("client.hudmodule.effectV2.pillColor.desc"), pillColor);
    addSetting("borderColorPx", LocalizeString::get("client.hudmodule.effectV2.borderColor.name"),
               LocalizeString::get("client.hudmodule.effectV2.borderColor.desc"), borderColor);
    addSetting("textColor", LocalizeString::get("client.hudmodule.effectV2.textColor.name"),
               LocalizeString::get("client.hudmodule.effectV2.textColor.desc"), textColor);
    addSetting("redBorderBad", LocalizeString::get("client.hudmodule.effectV2.tintHarmful.name"),
               LocalizeString::get("client.hudmodule.effectV2.tintHarmful.desc"), tintHarmful);

    addSetting("hideVanilla", LocalizeString::get("client.hudmodule.effectV2.hideVanilla.name"),
               LocalizeString::get("client.hudmodule.effectV2.hideVanilla.desc"), hideVanilla);

    // Same technique as EffectStatus / MovablePaperdoll: adjust the HUD screen's controls once it has been set up.
    // It has to keep running while the module is off, so that Minecraft's display can be given back.
    listen<RenderLayerEvent>(static_cast<EventListenerFunc>(&EffectV2::onRenderLayer), true,
                             10 /*need to overpower the hud renderer*/);
}

void EffectV2::onRenderLayer(Event& evG) {
    auto& ev = reinterpret_cast<RenderLayerEvent&>(evG);

    auto* view = ev.getScreenView();
    if (!view || !view->visualTree || !view->visualTree->rootControl) return;

    auto* root = view->visualTree->rootControl;
    if (root->name != "hud_screen") return;

    auto* vanilla = root->findFirstDescendantWithName("mob_effects_renderer");
    if (!vanilla) return;

    constexpr float hiddenPos = 9999.f;
    const bool hide = isEnabled() && isActive() && std::get<BoolValue>(hideVanilla);

    if (hide) {
        if (vanilla->position.x < hiddenPos) savedVanillaPos = vanilla->position;
        vanilla->position = { hiddenPos, hiddenPos };
        vanillaHidden = true;
    } else if (vanillaHidden) {
        if (vanilla->position.x >= hiddenPos) vanilla->position = savedVanillaPos;
        vanillaHidden = false;
    }
}

void EffectV2::render(DrawUtil& dcGeneric, bool isDefault, bool inEditor) {
    // The effect icons are game textures, which only the Minecraft renderer can draw.
    if (!dcGeneric.isMinecraft()) return;
    auto& dc = static_cast<MCDrawUtil&>(dcGeneric);

    const bool sample = isDefault || inEditor;

    // ---- Collect the effects ----
    std::vector<Entry> entries;
    if (sample) {
        // Something to move around in the HUD editor.
        entries.push_back({ { 1, 20 * 90, 1 }, 0.75f });
        entries.push_back({ { 5, 20 * 45, 1 }, 0.4f });
        entries.push_back({ { 19, 20 * 12, 0 }, 0.1f });
    } else {
        auto* ci = SDK::ClientInstance::get();
        auto* plr = ci ? ci->getLocalPlayer() : nullptr;
        auto* comp = plr ? plr->tryGetComponent<SDK::MobEffectsComponent>() : nullptr;
        if (comp) {
            for (auto const& e : comp->read()) {
                Entry en;
                en.data = e;
                if (e.duration > 0) {
                    // The game only tells us the time that is left. The longest time seen since the effect was applied
                    // is what the bar is measured against; a bigger number than last frame means it was refreshed.
                    auto& s = seen[e.id];
                    if (s.maxTicks <= 0 || e.duration > s.lastTicks) s.maxTicks = e.duration;
                    s.lastTicks = e.duration;
                    en.fraction = std::clamp(static_cast<float>(e.duration) / static_cast<float>(s.maxTicks), 0.f, 1.f);
                }
                entries.push_back(en);
            }
        }

        // Forget effects that ran out, so a new one of the same kind starts with a full bar.
        for (auto it = seen.begin(); it != seen.end();) {
            const int id = it->first;
            const bool present = std::any_of(entries.begin(), entries.end(), [id](Entry const& e) { return e.data.id == id; });
            it = present ? std::next(it) : seen.erase(it);
        }
    }

    // Effects the game gives no icon (instant effects) are never shown for long; skip ids that make no sense.
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](Entry const& e) { return e.data.id <= 0; }),
                  entries.end());

    const int sortKey = sortOrder.getSelectedKey();
    if (sortKey != SortGame) {
        auto key = [](Entry const& e) { return e.data.duration > 0 ? e.data.duration : INT_MAX; }; // infinite = longest
        std::stable_sort(entries.begin(), entries.end(), [&](Entry const& a, Entry const& b) {
            return sortKey == SortLongest ? key(a) > key(b) : key(a) < key(b);
        });
    }

    if (entries.empty()) {
        rect.right = rect.left;
        rect.bottom = rect.top;
        return;
    }

    // ---- Settings ----
    const int styleKey = style.getSelectedKey();
    const bool mini = styleKey == StyleMini;
    const bool wide = styleKey == StyleWide;
    const bool horizontal = stacking.getSelectedKey() == StackHorizontal;
    const int barPos = barPosition.getSelectedKey();
    const bool showLvl = std::get<BoolValue>(showLevel);
    const bool lvlOne = std::get<BoolValue>(showLevelOne);
    const bool showT = std::get<BoolValue>(showTime) && !mini; // Mini has no time, only the bar
    const bool pad = std::get<BoolValue>(padMinutes);
    const bool byTime = std::get<BoolValue>(colorBarByTime);
    const bool tint = std::get<BoolValue>(tintHarmful);

    const float slot = std::clamp(static_cast<float>(std::get<FloatValue>(slotSize)), 16.f, 96.f);
    const float gap = std::clamp(static_cast<float>(std::get<FloatValue>(spacing)), 0.f, 40.f);
    const float px = std::max(1.f, std::round(slot / 20.f)); // one "game pixel"
    const bool drawBar = barPos != BarOff && (mini || std::get<BoolValue>(barWithTime));
    const float textPx = std::clamp(static_cast<float>(std::get<FloatValue>(textSize)), 8.f, 48.f);
    const float levelPx = std::max(textPx * 0.6f, 8.f);
    const float flashFor = std::max(0.f, static_cast<float>(std::get<FloatValue>(flashSeconds)));

    const d2d::Color slotCol(std::get<ColorValue>(slotColor).getMainColor());
    const d2d::Color pillCol(std::get<ColorValue>(pillColor).getMainColor());
    const d2d::Color borderCol(std::get<ColorValue>(borderColor).getMainColor());
    const d2d::Color textCol(std::get<ColorValue>(textColor).getMainColor());
    const d2d::Color fixedBarCol(std::get<ColorValue>(barColor).getMainColor());
    const d2d::Color harmfulCol(0.88f, 0.22f, 0.22f, 1.f);
    const d2d::Color urgentCol(0.95f, 0.3f, 0.3f, 1.f);

    const auto font = Renderer::FontSelection::PrimaryRegular;
    const float timeH = std::max(dc.getTextSize(L"0:00", font, textPx, false).y, textPx);
    const float pillPad = std::max(4.f, textPx * 0.4f);

    // All time pills are as wide as the widest time, so the stack lines up.
    std::vector<std::wstring> times;
    times.reserve(entries.size());
    float pillW = dc.getTextSize(pad ? L"00:00" : L"0:00", font, textPx, false).x + pillPad * 2.f;
    for (auto const& e : entries) {
        times.push_back(formatTime(e.data.duration, pad));
        pillW = std::max(pillW, dc.getTextSize(times.back(), font, textPx, false).x + pillPad * 2.f);
    }
    if (wide) pillW = std::max(pillW, slot * 1.9f);

    const float pillH = wide ? std::max(timeH + 6.f, slot * 0.86f) : timeH + 4.f;
    const float pillGap = wide ? std::max(3.f, slot * 0.1f) : std::max(2.f, slot * 0.08f);

    // Size of one effect.
    float itemW, itemH;
    if (mini) {
        itemW = itemH = slot;
    } else if (wide) {
        itemW = slot + (showT ? pillGap + pillW : 0.f);
        itemH = slot;
    } else {
        itemW = showT ? std::max(slot, pillW) : slot;
        itemH = slot + (showT ? pillGap + pillH : 0.f);
    }

    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();

    // ---- Draw ----
    // The module's shapes and the icons have to land in the order they are drawn in: icons are always drawn straight
    // away, so the shapes must be too (otherwise the slots are drawn over the icons at the final flush, and the queued
    // shapes get mixed up with the image draws). The text is queued and drawn by the final flush, on top of everything.
    // Same way TargetHUD does it.
    dc.flush(false);
    dc.setImmediate(true);

    for (size_t i = 0; i < entries.size(); ++i) {
        auto const& e = entries[i];
        const float ox = horizontal ? static_cast<float>(i) * (itemW + gap) : 0.f;
        const float oy = horizontal ? 0.f : static_cast<float>(i) * (itemH + gap);

        // In the compact style the slot is centered over the (maybe wider) time pill.
        const float slotX = ox + (wide ? 0.f : (itemW - slot) / 2.f);
        const d2d::Rect slotRc(slotX, oy, slotX + slot, oy + slot);

        const bool infinite = e.data.duration <= 0;
        const bool urgent = !infinite && flashFor > 0.f && e.data.duration <= static_cast<int>(flashFor * 20.f);
        const float pulse = urgent ? 0.5f + 0.5f * std::sin(t * 7.f) : 0.f;

        const bool bad = tint && isHarmful(e.data.id);
        const d2d::Color white(1.f, 1.f, 1.f, slotCol.a);
        d2d::Color edge = bad ? mix(slotCol, harmfulCol, 0.85f) : mix(slotCol, white, 0.16f);
        d2d::Color fill = slotCol;
        if (urgent) fill = mix(slotCol, urgentCol, 0.35f * pulse);

        pixelSlot(dc, slotRc, px, borderCol, edge, fill);

        // Icon
        if (const char* name = iconName(e.data.id)) {
            SDK::TexturePtr tex {};
            dc.renderCtx->getTexture(
                &tex, SDK::ResourceLocation(std::string("textures/ui/") + name, SDK::ResourceFileSystem::UserPackage),
                false);
            if (tex.textureData) {
                const float icon = slot * 0.66f;
                const float cx = slotRc.left + (slot - icon) / 2.f;
                const float cy = slotRc.top + (slot - icon) / 2.f;
                dc.drawImage(tex, { cx, cy }, { icon, icon }, d2d::Colors::WHITE);
            }
        }

        // Bar
        if (drawBar) {
            const float inset = std::max(3.f, slot * 0.1f);
            const float thick = std::max(2.f, slot * 0.08f);
            const d2d::Color barCol = byTime ? timeColor(e.fraction) : fixedBarCol;
            const d2d::Color track(0.f, 0.f, 0.f, 0.55f);
            const float frac = infinite ? 1.f : e.fraction;
            if (barPos == BarLeft) {
                const float x = slotRc.left + inset * 0.6f;
                const float y0 = slotRc.top + inset, y1 = slotRc.bottom - inset;
                dc.fillRectangle({ x, y0, x + thick, y1 }, track);
                dc.fillRectangle({ x, y1 - (y1 - y0) * frac, x + thick, y1 }, barCol);
            } else {
                const float x0 = slotRc.left + inset, x1 = slotRc.right - inset;
                const float y = barPos == BarTop ? slotRc.top + inset * 0.6f : slotRc.bottom - inset * 0.6f - thick;
                dc.fillRectangle({ x0, y, x1, y + thick }, track);
                dc.fillRectangle({ x0, y, x0 + (x1 - x0) * frac, y + thick }, barCol);
            }
        }

        // Level in the top right corner of the slot, with a dark edge so it reads on any icon.
        if (showLvl && (e.data.amplifier > 0 || lvlOne)) {
            const std::wstring lvl = std::to_wstring(e.data.amplifier + 1);
            const float w = dc.getTextSize(lvl, font, levelPx, false).x + 2.f;
            const d2d::Rect rc(slotRc.right - w - 3.f, slotRc.top + 1.f, slotRc.right - 3.f, slotRc.top + 1.f + levelPx + 4.f);
            dc.drawText({ rc.left + 1.f, rc.top + 1.f, rc.right + 1.f, rc.bottom + 1.f }, lvl, d2d::Color(0.f, 0.f, 0.f, 0.85f),
                        font, levelPx, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            dc.drawText(rc, lvl, textCol, font, levelPx, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        // Time
        if (showT) {
            d2d::Rect pillRc;
            if (wide) {
                const float px = slotRc.right + pillGap;
                const float py = oy + (slot - pillH) / 2.f;
                pillRc = { px, py, px + pillW, py + pillH };
            } else {
                const float px = ox + (itemW - pillW) / 2.f;
                const float py = oy + slot + pillGap;
                pillRc = { px, py, px + pillW, py + pillH };
            }
            d2d::Color pillFill = pillCol;
            if (urgent) pillFill = mix(pillCol, urgentCol, 0.35f * pulse);
            pixelPill(dc, pillRc, px, borderCol, pillFill);
            const d2d::Color timeCol = urgent ? mix(textCol, urgentCol, 0.7f) : textCol;
            const d2d::Rect shadowRc(pillRc.left + px, pillRc.top + px, pillRc.right + px, pillRc.bottom + px);
            dc.drawText(shadowRc, times[i], d2d::Color(0.f, 0.f, 0.f, 0.4f), font, textPx, DWRITE_TEXT_ALIGNMENT_CENTER,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dc.drawText(pillRc, times[i], timeCol, font, textPx, DWRITE_TEXT_ALIGNMENT_CENTER,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    const float n = static_cast<float>(entries.size());
    const float totalW = horizontal ? n * itemW + (n - 1.f) * gap : itemW;
    const float totalH = horizontal ? itemH : n * itemH + (n - 1.f) * gap;
    rect.right = rect.left + totalW;
    rect.bottom = rect.top + totalH;

    dc.setImmediate(false);
    dc.flush();
}
