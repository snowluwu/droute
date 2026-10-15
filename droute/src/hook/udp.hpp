#pragma once

#include "src/hook/hooks.hpp"

namespace droute {

    bool TryUdpAssociate(UdpAssociation& out, uint64_t deadline = 0);
    void MarkUdpAssociationPending(SOCKET s, SOCKET expectedControlSocket = INVALID_SOCKET,
                                   const char* reason = "unspecified", int error = 0);

    // caller must hold g_stateMutex.
    void LogUdpAssociationStart(SOCKET s, UdpAssociation& association, const char* trigger);
    void LogUdpAssociationResult(SOCKET s, UdpAssociation& association, bool ok, int error, const char* stage);
    void LogUdpSummary(SOCKET s, UdpAssociation& association, const char* event);

}
