#include "client/Latite.h"
#include "client/input/Keyboard.h"
#include "client/event/events/TickEvent.h"

#include "client/Latite.h"
#include "client/event/events/TickEvent.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/game/MinecraftGame.h"
#include "mc/common/client/input/ClientInputHandler.h"
#include "mc/common/world/actor/player/Player.h"

ModernKeybindHandling::ModernKeybindHandling()
    : Module("ModernKeybindHandling", LocalizeString::get("client.module.modernKeybindHandling.name"),
             LocalizeString::get("client.module.modernKeybindHandling.desc"), GAME, nokeybind) {
    addSetting("fixSprint", LocalizeString::get("client.module.modernKeybindHandling.fixSprint.name"),
               LocalizeString::get("client.module.modernKeybindHandling.fixSprint.desc"), fixSprint);

    listen<TickEvent>(static_cast<EventListenerFunc>(&ModernKeybindHandling::onTick));
}

// Looks up the key the player bound to an action, falling back to the default key if the lookup fails.
int ModernKeybindHandling::keyFor(const char* name, int fallback) {
    const int key = Latite::getKeyboard().getMappedKey(name);
    return (key > 0 && key < 0x100) ? key : fallback;
}

void ModernKeybindHandling::onTick(Event&) {
    auto* ci = SDK::ClientInstance::get();
    if (!ci || !ci->minecraftGame) return;

    auto* plr = ci->getLocalPlayer();
    if (!plr) {
        wasGrabbed = true;
        tracking = false;
        return;
    }

    // The cursor is released while any screen is open and grabbed again when you are back in the game.
    const bool grabbed = ci->minecraftGame->isCursorGrabbed();
    if (grabbed && !wasGrabbed) tracking = true;
    wasGrabbed = grabbed;
    if (!grabbed || !tracking) return;

    auto* input = plr->getMoveInputComponent();
    if (!input) return;

    auto& kb = Latite::getKeyboard();
    const bool forward = kb.isKeyDown(keyFor("forward", 'W'));
    const bool back = kb.isKeyDown(keyFor("back", 'S'));
    const bool left = kb.isKeyDown(keyFor("left", 'A'));
    const bool right = kb.isKeyDown(keyFor("right", 'D'));
    const bool jump = kb.isKeyDown(keyFor("jump", VK_SPACE));
    const bool sneak = kb.isKeyDown(keyFor("sneak", VK_SHIFT));
    const bool sprint = kb.isKeyDown(keyFor("sprint", VK_CONTROL));

    // Set the state both ways, so a key released while the screen was open doesn't keep you walking either.
    input->rawInputState.up = forward;
    input->inputState.up = forward;
    input->rawInputState.down = back;
    input->inputState.down = back;
    input->rawInputState.left = left;
    input->inputState.left = left;
    input->rawInputState.right = right;
    input->inputState.right = right;
    input->rawInputState.jumpDown = jump;
    input->inputState.jumpDown = jump;
    input->rawInputState.sneakDown = sneak;
    input->inputState.sneakDown = sneak;

    // Sprint is only ever turned on, never forced off, so it doesn't fight Toggle Sprint.
    if (std::get<BoolValue>(fixSprint) && sprint) {
        input->rawInputState.sprintDown = true;
        input->inputState.sprintDown = true;
    }

    // Keep re-applying while a key is still held; once everything is released the game is back in sync.
    if (!(forward || back || left || right || jump || sneak || sprint)) tracking = false;
}
