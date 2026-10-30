#include "pch.h"
#include "ClientInstance.h"

#include "Platform_GameCore.h"
#include "MinecraftGame.h"
#include "mc/common/world/Minecraft.h"
#include "util/RttiLog.h"

SDK::ClientInstance* SDK::ClientInstance::instance = nullptr;

namespace {
    SDK::MinecraftGame* calibrationGame = nullptr;
    bool calibrationDone = false;
    unsigned calibrationAttempts = 0;
}

SDK::ClientInstance* SDK::ClientInstance::get() {
    if (!instance) {
        const auto platform = Platform_GameCore::get();
        if (!platform) {
            return nullptr;
        }

        const auto mcgame = platform->getMinecraftGame();
        if (!mcgame) {
            return nullptr;
        }

        instance = mcgame->getPrimaryClientInstance();
        calibrationGame = mcgame;
    }
    if (instance && !calibrationDone) {
        // The game fills ClientInstance in over time, so retry until every field has been found: on every call at
        // first (so users of the fields see correct offsets), then less often.
        static unsigned calls = 0;
        calls++;
        if (calibrationAttempts < 200 || calls % 30 == 0) {
            calibrate(instance, calibrationGame);
        }
    }
    return instance;
}

void SDK::ClientInstance::calibrate(ClientInstance* inst, MinecraftGame* game) {
    // The game has no usable RTTI for these classes, so only minecraftGame can be found (it is a pointer we already
    // know). The members next to it moved by the same amount; that shift is assumed for them.
    calibrationAttempts++;
    if (calibrationAttempts == 1) {
        const size_t found = rtti::findPointerField(inst, offMinecraftGame, 0x400, "", game);
        if (found) {
            const ptrdiff_t delta = static_cast<ptrdiff_t>(found) - static_cast<ptrdiff_t>(offMinecraftGame);
            Logger::Info("[Offsets] ClientInstance.minecraftGame {}: +0x{:X}", delta ? "moved to" : "at", found);
            if (delta != 0) {
                offMinecraftGame = found;
                offMinecraft += delta;
                offLevelRenderer += delta;
                offPacketSender += delta;
                offInputHandler += delta;
                Logger::Info("[Offsets] assuming minecraft=+0x{:X} levelRenderer=+0x{:X} packetSender=+0x{:X} "
                             "inputHandler=+0x{:X} (same shift)",
                             offMinecraft, offLevelRenderer, offPacketSender, offInputHandler);
            }
        } else {
            Logger::Warn("[Offsets] ClientInstance.minecraftGame not found");
        }
        rtti::logPointerMap("ClientInstance", inst, 0x100, 0x1000);
        rtti::logVtable("ClientInstance", inst, 0, 0x50);
        rtti::logVtable("MinecraftGame", game, 0, 0x50);
    }

    void* mc = nullptr;
    if (rtti::readPtr(reinterpret_cast<char*>(inst) + offMinecraft, &mc) && mc) {
        Minecraft::calibrate(static_cast<Minecraft*>(mc));
    }
    MinecraftGame::calibrate(game);

    if (calibrationAttempts >= 200) {
        calibrationDone = true;
        rtti::logPointerMap("MinecraftGame", game, 0x800, 0x1600);
    }
}

SDK::BlockSource* SDK::ClientInstance::getRegion() {
    // The virtual function index (0x1E) is not known for 1.26.60, and calling the wrong slot is unsafe.
    return nullptr;
}

SDK::LocalPlayer* SDK::ClientInstance::getLocalPlayer() {
    // The virtual function index (0x1F) is not known for 1.26.60. Until the pointer's field is found from a hook that
    // receives the player, report "no player" instead of calling an unknown function.
    if (offLocalPlayer < 0) return nullptr;
    void* p = nullptr;
    if (!rtti::readPtr(reinterpret_cast<char*>(this) + offLocalPlayer, &p) || !p) return nullptr;
    // Only hand out something that looks like a live game object (starts with a vtable in the game image).
    void* vt = nullptr;
    if (!rtti::plausiblePtr(reinterpret_cast<uintptr_t>(p)) || !rtti::readPtr(p, &vt) ||
        !rtti::inImage(reinterpret_cast<uintptr_t>(vt)))
        return nullptr;
    return static_cast<LocalPlayer*>(p);
}

bool SDK::ClientInstance::calibrateLocalPlayer(void* player) {
    if (offLocalPlayer >= 0) return true;
    if (!instance || !player) return false;
    ptrdiff_t first = -1;
    for (size_t off = 0; off < 0x1800; off += 8) {
        void* p = nullptr;
        if (!rtti::readPtr(reinterpret_cast<char*>(instance) + off, &p)) break;
        if (p == player) {
            Logger::Info("[Offsets] LocalPlayer 0x{:X} is stored in ClientInstance at +0x{:X}",
                         reinterpret_cast<uintptr_t>(player), off);
            if (first < 0) first = static_cast<ptrdiff_t>(off);
        }
    }
    if (first < 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Logger::Warn("[Offsets] LocalPlayer 0x{:X} was not found inside ClientInstance",
                         reinterpret_cast<uintptr_t>(player));
            rtti::logVtable("LocalPlayer", player, 0, 0x120);
            rtti::logPointerMap("LocalPlayer", player, 0, 0x400);
        }
        return false;
    }
    offLocalPlayer = first;
    Logger::Info("[Offsets] ClientInstance.localPlayer at +0x{:X}", first);
    rtti::logVtable("LocalPlayer", player, 0, 0x120);
    rtti::logPointerMap("LocalPlayer", player, 0, 0x400);
    return true;
}

SDK::GuiData* SDK::ClientInstance::getGuiData() {
    return hat::member_at<GuiData*>(this, offGuiData);
}

SDK::Options* SDK::ClientInstance::getOptions() {
    return hat::member_at<Options*>(this, offOptions);
}

SDK::NonOwnerPointer<SDK::SoundPlayerInterface> SDK::ClientInstance::getSoundEngine() {
    NonOwnerPointer<SoundPlayerInterface> soundEngine;

    memory::callVirtual<void, NonOwnerPointer<SoundPlayerInterface>&>(this, 0x13C, soundEngine);
    return soundEngine;
}

SDK::ResourcePackManager& SDK::ClientInstance::getResourcePackManager() {
    return *memory::callVirtual<ResourcePackManager*>(this, 0x60);
}

void SDK::ClientInstance::grabCursor() {
    reinterpret_cast<void(__fastcall*)(void*)>(Signatures::ClientInstance_grabCursor.result)(this);
}

// vtable call:
// 48 8b 80 ? ? ? ? 48 8b ce ff 15 ? ? ? ? 84 db
void SDK::ClientInstance::releaseCursor() {
    reinterpret_cast<void(__fastcall*)(void*)>(Signatures::ClientInstance_releaseCursor.result)(this);
}
