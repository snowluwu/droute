#pragma once

#include "src/hook/hooks.hpp"

namespace droute {

    // the flow id distinguishes a socket from a later reuse of the same handle
    struct UdpFlow {
        SOCKET socket;
        uint64_t flowId;

        bool operator<(const UdpFlow& other) const {
            if (socket != other.socket)
                return socket < other.socket;
            return flowId < other.flowId;
        }
    };

    void QueueUdpAssociation(SOCKET socket);
    int ConnectUdpSocket(SOCKET socket, const sockaddr* address, int length);
    bool GetUdpPeer(SOCKET socket, sockaddr_in& peer);
    bool IsTrackedUdpSocket(SOCKET socket);
    BOOL WINAPI Mine_CancelIoEx(HANDLE file, LPOVERLAPPED overlapped);
    void MarkUdpAssociationPending(SOCKET s, SOCKET expectedControlSocket = INVALID_SOCKET,
                                   const char* reason = "unspecified", int error = 0);

    // caller must hold the state lock
    void LogUdpSummary(SOCKET s, UdpAssociation& association, const char* event);

}
