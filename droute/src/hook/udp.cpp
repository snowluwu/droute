#include "pch.h"
#include "src/hook/udp.hpp"
#include "src/hook/hooks.hpp"
#include "src/core/config.hpp"
#include "src/core/logger.hpp"
#include "src/core/utils.hpp"
#include "src/net/socks5.hpp"
#include <mswsock.h>

namespace droute {

    enum class UdpIoKind { Send, Receive };

    struct UdpIoOperation {
        UdpIoKind kind = UdpIoKind::Send;
        SOCKET socket = INVALID_SOCKET;
        LPWSAOVERLAPPED overlapped = nullptr;
        LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutine = nullptr;

        std::vector<uint8_t> buffer;
        std::vector<WSABUF> applicationBuffers;
        DWORD applicationBufferSize = 0;
        DWORD proxyHeaderSize = 0;

        sockaddr* applicationFrom = nullptr;
        LPINT applicationFromLength = nullptr;
        int applicationFromCapacity = 0;
        sockaddr_storage receivedFrom = {};
        int receivedFromLength = sizeof(receivedFrom);

        LPDWORD applicationFlags = nullptr;
        DWORD receiveFlags = 0;

        sockaddr_in relayAtStart = {};
        bool hasRelayAtStart = false;

        std::mutex completionMutex;
        bool processed = false;
        DWORD completedBytes = 0;
        int completionError = 0;
    };

    struct UdpCompletion {
        DWORD bytes = 0;
        int error = 0;
    };

    static std::mutex g_udpIoMutex;
    static std::map<LPWSAOVERLAPPED, std::shared_ptr<UdpIoOperation>> g_udpIoOperations;

    static bool GetApplicationBufferSize(LPWSABUF buffers, DWORD count, DWORD& size) {
        if (!buffers || count == 0) {
            WSASetLastError(WSAEFAULT);
            return false;
        }

        uint64_t total = 0;
        for (DWORD i = 0; i < count; ++i) {
            if (buffers[i].len != 0 && !buffers[i].buf) {
                WSASetLastError(WSAEFAULT);
                return false;
            }
            total += buffers[i].len;
            if (total > MAXDWORD) {
                WSASetLastError(WSAEMSGSIZE);
                return false;
            }
        }

        size = static_cast<DWORD>(total);
        return true;
    }

    static bool GatherApplicationBuffers(LPWSABUF buffers, DWORD count,
                                         std::vector<uint8_t>& output, DWORD& size) {
        if (!GetApplicationBufferSize(buffers, count, size))
            return false;

        output.resize(size);
        size_t offset = 0;
        for (DWORD i = 0; i < count; ++i) {
            if (buffers[i].len == 0)
                continue;
            memcpy(output.data() + offset, buffers[i].buf, buffers[i].len);
            offset += buffers[i].len;
        }
        return true;
    }

    static void ScatterApplicationBuffers(const void* data, DWORD length,
                                          const std::vector<WSABUF>& buffers) {
        const uint8_t* source = static_cast<const uint8_t*>(data);
        DWORD copied = 0;
        for (const WSABUF& buffer : buffers) {
            if (copied >= length)
                break;
            const DWORD chunk = (std::min)(buffer.len, length - copied);
            if (chunk != 0)
                memcpy(buffer.buf, source + copied, chunk);
            copied += chunk;
        }
    }

    static bool IsTrackedUdpSocket(SOCKET s) {
        std::shared_lock<std::shared_mutex> lock(g_stateMutex);
        return g_udpMap.find(s) != g_udpMap.end();
    }

    static bool TryGetUdpRelay(SOCKET s, sockaddr_in& relay) {
        std::shared_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(s);
        if (it == g_udpMap.end() || it->second.status != UdpAssociation::Status::Associated)
            return false;
        relay = it->second.relayAddr;
        return true;
    }

    static bool IsUdpRelaySource(const UdpIoOperation& operation, const sockaddr_in& source) {
        if (operation.hasRelayAtStart && IsSameAddr(source, operation.relayAtStart))
            return true;

        sockaddr_in currentRelay = {};
        if (TryGetUdpRelay(operation.socket, currentRelay) && IsSameAddr(source, currentRelay))
            return true;

        return IsSameAddr(source, g_proxyAddr);
    }

    static UdpCompletion CompleteUdpIo(const std::shared_ptr<UdpIoOperation>& operation,
                                       int operationError, DWORD transferred) {
        std::lock_guard<std::mutex> completionLock(operation->completionMutex);
        if (operation->processed)
            return { operation->completedBytes, operation->completionError };

        operation->processed = true;
        operation->completionError = operationError;
        if (operationError != 0)
            return { 0, operationError };

        if (operation->kind == UdpIoKind::Send) {
            if (transferred >= operation->proxyHeaderSize) {
                operation->completedBytes = (std::min)(
                    operation->applicationBufferSize,
                    transferred - operation->proxyHeaderSize);
            }
            return { operation->completedBytes, 0 };
        }

        if (transferred > operation->buffer.size()) {
            operation->completionError = WSAEMSGSIZE;
            return { 0, operation->completionError };
        }

        const void* payload = operation->buffer.data();
        int payloadLength = static_cast<int>(transferred);
        sockaddr_in source = {};

        if (operation->receivedFrom.ss_family == AF_INET) {
            source = *reinterpret_cast<const sockaddr_in*>(&operation->receivedFrom);
            if (IsUdpRelaySource(*operation, source)) {
                if (!Socks5UnwrapUdp(operation->buffer.data(), static_cast<int>(transferred),
                                     source, payload, payloadLength)) {
                    LOG_WARN("udp %llu recv unwrap failed", (ULONG_PTR)operation->socket);
                    operation->completionError = WSAECONNRESET;
                    return { 0, operation->completionError };
                }
            }
        }

        const DWORD available = operation->applicationBufferSize;
        const DWORD copied = (std::min)(available, static_cast<DWORD>(payloadLength));
        ScatterApplicationBuffers(payload, copied, operation->applicationBuffers);
        operation->completedBytes = copied;

        if (operation->applicationFrom && operation->applicationFromLength) {
            const int copyLength = (std::min)(operation->applicationFromCapacity,
                                            static_cast<int>(sizeof(sockaddr_in)));
            if (copyLength > 0)
                memcpy(operation->applicationFrom, &source, copyLength);
            *operation->applicationFromLength = sizeof(sockaddr_in);
        }
        if (operation->applicationFlags)
            *operation->applicationFlags = operation->receiveFlags;

        if (static_cast<DWORD>(payloadLength) > available)
            operation->completionError = WSAEMSGSIZE;

        return { operation->completedBytes, operation->completionError };
    }

    static bool RegisterUdpIo(const std::shared_ptr<UdpIoOperation>& operation) {
        std::lock_guard<std::mutex> lock(g_udpIoMutex);
        auto existing = g_udpIoOperations.find(operation->overlapped);
        if (existing != g_udpIoOperations.end()) {
            std::lock_guard<std::mutex> completionLock(existing->second->completionMutex);
            if (!existing->second->processed) {
                WSASetLastError(WSAEINVAL);
                return false;
            }
            g_udpIoOperations.erase(existing);
        }
        g_udpIoOperations.emplace(operation->overlapped, operation);
        return true;
    }

    static std::shared_ptr<UdpIoOperation> TakeUdpIo(LPWSAOVERLAPPED overlapped) {
        if (!overlapped)
            return nullptr;

        std::lock_guard<std::mutex> lock(g_udpIoMutex);
        auto it = g_udpIoOperations.find(overlapped);
        if (it == g_udpIoOperations.end())
            return nullptr;

        auto operation = it->second;
        g_udpIoOperations.erase(it);
        return operation;
    }

    static void CALLBACK UdpIoCompletionRoutine(DWORD error, DWORD transferred,
                                                LPWSAOVERLAPPED overlapped, DWORD flags) {
        auto operation = TakeUdpIo(overlapped);
        if (!operation)
            return;

        UdpCompletion completion = CompleteUdpIo(operation, static_cast<int>(error), transferred);
        if (operation->completionRoutine) {
            operation->completionRoutine(static_cast<DWORD>(completion.error), completion.bytes,
                                         overlapped, flags);
        }
    }

    static void TrackUdpSocket(SOCKET s) {
        std::unique_lock<std::shared_mutex> lock(g_stateMutex);
        if (g_udpMap.find(s) != g_udpMap.end())
            return;

        g_udpMap.emplace(s, UdpAssociation{});
        LOG_DEBUG("udp socket %llu tracked", (ULONG_PTR)s);
    }

    void MarkUdpAssociationPending(SOCKET s, SOCKET expectedControlSocket) {
        std::unique_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(s);
        if (it == g_udpMap.end())
            return;
        if (expectedControlSocket != INVALID_SOCKET &&
            it->second.ctrlSocket != expectedControlSocket)
            return;

        if (it->second.ctrlSocket != INVALID_SOCKET) {
            Hooks::Real_closesocket(it->second.ctrlSocket);
            it->second.ctrlSocket = INVALID_SOCKET;
        }
        it->second.relayAddr = {};
        it->second.status = UdpAssociation::Status::PendingAssociate;
        g_pendingUdp.insert(s);
    }

    bool TryUdpAssociate(UdpAssociation& out) {
        out = {};
        out.status = UdpAssociation::Status::Idle;

        SOCKET ctrl = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ctrl == INVALID_SOCKET) {
            return false;
        }

        u_long mode = 1;
        if (Hooks::Real_ioctlsocket(ctrl, FIONBIO, &mode) != 0) {
            int error = WSAGetLastError();
            Hooks::Real_closesocket(ctrl);
            WSASetLastError(error);
            return false;
        }

        const uint64_t deadline = MakeDeadline(g_cfg.connectTimeout);
        int rc = Hooks::Real_connect(ctrl, reinterpret_cast<const sockaddr*>(&g_proxyAddr), sizeof(g_proxyAddr));
        if (rc == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                if (!WaitForConnect(ctrl, RemainingTimeout(deadline))) {
                    int error = WSAGetLastError();
                    Hooks::Real_closesocket(ctrl);
                    WSASetLastError(error);
                    return false;
                }
                int soErr = 0; int soLen = sizeof(soErr);
                if (getsockopt(ctrl, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&soErr), &soLen) != 0) {
                    int error = WSAGetLastError();
                    Hooks::Real_closesocket(ctrl);
                    WSASetLastError(error);
                    return false;
                }
                if (soErr != 0) {
                    Hooks::Real_closesocket(ctrl);
                    WSASetLastError(soErr);
                    return false;
                }
            } else {
                Hooks::Real_closesocket(ctrl);
                WSASetLastError(err);
                return false;
            }
        }

        if (!Socks5Handshake(ctrl, g_cfg.user.c_str(), g_cfg.password.c_str(), deadline)) {
            int error = WSAGetLastError();
            Hooks::Real_closesocket(ctrl);
            WSASetLastError(error);
            return false;
        }

        sockaddr_in relay;
        if (!Socks5RequestUdpAssociate(ctrl, relay, deadline)) {
            int error = WSAGetLastError();
            Hooks::Real_closesocket(ctrl);
            WSASetLastError(error);
            return false;
        }

        out.ctrlSocket = ctrl;
        out.relayAddr = relay;
        out.status = UdpAssociation::Status::Associated;
        return true;
    }

    static bool ActivateUdpRelay(SOCKET s, std::unique_lock<std::shared_mutex>& lock,
                                 std::map<SOCKET, UdpAssociation>::iterator& it) {
        if (it->second.status == UdpAssociation::Status::Associating ||
            it->second.status == UdpAssociation::Status::PendingAssociate)
            return false;

        it->second.status = UdpAssociation::Status::Associating;
        lock.unlock();
        UdpAssociation temp;
        bool ok = TryUdpAssociate(temp);
        int associationError = ok ? 0 : WSAGetLastError();
        lock.lock();
        it = g_udpMap.find(s);
        if (it == g_udpMap.end()) {
            if (temp.ctrlSocket != INVALID_SOCKET) {
                Hooks::Real_closesocket(temp.ctrlSocket);
            }
            if (!ok)
                WSASetLastError(associationError);
            return false;
        }
        if (ok) {
            if (it->second.ctrlSocket != INVALID_SOCKET)
                Hooks::Real_closesocket(it->second.ctrlSocket);
            it->second.ctrlSocket = temp.ctrlSocket;
            it->second.relayAddr = temp.relayAddr;
            it->second.status = UdpAssociation::Status::Associated;
            g_pendingUdp.erase(s);
        } else {
            it->second.status = UdpAssociation::Status::PendingAssociate;
            g_pendingUdp.insert(s);
            WSASetLastError(associationError);
        }
        return ok;
    }

    int WSAAPI Mine_bind(SOCKET s, const sockaddr* addr, int namelen) {
        bool isUdp = IsUdpSocket(s);

        int rc = Hooks::Real_bind(s, addr, namelen);
        if (rc == 0 && isUdp)
            TrackUdpSocket(s);

        return rc;
    }

    static bool IsDirectUdpDestination(const sockaddr_in& destination) {
        return IsLocalAddr(destination) || IsSameAddr(destination, g_proxyAddr) || IsMulticast(destination);
    }

    static bool GetUdpRelayForSend(SOCKET s, sockaddr_in& relayAddr, SOCKET& controlSocket) {
        std::unique_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(s);
        if (it == g_udpMap.end())
            return false;

        // A disappearing relay port can generate ICMP Port Unreachable before
        // the control TCP disconnect is detected. Do not let that abort Discord's
        // pending receive (including IOCP); the worker monitors the control TCP
        // connection and recreates the association independently.
        if (!it->second.suppressUdpConnReset) {
            BOOL reportReset = FALSE;
            DWORD returned = 0;
            if (WSAIoctl(s, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset),
                         nullptr, 0, &returned, nullptr, nullptr) == SOCKET_ERROR) {
                LOG_WARN("udp %llu cannot suppress relay port reset: wsa_error=%d",
                         (ULONG_PTR)s, WSAGetLastError());
                return false;
            }
            it->second.suppressUdpConnReset = true;
        }

        if (it->second.status == UdpAssociation::Status::Idle) {
            if (!ActivateUdpRelay(s, lock, it)) {
                int error = WSAGetLastError();
                LOG_WARN("udp %llu associate deferred: wsa_error=%d", (ULONG_PTR)s, error);
                WSASetLastError(error);
                return false;
            }
            LOG_INFO("udp %llu associated, relay=%s", (ULONG_PTR)s, AddrToString(it->second.relayAddr).c_str());
        }

        if (it->second.status != UdpAssociation::Status::Associated)
            return false;

        relayAddr = it->second.relayAddr;
        controlSocket = it->second.ctrlSocket;
        if (IsSocketDisconnected(controlSocket)) {
            lock.unlock();
            MarkUdpAssociationPending(s, controlSocket);
            return false;
        }
        return true;
    }

    int WSAAPI Mine_sendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
        if (!to || tolen < static_cast<int>(sizeof(sockaddr_in)) || len < 0 || (len > 0 && !buf)) {
            return Hooks::Real_sendto(s, buf, len, flags, to, tolen);
        }
        const sockaddr_in* dst = reinterpret_cast<const sockaddr_in*>(to);
        if (dst->sin_family != AF_INET || IsDirectUdpDestination(*dst)) {
            return Hooks::Real_sendto(s, buf, len, flags, to, tolen);
        }

        if (!IsUdpSocket(s))
            return Hooks::Real_sendto(s, buf, len, flags, to, tolen);
        TrackUdpSocket(s);

        sockaddr_in relayAddr = {};
        SOCKET controlSocket = INVALID_SOCKET;
        if (!GetUdpRelayForSend(s, relayAddr, controlSocket)) {
            WSASetLastError(WSAEWOULDBLOCK);
            return SOCKET_ERROR;
        }

        auto packet = Socks5WrapUdp(*dst, buf, len);
        int result = Hooks::Real_sendto(s, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
                                        flags, reinterpret_cast<const sockaddr*>(&relayAddr), sizeof(relayAddr));
        if (result == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK)
                MarkUdpAssociationPending(s, controlSocket);
            WSASetLastError(error);
        }
        return result == static_cast<int>(packet.size()) ? len : result;
    }

    int WSAAPI Mine_WSASendTo(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent,
                              DWORD dwFlags, const sockaddr* lpTo, int iTolen,
                              LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
        if (!lpTo || iTolen < static_cast<int>(sizeof(sockaddr_in)) || dwBufferCount == 0 || !lpBuffers) {
            return Hooks::Real_WSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
                                         lpTo, iTolen, lpOverlapped, lpCompletionRoutine);
        }
        const sockaddr_in* dst = reinterpret_cast<const sockaddr_in*>(lpTo);
        if (dst->sin_family != AF_INET || IsDirectUdpDestination(*dst)) {
            return Hooks::Real_WSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
                                         lpTo, iTolen, lpOverlapped, lpCompletionRoutine);
        }

        if (!IsUdpSocket(s)) {
            return Hooks::Real_WSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
                                         lpTo, iTolen, lpOverlapped, lpCompletionRoutine);
        }
        TrackUdpSocket(s);

        sockaddr_in relayAddr = {};
        SOCKET controlSocket = INVALID_SOCKET;
        if (!GetUdpRelayForSend(s, relayAddr, controlSocket)) {
            WSASetLastError(WSAEWOULDBLOCK);
            return SOCKET_ERROR;
        }

        std::vector<uint8_t> payload;
        DWORD payloadSize = 0;
        if (!GatherApplicationBuffers(lpBuffers, dwBufferCount, payload, payloadSize))
            return SOCKET_ERROR;
        if (payloadSize > INT_MAX) {
            WSASetLastError(WSAEMSGSIZE);
            return SOCKET_ERROR;
        }

        auto operation = std::make_shared<UdpIoOperation>();
        operation->kind = UdpIoKind::Send;
        operation->socket = s;
        operation->overlapped = lpOverlapped;
        operation->completionRoutine = lpCompletionRoutine;
        operation->applicationBufferSize = payloadSize;
        operation->buffer = Socks5WrapUdp(*dst, payload.data(), static_cast<int>(payloadSize));
        operation->proxyHeaderSize = static_cast<DWORD>(operation->buffer.size()) - payloadSize;
        operation->relayAtStart = relayAddr;
        operation->hasRelayAtStart = true;

        if (lpOverlapped && !RegisterUdpIo(operation))
            return SOCKET_ERROR;

        DWORD localBytesSent = 0;
        LPDWORD bytesSent = lpNumberOfBytesSent ? lpNumberOfBytesSent : &localBytesSent;
        WSABUF proxyBuffer = {
            static_cast<ULONG>(operation->buffer.size()),
            reinterpret_cast<char*>(operation->buffer.data())
        };
        int result = Hooks::Real_WSASendTo(s, &proxyBuffer, 1, bytesSent, dwFlags,
                                           reinterpret_cast<const sockaddr*>(&relayAddr), sizeof(relayAddr),
                                           lpOverlapped,
                                           lpOverlapped && lpCompletionRoutine
                                               ? UdpIoCompletionRoutine
                                               : lpCompletionRoutine);
        int error = result == 0 ? 0 : WSAGetLastError();

        if (result == 0) {
            UdpCompletion completion = CompleteUdpIo(operation, 0,
                *bytesSent != 0 ? *bytesSent : static_cast<DWORD>(operation->buffer.size()));
            if (lpNumberOfBytesSent)
                *lpNumberOfBytesSent = completion.bytes;
        }

        if (result == SOCKET_ERROR) {
            if (error != WSA_IO_PENDING) {
                if (lpOverlapped)
                    TakeUdpIo(lpOverlapped);
            }
            if (error != WSA_IO_PENDING && error != WSAEWOULDBLOCK) {
                MarkUdpAssociationPending(s, controlSocket);
            }
            WSASetLastError(error);
        }
        return result;
    }

    int WSAAPI Mine_recvfrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
        if (!buf || len < 0 || !IsTrackedUdpSocket(s))
            return Hooks::Real_recvfrom(s, buf, len, flags, from, fromlen);

        auto operation = std::make_shared<UdpIoOperation>();
        operation->kind = UdpIoKind::Receive;
        operation->socket = s;
        operation->applicationBufferSize = static_cast<DWORD>(len);
        operation->applicationBuffers.push_back({ static_cast<ULONG>(len), buf });
        operation->applicationFrom = from;
        operation->applicationFromLength = fromlen;
        operation->applicationFromCapacity = from && fromlen ? *fromlen : 0;
        operation->buffer.resize(static_cast<size_t>(len) + SOCKS5_UDP_MAX_HEADER_SIZE);
        operation->hasRelayAtStart = TryGetUdpRelay(s, operation->relayAtStart);

        int received = Hooks::Real_recvfrom(
            s, reinterpret_cast<char*>(operation->buffer.data()),
            static_cast<int>(operation->buffer.size()), flags,
            reinterpret_cast<sockaddr*>(&operation->receivedFrom), &operation->receivedFromLength);
        if (received == SOCKET_ERROR)
            return received;

        UdpCompletion completion = CompleteUdpIo(operation, 0, static_cast<DWORD>(received));
        if (completion.error != 0) {
            WSASetLastError(completion.error);
            return SOCKET_ERROR;
        }
        return static_cast<int>(completion.bytes);
    }

    int WSAAPI Mine_WSARecvFrom(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd,
                                LPDWORD lpFlags, sockaddr* lpFrom, LPINT lpFromlen,
                                LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
        if (!IsTrackedUdpSocket(s)) {
            return Hooks::Real_WSARecvFrom(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
                                           lpFrom, lpFromlen, lpOverlapped, lpCompletionRoutine);
        }

        DWORD applicationBufferSize = 0;
        if (!GetApplicationBufferSize(lpBuffers, dwBufferCount, applicationBufferSize))
            return SOCKET_ERROR;
        if (applicationBufferSize > INT_MAX - SOCKS5_UDP_MAX_HEADER_SIZE) {
            WSASetLastError(WSAEMSGSIZE);
            return SOCKET_ERROR;
        }

        auto operation = std::make_shared<UdpIoOperation>();
        operation->kind = UdpIoKind::Receive;
        operation->socket = s;
        operation->overlapped = lpOverlapped;
        operation->completionRoutine = lpCompletionRoutine;
        operation->applicationBuffers.assign(lpBuffers, lpBuffers + dwBufferCount);
        operation->applicationBufferSize = applicationBufferSize;
        operation->applicationFrom = lpFrom;
        operation->applicationFromLength = lpFromlen;
        operation->applicationFromCapacity = lpFrom && lpFromlen ? *lpFromlen : 0;
        operation->applicationFlags = lpFlags;
        operation->receiveFlags = lpFlags ? *lpFlags : 0;
        operation->buffer.resize(static_cast<size_t>(applicationBufferSize) + SOCKS5_UDP_MAX_HEADER_SIZE);
        operation->hasRelayAtStart = TryGetUdpRelay(s, operation->relayAtStart);

        if (lpOverlapped && !RegisterUdpIo(operation))
            return SOCKET_ERROR;

        WSABUF proxyBuffer = {
            static_cast<ULONG>(operation->buffer.size()),
            reinterpret_cast<char*>(operation->buffer.data())
        };
        DWORD localReceived = 0;
        LPDWORD bytesReceived = lpNumberOfBytesRecvd ? lpNumberOfBytesRecvd : &localReceived;
        int status = Hooks::Real_WSARecvFrom(
            s, &proxyBuffer, 1, bytesReceived, &operation->receiveFlags,
            reinterpret_cast<sockaddr*>(&operation->receivedFrom), &operation->receivedFromLength,
            lpOverlapped, lpOverlapped && lpCompletionRoutine
                ? UdpIoCompletionRoutine
                : lpCompletionRoutine);
        int error = status == 0 ? 0 : WSAGetLastError();

        if (status == 0) {
            UdpCompletion completion = CompleteUdpIo(operation, 0, *bytesReceived);
            if (lpNumberOfBytesRecvd)
                *lpNumberOfBytesRecvd = completion.bytes;
            if (completion.error != 0) {
                WSASetLastError(completion.error);
                return SOCKET_ERROR;
            }
        } else if (error != WSA_IO_PENDING) {
            if (lpOverlapped)
                TakeUdpIo(lpOverlapped);
            WSASetLastError(error);
        }

        return status;
    }

    BOOL WSAAPI Mine_WSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED lpOverlapped,
                                            LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags) {
        BOOL result = Hooks::Real_WSAGetOverlappedResult(s, lpOverlapped, lpcbTransfer, fWait, lpdwFlags);
        int error = result ? 0 : WSAGetLastError();

        if (!result && error == WSA_IO_INCOMPLETE) {
            WSASetLastError(error);
            return result;
        }

        auto operation = TakeUdpIo(lpOverlapped);
        if (!operation) {
            if (!result)
                WSASetLastError(error);
            return result;
        }

        const DWORD transferred = lpcbTransfer ? *lpcbTransfer : 0;
        UdpCompletion completion = CompleteUdpIo(operation, error, transferred);
        if (lpcbTransfer)
            *lpcbTransfer = completion.bytes;
        if (lpdwFlags && operation->kind == UdpIoKind::Receive)
            *lpdwFlags = operation->receiveFlags;

        if (completion.error != 0) {
            WSASetLastError(completion.error);
            return FALSE;
        }
        return TRUE;
    }

    BOOL WINAPI Mine_GetQueuedCompletionStatus(HANDLE completionPort, LPDWORD bytesTransferred,
                                               PULONG_PTR completionKey, LPOVERLAPPED* overlapped,
                                               DWORD milliseconds) {
        BOOL result = Hooks::Real_GetQueuedCompletionStatus(
            completionPort, bytesTransferred, completionKey, overlapped, milliseconds);
        int error = result ? 0 : static_cast<int>(GetLastError());

        LPWSAOVERLAPPED completed = overlapped
            ? reinterpret_cast<LPWSAOVERLAPPED>(*overlapped)
            : nullptr;
        auto operation = TakeUdpIo(completed);
        if (!operation) {
            if (!result)
                SetLastError(error);
            return result;
        }

        const DWORD transferred = bytesTransferred ? *bytesTransferred : 0;
        UdpCompletion completion = CompleteUdpIo(operation, error, transferred);
        if (bytesTransferred)
            *bytesTransferred = completion.bytes;

        if (completion.error != 0) {
            SetLastError(completion.error);
            return FALSE;
        }
        return TRUE;
    }

    BOOL WINAPI Mine_GetQueuedCompletionStatusEx(HANDLE completionPort,
                                                 LPOVERLAPPED_ENTRY entries, ULONG count,
                                                 PULONG removed, DWORD milliseconds, BOOL alertable) {
        BOOL result = Hooks::Real_GetQueuedCompletionStatusEx(
            completionPort, entries, count, removed, milliseconds, alertable);
        DWORD error = result ? ERROR_SUCCESS : GetLastError();
        if (!result || !entries || !removed) {
            if (!result)
                SetLastError(error);
            return result;
        }

        for (ULONG i = 0; i < *removed; ++i) {
            auto operation = TakeUdpIo(
                reinterpret_cast<LPWSAOVERLAPPED>(entries[i].lpOverlapped));
            if (!operation)
                continue;

            const int operationError = entries[i].Internal == 0
                ? 0
                : WSA_OPERATION_ABORTED;
            UdpCompletion completion = CompleteUdpIo(
                operation, operationError, entries[i].dwNumberOfBytesTransferred);
            entries[i].dwNumberOfBytesTransferred = completion.bytes;
        }
        return TRUE;
    }

    static BOOL FinishKernelOverlappedResult(BOOL result, DWORD error,
                                             LPOVERLAPPED overlapped,
                                             LPDWORD bytesTransferred) {
        if (!result && error == ERROR_IO_INCOMPLETE) {
            SetLastError(error);
            return result;
        }

        auto operation = TakeUdpIo(reinterpret_cast<LPWSAOVERLAPPED>(overlapped));
        if (!operation) {
            if (!result)
                SetLastError(error);
            return result;
        }

        const DWORD transferred = bytesTransferred ? *bytesTransferred : 0;
        UdpCompletion completion = CompleteUdpIo(
            operation, result ? 0 : static_cast<int>(error), transferred);
        if (bytesTransferred)
            *bytesTransferred = completion.bytes;
        if (completion.error != 0) {
            SetLastError(completion.error);
            return FALSE;
        }
        return TRUE;
    }

    BOOL WINAPI Mine_GetOverlappedResult(HANDLE file, LPOVERLAPPED overlapped,
                                         LPDWORD bytesTransferred, BOOL wait) {
        BOOL result = Hooks::Real_GetOverlappedResult(file, overlapped, bytesTransferred, wait);
        DWORD error = result ? ERROR_SUCCESS : GetLastError();
        return FinishKernelOverlappedResult(result, error, overlapped, bytesTransferred);
    }

    BOOL WINAPI Mine_GetOverlappedResultEx(HANDLE file, LPOVERLAPPED overlapped,
                                           LPDWORD bytesTransferred, DWORD milliseconds,
                                           BOOL alertable) {
        BOOL result = Hooks::Real_GetOverlappedResultEx(
            file, overlapped, bytesTransferred, milliseconds, alertable);
        DWORD error = result ? ERROR_SUCCESS : GetLastError();
        return FinishKernelOverlappedResult(result, error, overlapped, bytesTransferred);
    }

    int WSAAPI Mine_closesocket(SOCKET s) {
        {
            std::unique_lock<std::shared_mutex> lock(g_stateMutex);
            auto it = g_udpMap.find(s);
            if (it != g_udpMap.end()) {
                if (it->second.ctrlSocket != INVALID_SOCKET) {
                    Hooks::Real_closesocket(it->second.ctrlSocket);
                }
                g_udpMap.erase(it);
                g_pendingUdp.erase(s);
                LOG_DEBUG("udp socket %llu cleaned", (ULONG_PTR)s);
            }
            g_nonBlockingSockets.erase(s);
        }
        return Hooks::Real_closesocket(s);
    }

    int WSAAPI Mine_WSAEventSelect(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents) {
        {
            std::unique_lock<std::shared_mutex> lock(g_stateMutex);
            if (hEventObject && lNetworkEvents) {
                g_nonBlockingSockets.insert(s);
            } else {
                g_nonBlockingSockets.erase(s);
            }
        }
        return Hooks::Real_WSAEventSelect(s, hEventObject, lNetworkEvents);
    }

    int WSAAPI Mine_WSAAsyncSelect(SOCKET s, HWND hWnd, unsigned int wMsg, long lEvent) {
        {
            std::unique_lock<std::shared_mutex> lock(g_stateMutex);
            if (lEvent) {
                g_nonBlockingSockets.insert(s);
            } else {
                g_nonBlockingSockets.erase(s);
            }
        }
        return Hooks::Real_WSAAsyncSelect(s, hWnd, wMsg, lEvent);
    }

    int WSAAPI Mine_ioctlsocket(SOCKET s, long cmd, u_long* argp) {
        if (cmd == FIONBIO) {
            std::unique_lock<std::shared_mutex> lock(g_stateMutex);
            if (*argp) {
                g_nonBlockingSockets.insert(s);
            } else {
                g_nonBlockingSockets.erase(s);
            }
        }
        return Hooks::Real_ioctlsocket(s, cmd, argp);
    }

}
