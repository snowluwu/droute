#include "pch.h"
#include "src/dllmain.hpp"
#include "src/hook/tcp.hpp"
#include "src/hook/async.hpp"
#include "src/hook/udp.hpp"
#include "src/hook/hooks.hpp"
#include "src/core/config.hpp"
#include "src/core/logger.hpp"
#include "src/core/utils.hpp"
#include "src/net/socks5.hpp"

namespace droute {

    static std::atomic<uint64_t> g_nextTcpConnection{1};

    int ConnectToProxy(SOCKET s, uint64_t deadline) {
        sockaddr_in proxy;
        if (!GetProxyAddress(proxy))
            return SOCKET_ERROR;

        if (RemainingTimeout(deadline) == 0) {
            WSASetLastError(WSAETIMEDOUT);
            return SOCKET_ERROR;
        }

        int rc = Hooks::Real_connect(s, reinterpret_cast<const sockaddr*>(&proxy), sizeof(proxy));
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

    int ConnectViaProxy(SOCKET s, const sockaddr_in& target, uint64_t deadline) {
        if (ConnectToProxy(s, deadline) != 0) {
            LOG_WARN("event=tcp.connect.stage_failed socket=%llu stage=proxy_connect proxy=%s wsa_error=%d",
                     (ULONG_PTR)s, FormatAddress(g_proxyAddr).c_str(), WSAGetLastError());
            return SOCKET_ERROR;
        }
        if (!Socks5::Handshake(s, g_cfg.user.c_str(), g_cfg.password.c_str(), deadline)) {
            LOG_WARN("event=tcp.connect.stage_failed socket=%llu stage=socks_greeting_auth wsa_error=%d", (ULONG_PTR)s,
                     WSAGetLastError());
            return SOCKET_ERROR;
        }
        if (!Socks5::RequestConnect(s, target, deadline)) {
            LOG_WARN("event=tcp.connect.stage_failed socket=%llu stage=socks_connect wsa_error=%d", (ULONG_PTR)s,
                     WSAGetLastError());
            return SOCKET_ERROR;
        }
        return 0;
    }

    int WSAAPI Mine_connect(SOCKET s, const sockaddr* name, int namelen) {
        if (IsInitializingRuntime() || IsResolvingProxy())
            return Hooks::Real_connect(s, name, namelen);
        if (!EnsureRuntime()) {
            WSASetLastError(WSAENETDOWN);
            return SOCKET_ERROR;
        }
        if (IsUdpSocket(s))
            return ConnectUdpSocket(s, name, namelen);
        if (!name || namelen < static_cast<int>(sizeof(sockaddr_in))) {
            return Hooks::Real_connect(s, name, namelen);
        }

        const sockaddr_in* addr = reinterpret_cast<const sockaddr_in*>(name);
        if (addr->sin_family != AF_INET) {
            LOG_DEBUG("event=tcp.route socket=%llu route=direct reason=address_family family=%d", (ULONG_PTR)s,
                      addr->sin_family);
            return Hooks::Real_connect(s, name, namelen);
        }

        if (addr->sin_addr.s_addr == 0) {
            return Hooks::Real_connect(s, name, namelen);
        }

        if (IsLocalAddr(*addr) || IsSameAddr(*addr, g_proxyAddr)) {
            LOG_DEBUG("event=tcp.route socket=%llu route=direct reason=local_or_proxy destination=%s", (ULONG_PTR)s,
                      FormatAddress(*addr).c_str());
            return Hooks::Real_connect(s, name, namelen);
        }

        bool wasNonBlocking = false;
        {
            std::shared_lock<std::shared_mutex> lock(g_stateMutex);
            wasNonBlocking = g_nonBlockingSockets.count(s) != 0;
        }

        const std::string target = FormatAddress(*addr);
        const uint64_t startedAt = GetTickCount64();
        const uint64_t deadline = MakeDeadline(g_cfg.connectTimeout);
        const uint64_t connection = g_nextTcpConnection.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::shared_mutex> lock(g_stateMutex);
            g_tcpConnections[s] = {connection, startedAt};
        }

        LOG_DEBUG("event=tcp.connect.start connection=%llu socket=%llu destination=%s proxy=%s mode=%s timeout_ms=%u",
                 connection, (ULONG_PTR)s, target.c_str(), FormatAddress(g_proxyAddr).c_str(),
                 wasNonBlocking ? "nonblocking" : "blocking", g_cfg.connectTimeout);

        if (wasNonBlocking)
            return BeginTcpConnect(s, *addr);

        // use nonblocking I/O internally so the connection respects its deadline
        u_long mode = 1;
        if (Hooks::Real_ioctlsocket(s, FIONBIO, &mode) != 0) {
            LOG_WARN("event=tcp.connect.failed connection=%llu socket=%llu stage=socket_mode wsa_error=%d",
                     connection, (ULONG_PTR)s, WSAGetLastError());
            return SOCKET_ERROR;
        }

        int result = ConnectViaProxy(s, *addr, deadline);
        int resultError = result == 0 ? 0 : WSAGetLastError();

        mode = 0;
        if (Hooks::Real_ioctlsocket(s, FIONBIO, &mode) != 0 && result == 0) {
            result = SOCKET_ERROR;
            resultError = WSAGetLastError();
        }

        if (result != 0) {
            WSASetLastError(resultError);
            LOG_WARN("event=tcp.connect.failed connection=%llu socket=%llu destination=%s wsa_error=%d elapsed_ms=%llu",
                     connection, (ULONG_PTR)s, target.c_str(), resultError, GetTickCount64() - startedAt);
            WSASetLastError(resultError);
            return SOCKET_ERROR;
        }

        const uint64_t elapsed = GetTickCount64() - startedAt;

        LOG_DEBUG("event=tcp.connect.ready connection=%llu socket=%llu destination=%s mode=blocking elapsed_ms=%llu "
                 "application_result=success",
                 connection, (ULONG_PTR)s, target.c_str(), elapsed);

        return 0;
    }

}
