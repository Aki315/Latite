#include "pch.h"
#include "ClientInstance.h"

#include "Platform_GameCore.h"
#include "MinecraftGame.h"
#include "util/Rtti.h"

SDK::ClientInstance* SDK::ClientInstance::instance = nullptr;

namespace {
    SDK::MinecraftGame* calibrationGame = nullptr;
    bool calibrationDone = false;
    unsigned calibrationCalls = 0;
    unsigned calibrationAttempts = 0;

    void surveyObject(const char* label, const void* base, size_t start, size_t end) {
        Logger::Info("[Offsets] {} object map:", label);
        for (size_t off = start; off < end; off += 8) {
            void* p = nullptr;
            char name[128];
            if (rtti::readPtr(reinterpret_cast<const char*>(base) + off, &p) && p &&
                rtti::className(p, name, sizeof(name))) {
                Logger::Info("[Offsets]   {}+0x{:X} -> {}", label, off, name);
            }
        }
    }

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
        // The game fills ClientInstance in over time, so retry (throttled) until every field has been found.
        calibrationCalls++;
        if (calibrationCalls == 1 || calibrationCalls % 30 == 0) {
            calibrate(instance, calibrationGame);
        }
    }
    return instance;
}

void SDK::ClientInstance::calibrate(ClientInstance* inst, MinecraftGame* game) {
    struct Field {
        const char* label;
        size_t* var;
        const char* needle;
        const void* exact;
        bool found;
    };
    static Field fields[] = {
        {"ClientInstance.minecraftGame", &offMinecraftGame, "=MinecraftGame", nullptr, false},
        {"ClientInstance.minecraft", &offMinecraft, "=Minecraft", nullptr, false},
        {"ClientInstance.levelRenderer", &offLevelRenderer, "=LevelRenderer", nullptr, false},
        {"ClientInstance.packetSender", &offPacketSender, "PacketSender", nullptr, false},
        {"ClientInstance.inputHandler", &offInputHandler, "InputHandler", nullptr, false},
        {"ClientInstance.guiData", &offGuiData, "=GuiData", nullptr, false},
        {"ClientInstance.options", &offOptions, "Options", nullptr, false},
    };
    fields[0].exact = game;

    calibrationAttempts++;
    bool all = true;
    for (auto& f : fields) {
        if (!f.found) {
            f.found = findField(f.label, inst, *f.var, 0x400, f.needle, f.exact);
        }
        all = all && f.found;
    }
    if (!MinecraftGame::calibrate(game)) {
        all = false;
    }

    if (all || calibrationAttempts >= 40) {
        calibrationDone = true;
        if (!all) {
            for (auto& f : fields) {
                if (!f.found) Logger::Warn("[Offsets] {} not found, keeping +0x{:X}", f.label, *f.var);
            }
        }
        surveyObject("ClientInstance", inst, 0x100, 0x1000);
        surveyObject("MinecraftGame", game, 0x800, 0x1600);
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
