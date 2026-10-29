#include "RemoteConnectorComposite.h"
#include "PacketSender.h"
#include "mc/common/client/game/ClientInstance.h"
#include "util/Logger.h"
#include "util/Rtti.h"
#include <cstring>

namespace {
    // Re-finds one field (the object may not have filled it in yet, so this is retried a limited number of times).
    void calibrateField(const char* label, const void* base, size_t& var, size_t window, const char* needle,
                        bool& done, unsigned& tries) {
        if (done || !base) return;
        if (++tries > 600) {
            done = true;
            Logger::Warn("[Offsets] {} not found, keeping +0x{:X}", label, var);
            return;
        }
        const size_t found = rtti::findPointerField(base, var, window, needle);
        if (!found) return;
        done = true;
        if (found != var) {
            Logger::Info("[Offsets] {} moved: +0x{:X} -> +0x{:X}", label, var, found);
        } else {
            Logger::Info("[Offsets] {} at +0x{:X}", label, found);
        }
        var = found;
    }
}

SDK::RemoteConnectorComposite* SDK::RemoteConnectorComposite::get() {
    auto* clientInstance = ClientInstance::get();
    if (!clientInstance) {
        return nullptr;
    }

    static bool psDone = false, rcDone = false, nnDone = false, rkDone = false;
    static unsigned psTries = 0, rcTries = 0, nnTries = 0, rkTries = 0;

    void* packetSender = nullptr;
    if (!rtti::readPtr(reinterpret_cast<char*>(clientInstance) + ClientInstance::offPacketSender, &packetSender) ||
        !packetSender) {
        return nullptr;
    }
    calibrateField("PacketSender.networkSystem", packetSender, PacketSender::offNetworkSystem, 0x100, "NetworkSystem",
                   psDone, psTries);

    void* networkSystem = nullptr;
    if (!rtti::readPtr(reinterpret_cast<char*>(packetSender) + PacketSender::offNetworkSystem, &networkSystem) ||
        !networkSystem) {
        return nullptr;
    }
    calibrateField("NetworkSystem.remoteConnector", networkSystem, NetworkSystem::offRemoteConnector, 0x200,
                   "=RemoteConnectorComposite", rcDone, rcTries);

    void* composite = nullptr;
    if (!rtti::readPtr(reinterpret_cast<char*>(networkSystem) + NetworkSystem::offRemoteConnector, &composite) ||
        !composite) {
        return nullptr;
    }
    // A wrong offset must never be followed: only accept something that really is a RemoteConnectorComposite.
    char name[128];
    if (!rtti::className(composite, name, sizeof(name)) || std::strcmp(name, "RemoteConnectorComposite") != 0) {
        return nullptr;
    }
    calibrateField("RemoteConnectorComposite.netherNetConnector", composite, offNetherNetConnector, 0x100,
                   "NetherNetConnector", nnDone, nnTries);
    calibrateField("RemoteConnectorComposite.rakNetConnector", composite, offRakNetConnector, 0x100, "RakNetConnector",
                   rkDone, rkTries);
    return static_cast<RemoteConnectorComposite*>(composite);
}

SDK::RemoteConnector* SDK::RemoteConnectorComposite::getActiveConnector() {
    void* ownerControlBlock = nullptr;
    void* networkSessionOwner = nullptr;
    uint8_t ownerFlag = 0;
    if (!rtti::readPtr(reinterpret_cast<char*>(this) + 0x50, &ownerControlBlock) ||
        !rtti::readPtr(reinterpret_cast<char*>(this) + 0x60, &networkSessionOwner)) {
        return nullptr;
    }
    if (!ownerControlBlock || !rtti::readByte(ownerControlBlock, &ownerFlag) || !ownerFlag || !networkSessionOwner) {
        return nullptr;
    }

    void* sessionInfo = nullptr;
    rtti::readPtr(reinterpret_cast<char*>(networkSessionOwner) + 0x18, &sessionInfo);
    uint8_t kind[4] = {};
    bool usesNetherNet = false;
    if (sessionInfo) {
        for (int i = 0; i < 4; i++) {
            if (!rtti::readByte(reinterpret_cast<char*>(sessionInfo) + 0x18 + i, &kind[i])) break;
        }
        int value;
        std::memcpy(&value, kind, sizeof(value));
        usesNetherNet = value == 2;
    }
    return usesNetherNet ? static_cast<RemoteConnector*>(netherNetConnector)
                         : static_cast<RemoteConnector*>(rakNetConnector);
}

SDK::Social::GameConnectionInfo* SDK::RemoteConnectorComposite::getConnectionInfo() {
    auto* composite = get();
    if (!composite) return nullptr;

    auto* connector = composite->getActiveConnector();
    return connector ? connector->getConnectedGameInfo() : nullptr;
}
