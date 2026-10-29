#include "pch.h"
#include "Minecraft.h"
#include "util/RttiLog.h"
#include <cstring>

namespace {
    bool gameSessionFound = false;
    unsigned gameSessionAttempts = 0;
    bool levelFound = false;
    unsigned levelAttempts = 0;
    unsigned minecraftAttempts = 0;
}

void SDK::Minecraft::calibrate(Minecraft* mc) {
    // The object's fields are only filled in once a world exists, so this is retried a limited number of times.
    if (gameSessionFound || ++minecraftAttempts > 3000) return;
    if (minecraftAttempts == 1 || minecraftAttempts == 600) rtti::logPointerMap("Minecraft", mc, 0, 0x400);
    const size_t off = rtti::findPointerField(mc, offGameSession, 0x200, "GameSession");
    if (off) {
        Logger::Info("[Offsets] Minecraft.gameSession {}: +0x{:X}", off == offGameSession ? "at" : "moved to", off);
        // timer sits 0x18 after gameSession; assume it moved by the same amount
        offTimer = 0xE0 + (off - 0xC8);
        offGameSession = off;
        gameSessionFound = true;
        rtti::logPointerMap("Minecraft", mc, 0, 0x400);
    }
}

void SDK::GameSession::calibrate(GameSession* session) {
    if (levelFound || ++levelAttempts > 3000) return;
    static const char* const needles[] = {"=Level", "=MultiPlayerLevel", "=ClientLevel"};
    if (levelAttempts == 1) rtti::logPointerMap("GameSession", session, 0, 0x100);
    const size_t off = rtti::findPointerField(session, offLevel, 0x100, needles, 3);
    if (off) {
        Logger::Info("[Offsets] GameSession.level {}: +0x{:X}", off == offLevel ? "at" : "moved to", off);
        levelMoved = off != 0x40;
        offLevel = off;
        levelFound = true;
    }
}

SDK::Level* SDK::Minecraft::getLevel() {
    void* session = nullptr;
    if (!rtti::readPtr(reinterpret_cast<char*>(this) + offGameSession, &session) || !session) return nullptr;
    if (!gameSessionFound && minecraftAttempts <= 600) {
        // Don't follow a pointer that isn't a GameSession.
        char name[128];
        if (!rtti::className(session, name, sizeof(name)) || !std::strstr(name, "GameSession")) return nullptr;
    }
    return static_cast<GameSession*>(session)->getLevel();
}

SDK::Level* SDK::GameSession::getLevel() {
    calibrate(this);
    const auto base = reinterpret_cast<char*>(this);
    if (!levelMoved) {
        uint8_t flag = 0;
        if (!rtti::readByte(base + 0x28, &flag) || flag != 1) return nullptr;

        void* levelControlBlock = nullptr;
        uint8_t controlByte = 0;
        if (!rtti::readPtr(base + 0x30, &levelControlBlock) || !levelControlBlock ||
            !rtti::readByte(levelControlBlock, &controlByte) || controlByte != 1) {
            return nullptr;
        }
    }

    void* level = nullptr;
    if (!rtti::readPtr(base + offLevel, &level) || !level) return nullptr;
    if (!levelFound && levelAttempts <= 600) {
        char name[128];
        if (!rtti::className(level, name, sizeof(name)) || !std::strstr(name, "Level")) return nullptr;
    }
    return static_cast<Level*>(level);
}
