#include "pch.h"
#include "src/hook/tcp.hpp"
#include "src/hook/hooks.hpp"
#include "src/core/config.hpp"
#include "src/core/logger.hpp"
#include "src/core/utils.hpp"
#include "src/net/socks5.hpp"

namespace droute {

    static std::atomic<uint64_t> g_nextTcpConnection{ 1 };

    int ConnectToProxy(SOCKET s, uint64_t deadline) {
        int rc = Hooks::Real_connect(s, reinterpret_cast<const sockaddr*>(&g_proxyAddr), sizeof(g_proxyAddr));
        if (rc == 0)
            return 0;

        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS)
            return SOCKET_ERROR;

        if (!WaitForConnect(s, RemainingTimeout(deadline))) {
            return SOCKET_ERROR;
        }

        int soErr = 0;
        int soLen = sizeof(soErr);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &soLen) != 0)
            return SOCKET_ERROR;
        if (soErr != 0) {
            WSASetLastError(soErr);
            return SOCKET_ERROR;
        }

        return 0;
    }

    int Socks5ProxyConnect(SOCKET s, const sockaddr_in& target, uint64_t deadline) {
        if (!Socks5Handshake(s, g_cfg.user.c_str(), g_cfg.password.c_str(), deadline)) {
            LOG_WARN("event=tcp.connect.stage_failed socket=%llu stage=socks_greeting_auth wsa_error=%d", (ULONG_PTR)s, WSAGetLastError());
            return SOCKET_ERROR;
        }
        if (!Socks5RequestConnect(s, target, deadline)) {
            LOG_WARN("event=tcp.connect.stage_failed socket=%llu stage=socks_connect wsa_error=%d", (ULONG_PTR)s, WSAGetLastError());
            return SOCKET_ERROR;
        }
        return 0;
    }

    int ConnectViaProxy(SOCKET s, const sockaddr_in* target, uint64_t deadline) {
        if (ConnectToProxy(s, deadline) != 0) {
            LOG_WARN("event=tcp.connect.stage_failed socket=%llu stage=proxy_connect proxy=%s wsa_error=%d", (ULONG_PTR)s, AddrToString(g_proxyAddr).c_str(), WSAGetLastError());
            return SOCKET_ERROR;
        }
        if (Socks5ProxyConnect(s, *target, deadline) != 0) {
            return SOCKET_ERROR;
        }
        return 0;
    }

    int WSAAPI Mine_connect(SOCKET s, const sockaddr* name, int namelen) {
        if (namelen < static_cast<int>(sizeof(sockaddr_in))) {
            return Hooks::Real_connect(s, name, namelen);
        }

        const sockaddr_in* addr = reinterpret_cast<const sockaddr_in*>(name);
        if (addr->sin_family != AF_INET) {
            LOG_DEBUG("event=tcp.route socket=%llu route=direct reason=address_family family=%d", (ULONG_PTR)s, addr->sin_family);
            return Hooks::Real_connect(s, name, namelen);
        }

        if (addr->sin_addr.s_addr == 0) {
            return Hooks::Real_connect(s, name, namelen);
        }

        if (IsLocalAddr(*addr) || IsSameAddr(*addr, g_proxyAddr)) {
            LOG_DEBUG("event=tcp.route socket=%llu route=direct reason=local_or_proxy destination=%s", (ULONG_PTR)s, AddrToString(*addr).c_str());
            return Hooks::Real_connect(s, name, namelen);
        }
        if (IsUdpSocket(s)) {
            LOG_DEBUG("event=udp.connect socket=%llu route=direct destination=%s", (ULONG_PTR)s, AddrToString(*addr).c_str());
            return Hooks::Real_connect(s, name, namelen);
        }

        bool wasNonBlocking = false;
        {
            std::shared_lock<std::shared_mutex> lock(g_stateMutex);
            wasNonBlocking = g_nonBlockingSockets.count(s) != 0;
        }

        const std::string target = AddrToString(*addr);
        const uint64_t startedAt = GetTickCount64();
        const uint64_t deadline = MakeDeadline(g_cfg.connectTimeout);
        const uint64_t connection = g_nextTcpConnection.fetch_add(1, std::memory_order_relaxed);
        {
            std::unique_lock<std::shared_mutex> lock(g_stateMutex);
            g_tcpConnections[s] = { connection, startedAt, *addr };
        }
        LOG_INFO("event=tcp.connect.start connection=%llu socket=%llu destination=%s proxy=%s mode=%s timeout_ms=%u",
            connection, (ULONG_PTR)s, target.c_str(), AddrToString(g_proxyAddr).c_str(),
            wasNonBlocking ? "nonblocking" : "blocking", g_cfg.connectTimeout);

        bool changedToNonBlocking = false;
        if (!wasNonBlocking) {
            u_long mode = 1;
            if (Hooks::Real_ioctlsocket(s, FIONBIO, &mode) != 0) {
                LOG_WARN("event=tcp.connect.failed connection=%llu socket=%llu stage=socket_mode wsa_error=%d",
                    connection, (ULONG_PTR)s, WSAGetLastError());
                return SOCKET_ERROR;
            }
            changedToNonBlocking = true;
        }

        int result = ConnectViaProxy(s, addr, deadline);
        int resultError = result == 0 ? 0 : WSAGetLastError();

        if (changedToNonBlocking) {
            u_long mode = 0;
            if (Hooks::Real_ioctlsocket(s, FIONBIO, &mode) != 0 && result == 0) {
                result = SOCKET_ERROR;
                resultError = WSAGetLastError();
            }
        }

        if (result != 0) {
            WSASetLastError(resultError);
            LOG_WARN("event=tcp.connect.failed connection=%llu socket=%llu destination=%s wsa_error=%d elapsed_ms=%llu",
                     connection, (ULONG_PTR)s, target.c_str(), resultError, GetTickCount64() - startedAt);
            WSASetLastError(resultError);
            return SOCKET_ERROR;
        }

        const uint64_t elapsed = GetTickCount64() - startedAt;
        LOG_INFO("event=tcp.connect.ready connection=%llu socket=%llu destination=%s mode=%s elapsed_ms=%llu application_result=%s",
            connection, (ULONG_PTR)s, target.c_str(), wasNonBlocking ? "nonblocking" : "blocking",
            elapsed, wasNonBlocking ? "WSAEWOULDBLOCK" : "success");

        if (wasNonBlocking) {
            WSASetLastError(WSAEWOULDBLOCK);
            return SOCKET_ERROR;
        }
        return 0;
    }

}
