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
    int armorDamage[4] { -1, -1, -1, -1 }; // Estimated durability lost per armor piece, -1 = no estimate.
    SDK::ItemStack* offhand = nullptr;
    uint64_t id = 0;      // The player's runtime id, 0 for the editor preview.
    float opacity = 1.f;  // Fades from 1 to 0 over the last moments before the HUD goes away.
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
    // Durability bars are separate for armor and for the offhand item (a shield). What the server sends about other
    // players' armor can be out of date, so the armor bar can use an estimate built from the hits you land.
    ValueType showArmorDurability = BoolValue(true);
    ValueType estimateArmor = BoolValue(true);
    ValueType assumeProtection = BoolValue(true);  // They wear Protection IV (4 on each piece).
    ValueType assumeSharpness = BoolValue(true);   // Your sword or axe has Sharpness V.
    ValueType assumeUnbreaking = BoolValue(true);  // Their armor has Unbreaking III.
    static constexpr int StrengthNone = 0;
    static constexpr int StrengthI = 1;
    static constexpr int StrengthII = 2;
    EnumData assumeStrength; // Which Strength potion you have on: none, I or II (+3 damage per level).
    ValueType showShieldDurability = BoolValue(true);
    static constexpr int ShieldNumberRemaining = 0; // 312
    static constexpr int ShieldNumberPercent = 1;   // 93%
    static constexpr int ShieldNumberOff = 2;
    EnumData shieldNumber;
    static constexpr int ArmorNumberOff = 0;
    static constexpr int ArmorNumberPercent = 1;   // 93%
    static constexpr int ArmorNumberRemaining = 2; // 312
    EnumData armorNumber;
    ValueType showAbsorption = BoolValue(true);
    ValueType heartSize = FloatValue(2.f); // Size of one heart pixel; 2 = the original size. Whole numbers keep it crisp.
    ValueType itemSize = FloatValue(1.3f); // Size of the armor, offhand and item icons; 1 = the original size.
    ValueType textSize = FloatValue(20.f);
    ValueType textColor = ColorValue(1.f, 1.f, 1.f, 1.f);
    ValueType backgroundColor = ColorValue(0.067f, 0.082f, 0.129f, 0.94f);
    ValueType colorByHealth = BoolValue(true);
    ValueType healthColor = ColorValue(0.2f, 0.83f, 0.4f, 1.f);

    // The player you last hit. Only the runtime id is kept, the actor is looked up again every frame.
    uint64_t targetId = 0;
    bool hasTarget = false;
    std::chrono::steady_clock::time_point lastHit {};

    // The health bar and ring ease toward the real value instead of jumping.
    float shownFraction = 1.f;
    uint64_t shownFor = 0;
    std::chrono::steady_clock::time_point lastFrame {};

    // The armor durability guess for the current target. It starts from what the server sent and then adds the wear
    // from every hit you land: each hit wears every armor piece by max(1, floor(damage / 4)).
    struct ArmorGuess {
        bool known = false;
        std::string id;
        int maxDamage = 0;
        int lastServerDamage = 0;
        int baseDamage = 0; // What the server last said, the guess builds on this.
        float wear = 0.f;   // Extra durability lost since then, from the hits.
    };
    ArmorGuess armorGuess[4];
    uint64_t guessFor = 0;
    float guessLastHealth = -1.f;
    float lastWeaponBase = -1.f;   // Base damage of what you swung at the last hit, -1 = unknown weapon.
    bool lastWeaponSharpable = false;
    void updateArmorEstimate(float totalHealth, TargetInfo& info);

    SDK::Actor* findTarget() const;
    bool gatherInfo(bool preview, TargetInfo& out);
    std::string getFaceTexturePath(SDK::Player* player) const;
};
