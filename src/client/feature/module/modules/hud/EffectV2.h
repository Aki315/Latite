#pragma once
#include "../../HUDModule.h"

#include <map>
#include <string>

// Your potion effects as small panels, in the style of the Java "Inventory HUD" mod: a dark slot with the effect's
// icon and its level in the corner, and either a pill with the time left beside it (Wide) or a thin slot with the time
// under it (Compact). A bar shows how much of the effect is left, green > yellow > red.
// Position and size are set in Latite's HUD editor, like any other HUD module.
//
// Everything is drawn by this module (no texture from any other mod); only the game's own effect icons are used.
class EffectV2 final : public HUDModule {
public:
    EffectV2();
    ~EffectV2() override = default;

    [[nodiscard]] bool forceMinecraftRenderer() override { return true; }

    void render(DrawUtil& ctx, bool isDefault, bool inEditor) override;

    // Hides / restores Minecraft's own effect display on the HUD screen.
    void onRenderLayer(Event& ev);

private:
    static constexpr int StyleWide = 0;    // icon slot + time pill beside it
    static constexpr int StyleCompact = 1; // icon slot, time under it

    static constexpr int StackVertical = 0;
    static constexpr int StackHorizontal = 1;

    static constexpr int BarLeft = 0;
    static constexpr int BarTop = 1;
    static constexpr int BarBottom = 2;
    static constexpr int BarOff = 3;

    static constexpr int SortGame = 0;     // the order the game lists them in
    static constexpr int SortLongest = 1;  // most time left first
    static constexpr int SortShortest = 2; // least time left first

    EnumData style;
    EnumData stacking;
    EnumData barPosition;
    EnumData sortOrder;

    ValueType showLevel = BoolValue(true);
    ValueType showLevelOne = BoolValue(true);  // Also write the "1" for level I.
    ValueType showTime = BoolValue(true);
    ValueType padMinutes = BoolValue(false);   // 01:26 instead of 1:26
    ValueType slotSize = FloatValue(38.f);     // The square with the icon; the icon fills most of it.
    ValueType spacing = FloatValue(4.f);       // Between two effects.
    ValueType textSize = FloatValue(18.f);
    ValueType cornerRadius = FloatValue(4.f);
    ValueType colorBarByTime = BoolValue(true);
    ValueType barColor = ColorValue(0.2f, 0.83f, 0.4f, 1.f);
    ValueType slotColor = ColorValue(0.13f, 0.14f, 0.17f, 0.92f);
    ValueType pillColor = ColorValue(0.42f, 0.47f, 0.55f, 0.92f);
    ValueType borderColor = ColorValue(0.04f, 0.05f, 0.07f, 1.f);
    ValueType textColor = ColorValue(1.f, 1.f, 1.f, 1.f);
    ValueType tintHarmful = BoolValue(true);   // Bad effects (poison, slowness...) get a red border.
    ValueType flashSeconds = FloatValue(10.f); // The last seconds flash and the time turns red. 0 = never.
    ValueType hideVanilla = BoolValue(true);

    // The longest time each effect has had since it was applied, to know how full its bar is. Keyed by effect id;
    // the second number is the time it had last frame, so a refresh (a bigger number than before) starts the bar over.
    struct Seen {
        int maxTicks = 0;
        int lastTicks = 0;
    };
    std::map<int, Seen> seen;

    bool vanillaHidden = false;
    Vec2 savedVanillaPos = {};
};
