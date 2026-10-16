#pragma once
#include "../../TextModule.h"

// Lists your active potion effects (name, level, time left). It is a TextModule, so the position, size,
// background and colors are all edited in Latite's HUD editor like any other HUD module.
class EffectStatus : public TextModule {
public:
    EffectStatus();
    ~EffectStatus() override = default;

protected:
    std::wstringstream text(bool isDefault, bool inEditor) override;

private:
    ValueType showLevel = BoolValue(true);
    ValueType showTime = BoolValue(true);
    ValueType debug = BoolValue(false);
};
