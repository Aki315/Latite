#pragma once
#include "mc/Util.h"
#include "NetherNetConnector.h"
#include "RakNetConnector.h"

namespace SDK {
    class RemoteConnectorComposite {
    public:
        static RemoteConnectorComposite* get();
        static Social::GameConnectionInfo* getConnectionInfo();

        RemoteConnector* getActiveConnector();

        static inline size_t offNetherNetConnector = 0x68;
        static inline size_t offRakNetConnector = 0x70;
        CLASS_FIELD_DYN(NetherNetConnector*, netherNetConnector, offNetherNetConnector);
        CLASS_FIELD_DYN(RakNetConnector*, rakNetConnector, offRakNetConnector);
    };
}
