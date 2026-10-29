#pragma once
#include "mc/Util.h"
#include "NetworkSystem.h"
#include "Packet.h"

namespace SDK {
    class PacketSender {
        virtual ~PacketSender() = 0;

    public:
        virtual bool isInitialized() = 0;
        virtual void send(Packet*) = 0;
        virtual void sendTo(void* networkIdentifier, uint8_t subClientId, Packet* pkt);
        virtual void sendToServer(Packet* pkt) = 0;

        static inline size_t offNetworkSystem = 0x20;
        CLASS_FIELD_DYN(NetworkSystem*, networkSystem, offNetworkSystem);
    };
}
