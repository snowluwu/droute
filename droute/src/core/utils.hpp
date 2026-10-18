#pragma once

#include "pch.h"

namespace droute {

    bool IsLocalAddr(const sockaddr_in& addr);
    bool IsMulticast(const sockaddr_in& addr);
    bool IsUdpSocket(SOCKET s);
    bool IsSameAddr(const sockaddr_in& a, const sockaddr_in& b);
    bool IsSocketDisconnected(SOCKET s, int* error = nullptr, const char** reason = nullptr);

    bool WaitForWrite(SOCKET s, int timeoutMs);
    bool WaitForRead(SOCKET s, int timeoutMs);
    bool WaitForConnect(SOCKET s, int timeoutMs);
    bool SetNonBlocking(SOCKET s, bool nonBlock);

    uint64_t MakeDeadline(uint32_t timeoutMs);
    int RemainingTimeout(uint64_t deadline);

    bool SendAll(SOCKET s, const void* data, int len, uint64_t deadline);
    bool RecvAll(SOCKET s, void* buf, int len, uint64_t deadline);

    std::string FormatAddress(const sockaddr_in& addr);

}
