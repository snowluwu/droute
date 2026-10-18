#pragma once

#include "pch.h"

namespace droute {
    namespace Socks5 {

        constexpr int UdpHeaderSize = 10;
        constexpr int UdpMaxHeaderSize = 262;

        bool Handshake(SOCKET s, const char* user, const char* pass, uint64_t deadline);
        bool RequestConnect(SOCKET s, const sockaddr_in& target, uint64_t deadline);
        bool RequestUdpAssociate(SOCKET ctrl, sockaddr_in& outRelay, uint64_t deadline);

        std::vector<uint8_t> EncodeUdpPacket(const sockaddr_in& dst, const void* payload, int len);
        bool DecodeUdpPacket(const void* data, int len, sockaddr_in& outSrc, const void*& outPayload,
                             int& outPayloadLen);

    }
}
