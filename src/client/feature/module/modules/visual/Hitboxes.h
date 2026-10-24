#pragma once
#include "../../Module.h"

class Hitboxes : public Module {
public:
    void onRenderLevel(RenderLevelEvent& event);

    Hitboxes();

private:
    EnumData mode;
    static constexpr int mode_bounding_box = 0;
    static constexpr int mode_hitbox = 1;

    ValueType transparent = BoolValue(false);
    ValueType boxColor = ColorValue(1.f, 1.f, 1.f, 1.f);

    // A player's box changes color while they are close enough to hit.
    ValueType rangeColorOn = BoolValue(true);
    ValueType hitRange = FloatValue(3.f);
    ValueType rangeColor = ColorValue(1.f, 0.2f, 0.2f, 1.f);

    // Visual-only multipliers applied to the drawn box (1.0 = the real bounding box).
    ValueType widthScale = FloatValue(1.3f);
    ValueType heightScale = FloatValue(1.1f);

    // Thickness of every line that is drawn, in screen pixels (1 = the game's thin line).
    ValueType lineThickness = FloatValue(1.f);

    ValueType showEyeLine = BoolValue(true);
    ValueType eyeColor = ColorValue(1.f, 0.f, 0.f, 1.f);

    ValueType showLine = BoolValue(true);
    ValueType lineColor = ColorValue(0.f, 0.f, 1.f, 1.f);

    ValueType showHelper = BoolValue(false);
    ValueType helperLineColor = ColorValue(1.f, 0.f, 1.f, 1.f);

    // Draw your own hitbox while in third person (never in first person).
    ValueType showOwnThirdPerson = BoolValue(true);

    ValueType localPlayer = BoolValue(true);
    ValueType items = BoolValue(true);

    // Projectiles whose hitbox is not drawn at all.
    ValueType hideArrows = BoolValue(true);
    ValueType hideWindCharges = BoolValue(true);
    ValueType hidePearls = BoolValue(true);
};
