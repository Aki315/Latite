#pragma once
#include "../../Module.h"

// Fixes movement not resuming after closing a screen (inventory, chat, pause menu...) while a movement key is
// still held. Opening a screen swallows the key-down, and the game doesn't pick the held key back up until
// you release it and press it again. When the cursor is grabbed again, this re-applies the real keyboard state.
class ModernKeybindHandling : public Module {
public:
    ModernKeybindHandling();
    ~ModernKeybindHandling() override = default;

    void onTick(Event& ev);

private:
    static int keyFor(const char* name, int fallback);

    ValueType fixSprint = BoolValue(true);

    bool wasGrabbed = true;
    bool tracking = false;
};
