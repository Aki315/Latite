#pragma once
#include "../../HUDModule.h"

#include <chrono>
#include <string>

// Shows the player you just hit: head, name, health (hearts or number, with golden hearts), armor and offhand.
// It disappears a few seconds after your last hit on them. Position and size are set in Latite's HUD editor.
class TargetHUD final : public HUDModule {
public:
    TargetHUD();
    ~TargetHUD() override = default;

    void render(DrawUtil& ctx, bool isDefault, bool inEditor) override;
    void afterLoadConfig() override;

    bool forceMinecraftRenderer() override { return true; }

    void onAttack(Event& ev);

private:
    static constexpr int StyleBar = 0;
    static constexpr int StyleHearts = 1;
    static constexpr int StyleNumber = 2;

    struct Info {
        std::wstring name;
        float health = -1.f; // < 0 = unknown
        float maxHealth = 20.f;
        float absorption = 0.f;
        std::string faceTexturePath;
        SDK::ItemStack* armor[4] {};
        SDK::ItemStack* offhand = nullptr;
    };

    EnumData style;
    ValueType duration = FloatValue(5.f);
    ValueType showHead = BoolValue(true);
    ValueType showArmor = BoolValue(true);
    ValueType showOffhand = BoolValue(true);
    ValueType showAbsorption = BoolValue(true);
    ValueType textSize = FloatValue(22.f);
    ValueType textColor = ColorValue(1.f, 1.f, 1.f, 1.f);
    ValueType healthColor = ColorValue(0.9f, 0.12f, 0.2f, 1.f);
    ValueType backgroundColor = ColorValue(0.055f, 0.075f, 0.125f, 0.92f);
    ValueType accentColor = ColorValue(0.12f, 0.56f, 1.f, 1.f);

    // The player you last hit. Only the runtime id is kept, the actor is looked up again every frame.
    uint64_t targetId = 0;
    bool hasTarget = false;
    std::chrono::steady_clock::time_point lastHit {};

    SDK::Actor* findTarget() const;
    bool gatherInfo(bool preview, Info& out);
    std::string getFaceTexturePath(SDK::Player* player) const;
    void drawHeartRows(DrawUtil& dc, float x, float y, int hearts, int full, bool half, bool gold) const;
};
