#include "pch.h"
#include "src/net/socks5.hpp"
#include "src/core/logger.hpp"
#include "src/core/utils.hpp"

namespace droute {
    namespace Socks5 {

        struct Reply {
            uint8_t status;
            uint8_t addressType;
            sockaddr_in boundAddr;
        };

        static int ReplyToSocketError(uint8_t reply) {
            switch (reply) {
            case 0x01:
                return WSAECONNREFUSED;
            case 0x02:
                return WSAEACCES;
            case 0x03:
                return WSAENETUNREACH;
            case 0x04:
                return WSAEHOSTUNREACH;
            case 0x05:
                return WSAECONNREFUSED;
            case 0x06:
                return WSAETIMEDOUT;
            case 0x07:
                return WSAEOPNOTSUPP;
            case 0x08:
                return WSAEAFNOSUPPORT;
            default:
                return WSAECONNRESET;
            }
        }

        static bool ResolveReplyDomain(const uint8_t* name, size_t length, in_addr& out) {
            if (!name || length == 0)
                return false;

            std::string host(reinterpret_cast<const char*>(name), length);
            addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;

            addrinfo* result = nullptr;
            int error = getaddrinfo(host.c_str(), nullptr, &hints, &result);
            if (error != 0 || !result) {
                WSASetLastError(WSAHOST_NOT_FOUND);
                return false;
            }

            out = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
            freeaddrinfo(result);
            return true;
        }

        static bool ReadReply(SOCKET s, Reply& out, uint64_t deadline) {
            uint8_t header[4];
            if (!RecvAll(s, header, 4, deadline)) {
                LOG_ERROR("event=socks5.error socket=%llu socks5 handshake: failed to read reply header wsa_error=%d",
                          (ULONG_PTR)s, WSAGetLastError());
                return false;
            }
            if (header[0] != 0x05) {
                LOG_ERROR("event=socks5.error socket=%llu socks5 handshake: invalid version 0x%02X", (ULONG_PTR)s,
                          header[0]);
                WSASetLastError(WSAEPROTONOSUPPORT);
                return false;
            }
            if (header[2] != 0x00) {
                LOG_ERROR("event=socks5.error socket=%llu socks5 handshake: invalid reserved byte 0x%02X", (ULONG_PTR)s,
                          header[2]);
                WSASetLastError(WSAEPROTONOSUPPORT);
                return false;
            }

            out.status = header[1];
            out.addressType = header[3];
            LOG_DEBUG("event=socks5.reply socket=%llu reply=0x%02X atyp=0x%02X", (ULONG_PTR)s, out.status, out.addressType);

            size_t addrLen = 0;
            switch (out.addressType) {
            case 0x01:
                addrLen = 4 + 2;
                break;
            case 0x03: {
                uint8_t domainLen;
                if (!RecvAll(s, &domainLen, 1, deadline)) {
                    LOG_ERROR(
                        "event=socks5.error socket=%llu socks5 handshake: failed to read domain length wsa_error=%d",
                        (ULONG_PTR)s, WSAGetLastError());
                    return false;
                }
                addrLen = domainLen + 2;
                break;
            }
            case 0x04:
                addrLen = 16 + 2;
                break;
            default:
                LOG_ERROR("event=socks5.error socket=%llu socks5 handshake: unsupported ATYP 0x%02X", (ULONG_PTR)s,
                          out.addressType);
                WSASetLastError(WSAEAFNOSUPPORT);
                return false;
            }

            std::vector<uint8_t> addrBuf(addrLen);
            if (!RecvAll(s, addrBuf.data(), static_cast<int>(addrLen), deadline)) {
                LOG_ERROR("event=socks5.error socket=%llu socks5 handshake: failed to read reply address wsa_error=%d",
                          (ULONG_PTR)s, WSAGetLastError());
                return false;
            }

            out.boundAddr = {};
            out.boundAddr.sin_family = AF_INET;

            if (out.addressType == 0x01) {
                memcpy(&out.boundAddr.sin_addr.s_addr, addrBuf.data(), 4);
                memcpy(&out.boundAddr.sin_port, addrBuf.data() + 4, 2);
            } else if (out.addressType == 0x03) {
                size_t domainLen = addrLen - 2;
                if (!ResolveReplyDomain(addrBuf.data(), domainLen, out.boundAddr.sin_addr)) {
                    LOG_ERROR(
                        "event=socks5.error socket=%llu socks5 handshake: failed to resolve reply domain wsa_error=%d",
                        (ULONG_PTR)s, WSAGetLastError());
                    return false;
                }
                memcpy(&out.boundAddr.sin_port, addrBuf.data() + domainLen, 2);
            } else {
                static const uint8_t ipv4MappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
                if (memcmp(addrBuf.data(), ipv4MappedPrefix, sizeof(ipv4MappedPrefix)) != 0) {
                    WSASetLastError(WSAEAFNOSUPPORT);
                    LOG_ERROR("event=socks5.error socket=%llu socks5 handshake: IPv6 relay is not supported by an IPv4 "
                              "socket",
                              (ULONG_PTR)s);
                    return false;
                }
                memcpy(&out.boundAddr.sin_addr.s_addr, addrBuf.data() + 12, 4);
                memcpy(&out.boundAddr.sin_port, addrBuf.data() + 16, 2);
            }

            return true;
        }

        bool Handshake(SOCKET s, const char* user, const char* pass, uint64_t deadline) {
            bool hasAuth = user && user[0] && pass && pass[0];
            LOG_DEBUG("event=socks5.greeting.start socket=%llu auth_configured=%s remaining_ms=%d", (ULONG_PTR)s,
                      hasAuth ? "yes" : "no", RemainingTimeout(deadline));

            // version, method count, no authentication, username/password
            uint8_t req[4] = {0x05, 0x01, 0x00, 0x02};
            if (hasAuth)
                req[1] = 0x02;
            int reqLen = hasAuth ? 4 : 3;

            if (!SendAll(s, req, reqLen, deadline)) {
                LOG_ERROR("event=socks5.error socket=%llu socks5 handshake: failed to send greeting wsa_error=%d",
                          (ULONG_PTR)s, WSAGetLastError());
                return false;
            }

            uint8_t resp[2];
            if (!RecvAll(s, resp, 2, deadline)) {
                LOG_ERROR(
                    "event=socks5.error socket=%llu socks5 handshake: failed to recv greeting response wsa_error=%d",
                    (ULONG_PTR)s, WSAGetLastError());
                return false;
            }

            if (resp[0] != 0x05) {
                LOG_ERROR("event=socks5.error socket=%llu socks5 handshake: invalid version 0x%02X", (ULONG_PTR)s,
                          resp[0]);
                WSASetLastError(WSAEPROTONOSUPPORT);
                return false;
            }

            LOG_DEBUG("event=socks5.greeting.selected socket=%llu method=0x%02X", (ULONG_PTR)s, resp[1]);
            if (resp[1] == 0x02) {
                if (!hasAuth) {
                    LOG_ERROR(
                        "event=socks5.error socket=%llu socks5 handshake: server demands auth but none configured",
                        (ULONG_PTR)s);
                    WSASetLastError(WSAEACCES);
                    return false;
                }
                size_t ulen = strlen(user);
                size_t plen = strlen(pass);
                if (ulen > 255 || plen > 255) {
                    LOG_ERROR("event=socks5.error socket=%llu socks5 handshake: credentials too long", (ULONG_PTR)s);
                    WSASetLastError(WSAEINVAL);
                    return false;
                }

                uint8_t auth[1 + 1 + 255 + 1 + 255];
                size_t pos = 0;
                auth[pos++] = 0x01;
                auth[pos++] = static_cast<uint8_t>(ulen);
                memcpy(&auth[pos], user, ulen);
                pos += ulen;
                auth[pos++] = static_cast<uint8_t>(plen);
                memcpy(&auth[pos], pass, plen);
                pos += plen;

                if (!SendAll(s, auth, static_cast<int>(pos), deadline)) {
                    LOG_ERROR("event=socks5.error socket=%llu socks5 auth: failed to send credentials wsa_error=%d",
                              (ULONG_PTR)s, WSAGetLastError());
                    return false;
                }
                uint8_t aresp[2];
                if (!RecvAll(s, aresp, 2, deadline)) {
                    LOG_ERROR("event=socks5.error socket=%llu socks5 auth: failed to recv auth response wsa_error=%d",
                              (ULONG_PTR)s, WSAGetLastError());
                    return false;
                }
                if (aresp[0] != 0x01 || aresp[1] != 0x00) {
                    LOG_ERROR("event=socks5.error socket=%llu socks5 auth failed", (ULONG_PTR)s);
                    WSASetLastError(WSAEACCES);
                    return false;
                }
            } else if (resp[1] != 0x00) {
                LOG_ERROR("event=socks5.error socket=%llu socks5 handshake: unsupported method 0x%02X", (ULONG_PTR)s,
                          resp[1]);
                WSASetLastError(WSAEACCES);
                return false;
            }

            LOG_DEBUG("event=socks5.greeting.ready socket=%llu method=0x%02X", (ULONG_PTR)s, resp[1]);
            return true;
        }

        bool RequestConnect(SOCKET s, const sockaddr_in& target, uint64_t deadline) {
            LOG_DEBUG("event=socks5.connect.request socket=%llu destination=%s remaining_ms=%d", (ULONG_PTR)s,
                      FormatAddress(target).c_str(), RemainingTimeout(deadline));
            uint8_t req[10] = {0x05, 0x01, 0x00, 0x01};
            memcpy(req + 4, &target.sin_addr.s_addr, 4);
            memcpy(req + 8, &target.sin_port, 2);

            if (!SendAll(s, req, 10, deadline)) {
                LOG_ERROR("event=socks5.error socket=%llu socks5 connect: failed to send request wsa_error=%d",
                          (ULONG_PTR)s, WSAGetLastError());
                return false;
            }

            Reply reply;
            if (!ReadReply(s, reply, deadline)) {
                return false;
            }
            if (reply.status != 0x00) {
                LOG_ERROR("event=socks5.error socket=%llu socks5 connect rejected: rep=0x%02X", (ULONG_PTR)s,
                          reply.status);
                WSASetLastError(ReplyToSocketError(reply.status));
                return false;
            }

            return true;
        }

        bool RequestUdpAssociate(SOCKET ctrl, sockaddr_in& outRelay, uint64_t deadline) {
            LOG_DEBUG("event=socks5.udp_associate.request control=%llu remaining_ms=%d", (ULONG_PTR)ctrl,
                      RemainingTimeout(deadline));
            uint8_t req[10] = {0x05, 0x03, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
            if (!SendAll(ctrl, req, 10, deadline)) {
                LOG_ERROR("event=socks5.error socket=%llu socks5 udp associate: failed to send request wsa_error=%d",
                          (ULONG_PTR)ctrl, WSAGetLastError());
                return false;
            }

            Reply reply;
            if (!ReadReply(ctrl, reply, deadline)) {
                return false;
            }
            if (reply.status != 0x00) {
                LOG_ERROR("event=socks5.error socket=%llu socks5 udp associate rejected: rep=0x%02X", (ULONG_PTR)ctrl,
                          reply.status);
                WSASetLastError(ReplyToSocketError(reply.status));
                return false;
            }

            outRelay = reply.boundAddr;
            if (outRelay.sin_addr.s_addr == INADDR_ANY) {
                sockaddr_in peer = {};
                int peerLength = sizeof(peer);
                if (getpeername(ctrl, reinterpret_cast<sockaddr*>(&peer), &peerLength) != 0)
                    return false;
                outRelay.sin_addr = peer.sin_addr;
                LOG_DEBUG("event=socks5.udp_associate.resolve_wildcard control=%llu resolved=%s", (ULONG_PTR)ctrl,
                          FormatAddress(outRelay).c_str());
            }
            if (outRelay.sin_port == 0) {
                LOG_ERROR("event=socks5.udp_associate.invalid_relay control=%llu reason=zero_port", (ULONG_PTR)ctrl);
                WSASetLastError(WSAEADDRNOTAVAIL);
                return false;
            }
            LOG_DEBUG("event=socks5.udp_associate.ready control=%llu relay=%s", (ULONG_PTR)ctrl,
                      FormatAddress(outRelay).c_str());
            return true;
        }

        std::vector<uint8_t> EncodeUdpPacket(const sockaddr_in& dst, const void* payload, int len) {
            std::vector<uint8_t> out(UdpHeaderSize + len);
            out[3] = 0x01;
            memcpy(out.data() + 4, &dst.sin_addr, sizeof(dst.sin_addr));
            memcpy(out.data() + 8, &dst.sin_port, sizeof(dst.sin_port));
            if (len > 0) {
                memcpy(out.data() + UdpHeaderSize, payload, len);
            }
            return out;
        }

        bool DecodeUdpPacket(const void* data, int len, sockaddr_in& outSrc, const void*& outPayload,
                             int& outPayloadLen) {
            if (!data || len < 4)
                return false;
            const uint8_t* p = static_cast<const uint8_t*>(data);
            if (p[0] != 0x00 || p[1] != 0x00)
                return false;
            if (p[2] != 0x00)
                return false;

            outSrc = {};
            outSrc.sin_family = AF_INET;

            int headerSize = 0;
            switch (p[3]) {
            case 0x01:
                headerSize = UdpHeaderSize;
                if (len < headerSize)
                    return false;
                memcpy(&outSrc.sin_addr.s_addr, p + 4, 4);
                memcpy(&outSrc.sin_port, p + 8, 2);
                break;

            case 0x03: {
                if (len < 5)
                    return false;
                const uint8_t domainLength = p[4];
                headerSize = 7 + domainLength;
                if (len < headerSize)
                    return false;
                if (!ResolveReplyDomain(p + 5, domainLength, outSrc.sin_addr))
                    return false;
                memcpy(&outSrc.sin_port, p + 5 + domainLength, 2);
                break;
            }

            case 0x04: {
                headerSize = 22;
                if (len < headerSize)
                    return false;
                static const uint8_t ipv4MappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
                if (memcmp(p + 4, ipv4MappedPrefix, sizeof(ipv4MappedPrefix)) != 0)
                    return false;
                memcpy(&outSrc.sin_addr.s_addr, p + 16, 4);
                memcpy(&outSrc.sin_port, p + 20, 2);
                break;
            }

            default:
                return false;
            }

            outPayload = p + headerSize;
            outPayloadLen = len - headerSize;
            return true;
        }

    }
}
