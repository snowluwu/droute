#pragma once

#include "src/hook/hooks.hpp"

namespace droute {

    bool TryUdpAssociate(UdpAssociation& out, uint64_t deadline = 0);
    void MarkUdpAssociationPending(SOCKET s, SOCKET expectedControlSocket = INVALID_SOCKET);

}
