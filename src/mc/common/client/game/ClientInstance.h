#pragma once
#include "mc/Util.h"
#include "mc/common/utility/NonOwnerPointer.h"
#include "util/LMath.h"

namespace SDK {
    class ClientInstance {
    public:
        // Field offsets. They are re-found from the game's RTTI when ClientInstance is first obtained (calibrate),
        // because game updates keep moving them.
        static inline size_t offMinecraftGame = 0x1A8;
        static inline size_t offMinecraft = 0x1B0;
        static inline size_t offLevelRenderer = 0x1C0;
        static inline size_t offPacketSender = 0x1D0;
        static inline size_t offInputHandler = 0x1E0;
        static inline size_t offGuiData = 0x650;
        static inline size_t offOptions = 0xE10;
        // Where the LocalPlayer pointer sits in ClientInstance; -1 until found (see calibrateLocalPlayer).
        static inline ptrdiff_t offLocalPlayer = -1;

        CLASS_FIELD_DYN(class MinecraftGame*, minecraftGame, offMinecraftGame);
        CLASS_FIELD_DYN(class Minecraft*, minecraft, offMinecraft);
        CLASS_FIELD_DYN(class LevelRenderer*, levelRenderer, offLevelRenderer);
        CLASS_FIELD_DYN(class PacketSender*, packetSender, offPacketSender);
        CLASS_FIELD_DYN(class ClientInputHandler*, inputHandler, offInputHandler);
        CLASS_FIELD(struct Vec2, cursorPos, 0x588);

        static ClientInstance* get();
        // Called from a hook that receives the LocalPlayer, to find where ClientInstance keeps it. True once found.
        static bool calibrateLocalPlayer(void* player);
        class BlockSource* getRegion();
        class LocalPlayer* getLocalPlayer();
        class GuiData* getGuiData();
        class Options* getOptions();
        NonOwnerPointer<class SoundPlayerInterface> getSoundEngine();
        class ResourcePackManager& getResourcePackManager();
        // class ClientHMDState* getClientHMDState(); Removed in 1.21.111

        void grabCursor();
        void releaseCursor();

    private:
        static void discoverLocalPlayer();
        static void calibrateGuiData(ClientInstance* inst);
        static void calibrate(ClientInstance* inst, class MinecraftGame* game);
        static ClientInstance* instance;
    };
}
