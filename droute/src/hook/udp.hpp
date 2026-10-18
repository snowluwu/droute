#pragma once

#include "src/hook/hooks.hpp"

namespace droute {

    bool TryUdpAssociate(UdpAssociation& out, uint64_t deadline = 0);
    void QueueUdpAssociation(SOCKET socket);
    int ConnectUdpSocket(SOCKET socket, const sockaddr* address, int length);
    bool GetUdpPeer(SOCKET socket, sockaddr_in& peer);
    bool IsTrackedUdpSocket(SOCKET socket);
    BOOL WINAPI Mine_CancelIoEx(HANDLE file, LPOVERLAPPED overlapped);
    void MarkUdpAssociationPending(SOCKET s, SOCKET expectedControlSocket = INVALID_SOCKET,
                                   const char* reason = "unspecified", int error = 0);

    bool ActivateUdpRelay(SOCKET s, std::unique_lock<std::shared_mutex>& lock,
                          std::map<SOCKET, UdpAssociation>::iterator& association, uint64_t deadline,
                          const char* trigger);

    // caller must hold the state lock
    void LogUdpAssociationStart(SOCKET s, const UdpAssociation& association, const char* trigger);
    void LogUdpAssociationResult(SOCKET s, const UdpAssociation& association, bool ok, int error, const char* stage);
    void LogUdpSummary(SOCKET s, UdpAssociation& association, const char* event);

}
