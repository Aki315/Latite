#include "pch.h"
#include "ClientInstance.h"

#include "Platform_GameCore.h"
#include "MinecraftGame.h"
#include "mc/common/world/Minecraft.h"
#include "mc/common/world/actor/player/Player.h"
#include "util/RttiLog.h"
#include <cmath>
#include <cstring>
#include <vector>

SDK::ClientInstance* SDK::ClientInstance::instance = nullptr;

namespace {
    SDK::MinecraftGame* calibrationGame = nullptr;
    bool calibrationDone = false;
    unsigned calibrationAttempts = 0;
}

namespace {
    // The game's client map points at a base-class part of ClientInstance, not at the start of the object. That part
    // has its own tiny virtual function table whose first entry is an adjustor thunk ("add rcx, -0x98; jmp ..."), which
    // is how the real start of the object can be found.
    void* rebaseToRealInstance(void* p) {
        if (!p) return p;
        void* vt = nullptr;
        if (!rtti::readPtr(p, &vt) || !rtti::inImage(reinterpret_cast<uintptr_t>(vt))) return p;
        void* fn = nullptr;
        if (!rtti::readPtr(vt, &fn) || !rtti::inImage(reinterpret_cast<uintptr_t>(fn))) return p;
        uint8_t b[8] = {};
        if (!rtti::readBytes(fn, b, sizeof(b))) return p;
        if (!(b[0] == 0x48 && b[1] == 0x81 && b[2] == 0xC1 && b[7] == 0xE9)) return p;
        int32_t adj = 0;
        std::memcpy(&adj, b + 3, 4);
        if (adj >= 0 || adj < -0x1000) return p;
        void* r = reinterpret_cast<char*>(p) + adj;
        void* rvt = nullptr;
        if (!rtti::readPtr(r, &rvt) || !rtti::inImage(reinterpret_cast<uintptr_t>(rvt)) || rvt == vt) return p;
        size_t slots = 0;
        for (; slots < 0x400; slots++) {
            void* f = nullptr;
            if (!rtti::readPtr(reinterpret_cast<char*>(rvt) + slots * 8, &f) ||
                !rtti::inImage(reinterpret_cast<uintptr_t>(f)))
                break;
        }
        Logger::Info("[Offsets] client map entry 0x{:X} is a base-class part; the real ClientInstance is at 0x{:X} "
                     "({} bytes lower, vtable exe+0x{:X} with {} slots)",
                     reinterpret_cast<uintptr_t>(p), reinterpret_cast<uintptr_t>(r), -adj,
                     reinterpret_cast<uintptr_t>(rvt) - rtti::imageBase(), slots);
        return slots >= 0x40 ? r : p;
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

        instance = static_cast<ClientInstance*>(rebaseToRealInstance(mcgame->getPrimaryClientInstance()));
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
        rtti::logPointerMap("ClientInstance", inst, 0x100, 0x1400);
        rtti::logVtable("ClientInstance", inst, 0, 0x60);
        rtti::logVtable("MinecraftGame", game, 0, 0x50);
    }

    void* mc = nullptr;
    if (rtti::readPtr(reinterpret_cast<char*>(inst) + offMinecraft, &mc) && mc) {
        Minecraft::calibrate(static_cast<Minecraft*>(mc));
    }
    MinecraftGame::calibrate(game);
    calibrateGuiData(inst);

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
    // ClientInstance has no virtual function table of its own in 1.26.60 (it used to have getLocalPlayer in slot
    // 0x1F). The player's field is found by looking for the object that holds the game's PacketSender pointer, or from
    // a hook that receives the player. Until then, report "no player".
    if (offLocalPlayer < 0) {
        static unsigned calls = 0;
        if (++calls % 300 == 1) discoverLocalPlayer();
        if (offLocalPlayer < 0) return nullptr;
    }
    void* p = nullptr;
    if (!rtti::readPtr(reinterpret_cast<char*>(this) + offLocalPlayer, &p) || !p) return nullptr;
    // Only hand out something that looks like a live game object (starts with a vtable in the game image).
    void* vt = nullptr;
    if (!rtti::plausiblePtr(reinterpret_cast<uintptr_t>(p)) || !rtti::readPtr(p, &vt) ||
        !rtti::inImage(reinterpret_cast<uintptr_t>(vt)))
        return nullptr;
    return static_cast<LocalPlayer*>(p);
}

void SDK::ClientInstance::discoverLocalPlayer() {
    if (offLocalPlayer >= 0 || !instance) return;
    void* packetSender = nullptr;
    if (!rtti::readPtr(reinterpret_cast<char*>(instance) + offPacketSender, &packetSender) || !packetSender) return;

    static std::vector<uint8_t> block;
    block.resize(0x1800);
    struct Candidate {
        size_t field;
        size_t holds;
        size_t vtableSlots;
    };
    std::vector<Candidate> found;
    for (size_t off = 0x100; off < 0x1400; off += 8) {
        void* p = nullptr;
        if (!rtti::readPtr(reinterpret_cast<char*>(instance) + off, &p) || !p) continue;
        if (!rtti::plausiblePtr(reinterpret_cast<uintptr_t>(p)) || p == packetSender) continue;
        void* vt = nullptr;
        if (!rtti::readPtr(p, &vt) || !rtti::inImage(reinterpret_cast<uintptr_t>(vt))) continue;
        if (!rtti::readBytes(p, block.data(), block.size())) continue;
        for (size_t d = 8; d + 8 <= block.size(); d += 8) {
            void* v;
            std::memcpy(&v, block.data() + d, 8);
            if (v != packetSender) continue;
            size_t slots = 0;
            for (; slots < 0x400; slots++) {
                void* fn = nullptr;
                if (!rtti::readPtr(reinterpret_cast<char*>(vt) + slots * 8, &fn) ||
                    !rtti::inImage(reinterpret_cast<uintptr_t>(fn)))
                    break;
            }
            found.push_back({ off, d, slots });
            break;
        }
    }
    static unsigned reports = 0;
    if (found.empty()) {
        if (reports++ < 2) Logger::Info("[Offsets] no ClientInstance field points at an object holding the PacketSender");
        return;
    }
    const Candidate* best = &found[0];
    for (auto& c : found) {
        Logger::Info("[Offsets] ClientInstance+0x{:X} -> object holding the PacketSender at +0x{:X}, vtable has {} slots",
                     c.field, c.holds, c.vtableSlots);
        if (c.vtableSlots > best->vtableSlots) best = &c;
    }
    // A player/actor class has a long virtual function table; the other holders do not.
    if (best->vtableSlots >= 0x80) {
        offLocalPlayer = static_cast<ptrdiff_t>(best->field);
        Player::offPacketSender = best->holds;
        Logger::Info("[Offsets] ClientInstance.localPlayer taken from +0x{:X}; Player.packetSender at +0x{:X}",
                     best->field, best->holds);
        void* p = nullptr;
        rtti::readPtr(reinterpret_cast<char*>(instance) + best->field, &p);
        if (p) {
            rtti::logVtable("LocalPlayer", p, 0, 0x120);
            rtti::logPointerMap("LocalPlayer", p, 0, 0x400);
        }
    }
}

bool SDK::ClientInstance::calibrateLocalPlayer(void* player) {
    if (offLocalPlayer >= 0) return true;
    if (!instance || !player) return false;
    ptrdiff_t first = -1;
    for (size_t off = 0; off < 0x1800; off += 8) {  // (also used when a hook hands us the player)
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

namespace {
    // GuiData keeps the window size, the gui size and the scale as floats, and they must agree with each other.
    bool looksLikeGuiData(void* g) {
        float f[0x70 / 4] = {};
        if (!g || !rtti::readBytes(g, f, sizeof(f))) return false;
        const float sx = f[0x40 / 4], sy = f[0x44 / 4], gx = f[0x50 / 4], gy = f[0x54 / 4];
        const float sc = f[0x5C / 4], fr = f[0x60 / 4];
        return sx >= 200.f && sx <= 20000.f && sy >= 200.f && sy <= 20000.f && sc >= 0.5f && sc <= 16.f && fr > 0.f &&
               std::fabs(fr * sc - 1.f) < 0.02f && std::fabs(gx * sc - sx) < 3.f && std::fabs(gy * sc - sy) < 3.f;
    }
    bool guiDataValidated = false;
}

void SDK::ClientInstance::calibrateGuiData(ClientInstance* inst) {
    if (guiDataValidated) return;
    void* g = nullptr;
    rtti::readPtr(reinterpret_cast<char*>(inst) + offGuiData, &g);
    if (looksLikeGuiData(g)) {
        guiDataValidated = true;
        Logger::Info("[Offsets] GuiData found at ClientInstance+0x{:X}", offGuiData);
        return;
    }
    for (size_t off = 0x100; off < 0x1000; off += 8) {
        void* p = nullptr;
        if (!rtti::readPtr(reinterpret_cast<char*>(inst) + off, &p) || !p) continue;
        if (!rtti::plausiblePtr(reinterpret_cast<uintptr_t>(p))) continue;
        if (looksLikeGuiData(p)) {
            offGuiData = off;
            guiDataValidated = true;
            Logger::Info("[Offsets] GuiData moved to ClientInstance+0x{:X}", off);
            return;
        }
    }
    static unsigned reports = 0;
    if (reports++ == 0 && g) {
        float f[0x70 / 4] = {};
        rtti::readBytes(g, f, sizeof(f));
        Logger::Warn("[Offsets] GuiData not found. Floats at ClientInstance+0x{:X}'s object, from +0x30: "
                     "{} {} {} {} | {} {} {} {} | {} {} {} {} | {} {} {} {}",
                     offGuiData, f[0x30 / 4], f[0x34 / 4], f[0x38 / 4], f[0x3C / 4], f[0x40 / 4], f[0x44 / 4],
                     f[0x48 / 4], f[0x4C / 4], f[0x50 / 4], f[0x54 / 4], f[0x58 / 4], f[0x5C / 4], f[0x60 / 4],
                     f[0x64 / 4], f[0x68 / 4], f[0x6C / 4]);
    }
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
