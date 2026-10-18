#include "pch.h"
#include "src/core/utils.hpp"
#include "src/hook/hooks.hpp"

namespace droute {

    bool IsLocalAddr(const sockaddr_in& addr) {
        if (addr.sin_family != AF_INET)
            return false;
        uint32_t ip = ntohl(addr.sin_addr.s_addr);
        // loopback, private networks and link-local addresses
        if ((ip & 0xFF000000) == 0x7F000000)
            return true;
        if ((ip & 0xFF000000) == 0x0A000000)
            return true;
        if ((ip & 0xFFF00000) == 0xAC100000)
            return true;
        if ((ip & 0xFFFF0000) == 0xC0A80000)
            return true;
        if ((ip & 0xFFFF0000) == 0xA9FE0000)
            return true;
        return false;
    }

    bool IsMulticast(const sockaddr_in& addr) {
        if (addr.sin_family != AF_INET)
            return false;
        uint32_t ip = ntohl(addr.sin_addr.s_addr);
        return (ip & 0xF0000000) == 0xE0000000;
    }

    bool IsUdpSocket(SOCKET s) {
        int type = 0;
        int len = sizeof(type);
        if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char*)&type, &len) != 0)
            return false;
        return type == SOCK_DGRAM;
    }

    bool IsSameAddr(const sockaddr_in& a, const sockaddr_in& b) {
        return a.sin_family == b.sin_family && a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }

    bool IsSocketDisconnected(SOCKET s, int* error, const char** reason) {
        if (error)
            *error = 0;
        if (reason)
            *reason = "connected";
        if (s == INVALID_SOCKET) {
            if (reason)
                *reason = "invalid_control_socket";
            return true;
        }

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);

        timeval timeout = {};
        int result = select(0, &readable, nullptr, nullptr, &timeout);
        if (result == 0)
            return false;
        if (result == SOCKET_ERROR) {
            if (error)
                *error = WSAGetLastError();
            if (reason)
                *reason = "control_select_failed";
            return true;
        }

        char byte = 0;
        result = Hooks::Real_recv(s, &byte, 1, MSG_PEEK);
        if (result > 0)
            return false;
        if (result == 0) {
            if (reason)
                *reason = "control_peer_closed";
            return true;
        }

        const int socketError = WSAGetLastError();
        if (socketError == WSAEWOULDBLOCK)
            return false;
        if (error)
            *error = socketError;
        if (reason)
            *reason = "control_recv_failed";
        return true;
    }

    static bool WaitForSocket(SOCKET s, bool write, int timeoutMs) {
        if (timeoutMs <= 0) {
            WSASetLastError(WSAETIMEDOUT);
            return false;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);

        timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        int r = select(0, write ? nullptr : &fds, write ? &fds : nullptr, nullptr, &tv);
        if (r == 0) {
            WSASetLastError(WSAETIMEDOUT);
            return false;
        }
        return r > 0 && FD_ISSET(s, &fds);
    }

    bool WaitForWrite(SOCKET s, int timeoutMs) {
        return WaitForSocket(s, true, timeoutMs);
    }

    bool WaitForRead(SOCKET s, int timeoutMs) {
        return WaitForSocket(s, false, timeoutMs);
    }

    bool WaitForConnect(SOCKET s, int timeoutMs) {
        if (timeoutMs <= 0) {
            WSASetLastError(WSAETIMEDOUT);
            return false;
        }

        fd_set writable;
        fd_set exceptional;
        FD_ZERO(&writable);
        FD_ZERO(&exceptional);
        FD_SET(s, &writable);
        FD_SET(s, &exceptional);

        timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        int result = select(0, nullptr, &writable, &exceptional, &tv);
        if (result == 0) {
            WSASetLastError(WSAETIMEDOUT);
            return false;
        }
        return result > 0 && (FD_ISSET(s, &writable) || FD_ISSET(s, &exceptional));
    }

    uint64_t MakeDeadline(uint32_t timeoutMs) {
        return GetTickCount64() + timeoutMs;
    }

    int RemainingTimeout(uint64_t deadline) {
        const uint64_t now = GetTickCount64();
        if (now >= deadline)
            return 0;

        const uint64_t remaining = deadline - now;
        return remaining > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(remaining);
    }

    bool SendAll(SOCKET s, const void* data, int len, uint64_t deadline) {
        const char* bytes = static_cast<const char*>(data);
        int sent = 0;
        while (sent < len) {
            int written = Hooks::Real_send(s, bytes + sent, len - sent, 0);
            if (written > 0) {
                sent += written;
                continue;
            }
            if (written == 0) {
                WSASetLastError(WSAECONNRESET);
                return false;
            }
            if (WSAGetLastError() != WSAEWOULDBLOCK)
                return false;
            if (!WaitForWrite(s, RemainingTimeout(deadline)))
                return false;
        }
        return true;
    }

    bool RecvAll(SOCKET s, void* buf, int len, uint64_t deadline) {
        char* bytes = static_cast<char*>(buf);
        int received = 0;
        while (received < len) {
            int count = Hooks::Real_recv(s, bytes + received, len - received, 0);
            if (count > 0) {
                received += count;
                continue;
            }
            if (count == 0) {
                WSASetLastError(WSAECONNRESET);
                return false;
            }
            if (WSAGetLastError() != WSAEWOULDBLOCK)
                return false;
            if (!WaitForRead(s, RemainingTimeout(deadline)))
                return false;
        }
        return true;
    }

    std::string FormatAddress(const sockaddr_in& addr) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        char buf[64];
        snprintf(buf, sizeof(buf), "%s:%d", ip, ntohs(addr.sin_port));
        return std::string(buf);
    }

}
