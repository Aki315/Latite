#pragma once
#include "mc/Util.h"
#include "RemoteConnectorComposite.h"

namespace SDK {
    class NetworkSystem {
    public:
        static inline size_t offRemoteConnector = 0xF8;
        CLASS_FIELD_DYN(RemoteConnectorComposite*, remoteConnector, offRemoteConnector);
    };
}
