#pragma once
#include "../../HUDModule.h"

class ArmorHUD : public HUDModule {
private:
    ValueType horizontal = BoolValue(false);
    ValueType dynamicColor = BoolValue(false);
    // Durability text turns yellow / red once the remaining durability (in percent) drops to these values.
    ValueType yellowThreshold = FloatValue(50.f);
    ValueType redThreshold = FloatValue(25.f);
    ValueType greenColor = ColorValue(0.33f, 1.f, 0.33f, 1.f);
    ValueType yellowColor = ColorValue(1.f, 0.85f, 0.2f, 1.f);
    ValueType redColor = ColorValue(1.f, 0.25f, 0.25f, 1.f);

    ValueType durability = BoolValue(true);
    ValueType durabilityText = BoolValue(true);
    ValueType background = BoolValue(true);

    static constexpr int mode_vetical = 0;
    static constexpr int mode_horizontal = 1;
    EnumData mode;

    ValueType showDamage = BoolValue(false);
    ValueType percentage = BoolValue(true);
    ValueType txtColor = ColorValue();

public:
    ArmorHUD();
    virtual ~ArmorHUD() = default;

    virtual void render(DrawUtil& ctx, bool isDefault, bool inEditor);

    // Force Armor Hud to use Minecraft renderer at all times
    virtual bool forceMinecraftRenderer() override { return true; }

private:
    d2d::Rect drawItem(MCDrawUtil& dc, Vec2 pos, SDK::ItemStack* stack, float size = 1.f);
    StoredColor durabilityColor(int remaining, int max);
};
