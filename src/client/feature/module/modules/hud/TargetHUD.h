#pragma once
#include "../../HUDModule.h"

#include <chrono>
#include <string>

// What the Target HUD knows about the player you hit.
struct TargetInfo {
    std::wstring name;
    float health = -1.f; // < 0 = unknown
    float maxHealth = 20.f;
    float absorption = 0.f;
    std::string faceTexturePath;
    SDK::ItemStack* armor[4] {};
    SDK::ItemStack* offhand = nullptr;
};

// Shows the player you just hit: head, name, health (colored green > yellow > red as it drops), golden hearts,
// armor and offhand, in one of five designs. It disappears a few seconds after your last hit on them.
// Position and size are set in Latite's HUD editor.
class TargetHUD final : public HUDModule {
public:
    TargetHUD();
    ~TargetHUD() override = default;

    void render(DrawUtil& ctx, bool isDefault, bool inEditor) override;
    void afterLoadConfig() override;

    bool forceMinecraftRenderer() override { return true; }

    void onAttack(Event& ev);

private:
    static constexpr int DesignPill = 0;
    static constexpr int DesignHearts = 1;
    static constexpr int DesignRing = 2;
    static constexpr int DesignTag = 3;
    static constexpr int DesignSideBar = 4;

    static constexpr int DisplayDefault = 0; // The design's own choice (Hearts design: hearts, the others: bar).
    static constexpr int DisplayBar = 1;
    static constexpr int DisplayHearts = 2;
    static constexpr int DisplayNumber = 3;
    static constexpr int UnitPoints = 0; // 20/20
    static constexpr int UnitHearts = 1; // 10/10

    EnumData design;
    EnumData healthDisplay;
    EnumData numberUnit;
    ValueType decimals = FloatValue(0.f);
    ValueType duration = FloatValue(5.f);
    ValueType showArmor = BoolValue(true);
    ValueType showOffhand = BoolValue(true);
    ValueType showDurability = BoolValue(true);
    ValueType showAbsorption = BoolValue(true);
    ValueType textSize = FloatValue(20.f);
    ValueType textColor = ColorValue(1.f, 1.f, 1.f, 1.f);
    ValueType backgroundColor = ColorValue(0.067f, 0.082f, 0.129f, 0.94f);
    ValueType colorByHealth = BoolValue(true);
    ValueType healthColor = ColorValue(0.2f, 0.83f, 0.4f, 1.f);

    // The player you last hit. Only the runtime id is kept, the actor is looked up again every frame.
    uint64_t targetId = 0;
    bool hasTarget = false;
    std::chrono::steady_clock::time_point lastHit {};

    SDK::Actor* findTarget() const;
    bool gatherInfo(bool preview, TargetInfo& out);
    std::string getFaceTexturePath(SDK::Player* player) const;
};
