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

    // true once the field is found (the object might not have filled it in yet; the caller retries)
    bool findField(const char* label, const void* base, size_t& var, size_t window, const char* needle,
                   const void* exact = nullptr) {
        const size_t found = rtti::findPointerField(base, var, window, needle, exact);
        if (!found) return false;
        if (found != var) {
            Logger::Info("[Offsets] {} moved: +0x{:X} -> +0x{:X}", label, var, found);
        } else {
            Logger::Info("[Offsets] {} at +0x{:X}", label, found);
        }
        var = found;
        return true;
    }
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
    struct Field {
        const char* label;
        size_t* var;
        size_t def;
        const char* needle;
        const void* exact;
        bool provisional; // may be guessed from how far minecraftGame moved
        bool found;
        bool guessed;
    };
    static Field fields[] = {
        {"ClientInstance.minecraftGame", &offMinecraftGame, 0x1A8, "=MinecraftGame", nullptr, false, false, false},
        {"ClientInstance.minecraft", &offMinecraft, 0x1B0, "=Minecraft", nullptr, true, false, false},
        {"ClientInstance.levelRenderer", &offLevelRenderer, 0x1C0, "=LevelRenderer", nullptr, true, false, false},
        {"ClientInstance.packetSender", &offPacketSender, 0x1D0, "PacketSender", nullptr, true, false, false},
        {"ClientInstance.inputHandler", &offInputHandler, 0x1E0, "InputHandler", nullptr, true, false, false},
        {"ClientInstance.guiData", &offGuiData, 0x650, "=GuiData", nullptr, false, false, false},
        {"ClientInstance.options", &offOptions, 0xE10, "Options", nullptr, false, false, false},
    };
    fields[0].exact = game;

    calibrationAttempts++;
    if (calibrationAttempts == 1) {
        rtti::logPointerMap("ClientInstance", inst, 0x100, 0x300);
    }

    bool all = true;
    for (auto& f : fields) {
        if (!f.found) {
            f.found = findField(f.label, inst, *f.var, 0x400, f.needle, f.exact);
            if (f.found) f.guessed = false;
        }
        all = all && f.found;
    }

    // The fields next to minecraftGame normally moved by the same amount. Until RTTI finds them, use that.
    if (fields[0].found) {
        const ptrdiff_t delta = static_cast<ptrdiff_t>(offMinecraftGame) - static_cast<ptrdiff_t>(fields[0].def);
        if (delta != 0) {
            for (auto& f : fields) {
                if (f.provisional && !f.found && !f.guessed) {
                    *f.var = f.def + delta;
                    f.guessed = true;
                    Logger::Info("[Offsets] {} not found yet, guessing +0x{:X} (same shift as minecraftGame)", f.label,
                                 *f.var);
                }
            }
        }
    }

    if (fields[1].found || fields[1].guessed) {
        void* mc = nullptr;
        if (rtti::readPtr(reinterpret_cast<char*>(inst) + offMinecraft, &mc) && mc) {
            Minecraft::calibrate(static_cast<Minecraft*>(mc));
        }
    }
    if (!MinecraftGame::calibrate(game)) {
        all = false;
    }

    if (all || calibrationAttempts >= 200) {
        calibrationDone = true;
        if (!all) {
            for (auto& f : fields) {
                if (!f.found) Logger::Warn("[Offsets] {} not found, using +0x{:X}", f.label, *f.var);
            }
        }
        rtti::logPointerMap("ClientInstance", inst, 0x100, 0x1000);
        rtti::logPointerMap("MinecraftGame", game, 0x800, 0x1600);
    }
}

SDK::BlockSource* SDK::ClientInstance::getRegion() {
    return memory::callVirtual<BlockSource*>(this, 0x1E);
}

SDK::LocalPlayer* SDK::ClientInstance::getLocalPlayer() {
    return memory::callVirtual<LocalPlayer*>(this, 0x1F);
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
