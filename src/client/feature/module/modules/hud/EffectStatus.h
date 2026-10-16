#pragma once
#include "../../TextModule.h"

// Lists your active potion effects (name, level, time left). It is a TextModule, so the position, size,
// background and colors are all edited in Latite's HUD editor like any other HUD module.
//
// Optionally it draws the game's own effect icon in front of every line, and it can hide Minecraft's built-in
// effect display so the effects are not shown twice.
class EffectStatus : public TextModule {
public:
    EffectStatus();
    ~EffectStatus() override = default;

    // Icons are game textures, so they can only be drawn with the Minecraft renderer.
    [[nodiscard]] bool forceMinecraftRenderer() override;

    void render(DrawUtil& ctx, bool isDefault, bool inEditor) override;

    // Hides / restores Minecraft's own effect display on the HUD screen.
    void onRenderLayer(Event& ev);

protected:
    std::wstringstream text(bool isDefault, bool inEditor) override;

private:
    void renderWithIcons(MCDrawUtil& dc, bool isDefault, bool inEditor);

    ValueType showLevel = BoolValue(true);
    ValueType showTime = BoolValue(true);
    ValueType showIcons = BoolValue(true);
    ValueType hideVanilla = BoolValue(true);
    ValueType debug = BoolValue(false);

    bool vanillaHidden = false;
    Vec2 savedVanillaPos = {};
};
