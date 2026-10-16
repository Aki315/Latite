#pragma once
#include "../../Module.h"
#include "client/event/events/RenderOverlayEvent.h"
#include "util/DrawContext.h"

// Shows the health of nearby players above their heads.
//
// Style:
//   Number -> text above the head. Format is either "10/10" (hearts) or "20/20" (health points).
//   Hearts -> a row of hearts above the head (full / half / empty).
class HealthIndicator : public Module {
public:
    HealthIndicator();
    ~HealthIndicator() override = default;

private:
    enum Style : int {
        StyleNumber = 0,
        StyleHearts = 1,
    };

    enum NumberFormat : int {
        FormatHearts = 0, // 10/10
        FormatPoints = 1, // 20/20
    };

    void onRenderOverlay(Event& ev);

    void drawNumber(D2DUtil& dc, Vec2 const& anchor, float scale, float health, float maxHealth);
    void drawHearts(D2DUtil& dc, Vec2 const& anchor, float scale, float health, float maxHealth);

    EnumData style;
    EnumData numberFormat;

    ValueType textSize = FloatValue(28.f);
    ValueType textColor = ColorValue(1.f, 1.f, 1.f, 1.f);
    ValueType colorByHealth = BoolValue(true);
    ValueType showBackground = BoolValue(true);
    ValueType playersOnly = BoolValue(true);
    ValueType maxDistance = FloatValue(32.f);
    ValueType heightOffset = FloatValue(0.75f);
};
