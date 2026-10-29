#include "pch.h"
#include "Minecraft.h"
#include "util/RttiLog.h"
#include <cmath>

// The game has no usable RTTI for these classes, so a moved field is recognised by what it points at instead.

namespace {
    bool timerFound = false;
    bool sessionFound = false;
    unsigned timerAttempts = 0;

    // Timer: { float tps; float ticks; float alpha; float timeScale; float passedTime; }. tps is 20 and
    // alpha is in [0,1].
    bool looksLikeTimer(void* p) {
        float t[5];
        if (!rtti::plausiblePtr(reinterpret_cast<uintptr_t>(p)) || !rtti::readBytes(p, t, sizeof(t))) return false;
        for (float f : t) {
            if (!std::isfinite(f)) return false;
        }
        return t[0] == 20.0f && t[2] >= 0.0f && t[2] <= 1.0001f && t[3] > 0.0f && t[3] <= 16.0f && t[4] >= 0.0f;
    }

    // GameSession (only exists while a world is open): a polymorphic object whose byte at +0x28 is 1, with a
    // valid control block at +0x30 and a polymorphic Level at +0x40.
    bool looksLikeGameSession(void* p) {
        if (!rtti::plausiblePtr(reinterpret_cast<uintptr_t>(p))) return false;
        void* vtable = nullptr;
        if (!rtti::readPtr(p, &vtable) || !rtti::inImage(reinterpret_cast<uintptr_t>(vtable))) return false;
        const auto base = reinterpret_cast<char*>(p);
        uint8_t flag = 0;
        if (!rtti::readByte(base + 0x28, &flag) || flag != 1) return false;
        void* controlBlock = nullptr;
        uint8_t controlByte = 0;
        if (!rtti::readPtr(base + 0x30, &controlBlock) || !controlBlock ||
            !rtti::readByte(controlBlock, &controlByte) || controlByte != 1) {
            return false;
        }
        void* level = nullptr;
        void* levelVtable = nullptr;
        return rtti::readPtr(base + 0x40, &level) && level && rtti::readPtr(level, &levelVtable) &&
               rtti::inImage(reinterpret_cast<uintptr_t>(levelVtable));
    }
}

void SDK::Minecraft::calibrate(Minecraft* mc) {
    if (timerFound || ++timerAttempts > 400) return;
    if (timerAttempts == 1) rtti::logPointerMap("Minecraft", mc, 0, 0x400);
    // Look outwards from where the timer used to be.
    for (size_t d = 0; d <= 0x200; d += 8) {
        for (int sign = 0; sign < 2; sign++) {
            if (sign == 1 && d == 0) continue;
            if (sign == 1 && d > offTimer) continue;
            const size_t off = sign == 0 ? offTimer + d : offTimer - d;
            void* p = nullptr;
            if (rtti::readPtr(reinterpret_cast<char*>(mc) + off, &p) && p && looksLikeTimer(p)) {
                Logger::Info("[Offsets] Minecraft.timer {}: +0x{:X}", off == offTimer ? "at" : "moved to", off);
                offTimer = off;
                timerFound = true;
                return;
            }
        }
    }
}

SDK::Level* SDK::Minecraft::getLevel() {
    const auto base = reinterpret_cast<char*>(this);
    void* session = nullptr;
    if (!sessionFound) {
        // Look for the GameSession (only present in a world), outwards from where it used to be.
        static unsigned scanTick = 0;
        if (++scanTick % 4 == 0) {
            for (size_t d = 0; d <= 0x200 && !sessionFound; d += 8) {
                for (int sign = 0; sign < 2; sign++) {
                    if (sign == 1 && (d == 0 || d > offGameSession)) continue;
                    const size_t off = sign == 0 ? offGameSession + d : offGameSession - d;
                    void* p = nullptr;
                    if (rtti::readPtr(base + off, &p) && p && looksLikeGameSession(p)) {
                        Logger::Info("[Offsets] Minecraft.gameSession {}: +0x{:X}",
                                     off == offGameSession ? "at" : "moved to", off);
                        offGameSession = off;
                        sessionFound = true;
                        rtti::logPointerMap("GameSession", p, 0, 0x100);
                        break;
                    }
                }
            }
        }
        if (!sessionFound) return nullptr;
    }
    if (!rtti::readPtr(base + offGameSession, &session) || !session) return nullptr;
    return static_cast<GameSession*>(session)->getLevel();
}

SDK::Level* SDK::GameSession::getLevel() {
    if (!looksLikeGameSession(this)) return nullptr;

    void* level = nullptr;
    if (!rtti::readPtr(reinterpret_cast<char*>(this) + offLevel, &level)) return nullptr;
    return static_cast<Level*>(level);
}
