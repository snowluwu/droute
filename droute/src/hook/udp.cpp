#include "pch.h"
#include "src/hook/udp.hpp"
#include "src/hook/hooks.hpp"
#include "src/core/config.hpp"
#include "src/core/logger.hpp"
#include "src/core/utils.hpp"
#include "src/net/socks5.hpp"
#include <mswsock.h>

namespace droute {

    enum class UdpIoKind {
        Send,
        Receive
    };

    static std::atomic<uint64_t> g_nextUdpFlow{1};

    static const char* UdpStatusName(UdpAssociation::Status status) {
        switch (status) {
        case UdpAssociation::Status::Idle:
            return "idle";
        case UdpAssociation::Status::Associating:
            return "associating";
        case UdpAssociation::Status::PendingAssociate:
            return "pending";
        case UdpAssociation::Status::Associated:
            return "associated";
        }
        return "unknown";
    }

    void LogUdpAssociationStart(SOCKET s, UdpAssociation& a, const char* trigger) {
        Logger::ErrorScope preserveError;
        ++a.attempts;
        a.attemptStartedAt = GetTickCount64();
        const LogLevel level = a.attempts == 1 || a.lastAssociationError == 0 ? LogLevel::Info : LogLevel::Debug;
        DROUTE_LOG(level, "event=udp.associate.start flow=%llu socket=%llu attempt=%llu trigger=%s proxy=%s", a.flowId,
                   (ULONG_PTR)s, a.attempts, trigger, FormatAddress(g_proxyAddr).c_str());
    }

    void LogUdpAssociationResult(SOCKET s, UdpAssociation& a, bool ok, int error, const char* stage) {
        Logger::ErrorScope preserveError;
        const uint64_t elapsed = GetTickCount64() - a.attemptStartedAt;
        if (ok) {
            ++a.generation;
            LOG_INFO("event=udp.associate.ready flow=%llu socket=%llu attempt=%llu generation=%llu control=%llu "
                     "relay=%s elapsed_ms=%llu",
                     a.flowId, (ULONG_PTR)s, a.attempts, a.generation, (ULONG_PTR)a.ctrlSocket,
                     FormatAddress(a.relayAddr).c_str(), elapsed);
        } else {
            const LogLevel level =
                a.attempts == 1 || error != a.lastAssociationError || strcmp(stage, a.associationStage) != 0
                    ? LogLevel::Warn
                    : LogLevel::Debug;
            DROUTE_LOG(level,
                       "event=udp.associate.failed flow=%llu socket=%llu attempt=%llu stage=%s wsa_error=%d "
                       "elapsed_ms=%llu next_retry_ms=%u",
                       a.flowId, (ULONG_PTR)s, a.attempts, stage, error, elapsed, g_cfg.reconnectInterval);
        }
        a.lastAssociationError = error;
        a.associationStage = stage;
    }

    void LogUdpSummary(SOCKET s, UdpAssociation& a, const char* event) {
        Logger::ErrorScope preserveError;
        const uint64_t now = GetTickCount64();
        LOG_INFO("event=%s flow=%llu socket=%llu state=%s generation=%llu attempts=%llu control=%llu relay=%s "
                 "destination=%s lifetime_ms=%llu interval_ms=%llu tx_packets=%llu rx_packets=%llu tx_bytes=%llu "
                 "rx_bytes=%llu interval_tx=%llu interval_rx=%llu tx_age_ms=%lld rx_age_ms=%lld pending_io=%llu "
                 "io_errors=%llu would_block=%llu blocked_sends=%llu last_io_error=%d last_associate_error=%d "
                 "associate_stage=%s",
                 event, a.flowId, (ULONG_PTR)s, UdpStatusName(a.status), a.generation, a.attempts,
                 (ULONG_PTR)a.ctrlSocket, FormatAddress(a.relayAddr).c_str(), FormatAddress(a.destination).c_str(),
                 now - a.createdAt, now - a.lastSummaryAt, a.txPackets, a.rxPackets, a.txBytes, a.rxBytes,
                 a.txPackets - a.reportedTx, a.rxPackets - a.reportedRx,
                 a.lastTxAt ? static_cast<int64_t>(now - a.lastTxAt) : -1LL,
                 a.lastRxAt ? static_cast<int64_t>(now - a.lastRxAt) : -1LL, a.pendingIo, a.ioErrors, a.wouldBlock,
                 a.blockedSends, a.lastIoError, a.lastAssociationError, a.associationStage);
        a.reportedTx = a.txPackets;
        a.reportedRx = a.rxPackets;
        a.lastSummaryAt = now;
    }

    struct UdpIoObservation {
        uint64_t flowId = 0, generation = 0, startedAt = 0;
        const char* api = "unknown";
        const char* completionApi = "inline";
        const char* stage = "transport";
        bool pending = false;
        bool proxied = false;
    };

    static void CaptureUdpObservation(SOCKET s, UdpIoObservation& observation, const char* api,
                                      const sockaddr_in* destination = nullptr) {
        Logger::ErrorScope preserveError;
        observation.api = api;
        observation.startedAt = GetTickCount64();
        std::unique_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(s);
        if (it == g_udpMap.end())
            return;
        auto& a = it->second;
        observation.flowId = a.flowId;
        observation.generation = a.generation;
        if (destination && !IsSameAddr(*destination, a.destination)) {
            a.destination = *destination;
            LOG_INFO("event=udp.destination flow=%llu socket=%llu destination=%s api=%s", a.flowId, (ULONG_PTR)s,
                     FormatAddress(*destination).c_str(), api);
        }
    }

    static void RecordUdpIo(SOCKET s, const UdpIoObservation& observation, UdpIoKind kind, DWORD bytes, int error,
                            const sockaddr_in* source = nullptr) {
        Logger::ErrorScope preserveError;
        const uint64_t now = GetTickCount64();
        const char* direction = kind == UdpIoKind::Send ? "send" : "receive";
        std::unique_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(s);
        if (it == g_udpMap.end() || it->second.flowId != observation.flowId) {
            LOG_DEBUG("event=udp.io.late_completion flow=%llu socket=%llu generation=%llu direction=%s api=%s "
                      "bytes=%lu error=%d",
                      observation.flowId, (ULONG_PTR)s, observation.generation, direction, observation.api, bytes,
                      error);
            return;
        }
        auto& a = it->second;
        if (observation.pending && a.pendingIo)
            --a.pendingIo;
        if (error == WSAEWOULDBLOCK) {
            ++a.wouldBlock;
        } else if (error) {
            ++a.ioErrors;
            if (error != a.lastIoError || now - a.lastIoErrorLogAt >= 10000) {
                LOG_WARN("event=udp.io.failed flow=%llu socket=%llu generation=%llu direction=%s api=%s stage=%s "
                         "error=%d io_errors=%llu elapsed_ms=%llu",
                         a.flowId, (ULONG_PTR)s, observation.generation, direction, observation.api, observation.stage,
                         error, a.ioErrors, now - observation.startedAt);
                a.lastIoErrorLogAt = now;
            }
            a.lastIoError = error;
        } else if (observation.proxied) {
            uint64_t& packets = kind == UdpIoKind::Send ? a.txPackets : a.rxPackets;
            uint64_t& totalBytes = kind == UdpIoKind::Send ? a.txBytes : a.rxBytes;
            uint64_t& lastAt = kind == UdpIoKind::Send ? a.lastTxAt : a.lastRxAt;
            uint64_t& firstGeneration = kind == UdpIoKind::Send ? a.firstTxGeneration : a.firstRxGeneration;
            ++packets;
            totalBytes += bytes;
            lastAt = now;
            if (observation.generation == a.generation && firstGeneration != a.generation) {
                firstGeneration = a.generation;
                LOG_INFO(
                    "event=udp.io.first flow=%llu socket=%llu generation=%llu direction=%s api=%s bytes=%lu source=%s",
                    a.flowId, (ULONG_PTR)s, a.generation, direction, observation.api, bytes,
                    source ? FormatAddress(*source).c_str() : "none");
            }
        }
        LOG_TRACE("event=udp.io.complete flow=%llu socket=%llu generation=%llu direction=%s api=%s completion_api=%s "
                  "route=%s bytes=%lu error=%d elapsed_ms=%llu",
                  a.flowId, (ULONG_PTR)s, observation.generation, direction, observation.api, observation.completionApi,
                  observation.proxied ? "proxy"
                  : error             ? "unknown"
                                      : "direct",
                  bytes, error, now - observation.startedAt);
    }

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
        UdpIoObservation observation;
        sockaddr_in completedSource = {};
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

    static bool GatherApplicationBuffers(LPWSABUF buffers, DWORD count, std::vector<uint8_t>& output, DWORD& size) {
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

    static void ScatterApplicationBuffers(const void* data, DWORD length, const std::vector<WSABUF>& buffers) {
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

    static UdpCompletion CompleteUdpIo(const std::shared_ptr<UdpIoOperation>& operation, int operationError,
                                       DWORD transferred, const char* completionApi = "inline") {
        std::lock_guard<std::mutex> completionLock(operation->completionMutex);
        if (operation->processed)
            return {operation->completedBytes, operation->completionError};

        operation->processed = true;
        operation->observation.completionApi = completionApi;
        operation->completionError = operationError;

        struct CompletionObservation {
            UdpIoOperation& operation;

            ~CompletionObservation() {
                RecordUdpIo(operation.socket, operation.observation, operation.kind, operation.completedBytes,
                            operation.completionError,
                            operation.kind == UdpIoKind::Receive ? &operation.completedSource : nullptr);
            }
        } observation{*operation};

        if (operationError != 0)
            return {0, operationError};

        if (operation->kind == UdpIoKind::Send) {
            if (transferred >= operation->proxyHeaderSize) {
                operation->completedBytes =
                    (std::min)(operation->applicationBufferSize, transferred - operation->proxyHeaderSize);
            }
            return {operation->completedBytes, 0};
        }

        if (transferred > operation->buffer.size()) {
            operation->observation.stage = "receive_buffer_size";
            operation->completionError = WSAEMSGSIZE;
            return {0, operation->completionError};
        }

        const void* payload = operation->buffer.data();
        int payloadLength = static_cast<int>(transferred);
        sockaddr_in source = {};

        if (operation->receivedFrom.ss_family == AF_INET) {
            source = *reinterpret_cast<const sockaddr_in*>(&operation->receivedFrom);
            if (IsUdpRelaySource(*operation, source)) {
                operation->observation.proxied = true;
                if (!Socks5::DecodeUdpPacket(operation->buffer.data(), static_cast<int>(transferred), source, payload,
                                     payloadLength)) {
                    LOG_DEBUG("event=udp.unwrap.failed flow=%llu socket=%llu generation=%llu relay=%s wire_bytes=%lu",
                              operation->observation.flowId, (ULONG_PTR)operation->socket,
                              operation->observation.generation, FormatAddress(source).c_str(), transferred);
                    LOG_DEBUG("event=udp.unwrap.header flow=%llu reserved_hi=%u reserved_lo=%u fragment=%u atyp=%u "
                              "header_present=%s",
                              operation->observation.flowId, transferred > 0 ? operation->buffer[0] : 0,
                              transferred > 1 ? operation->buffer[1] : 0, transferred > 2 ? operation->buffer[2] : 0,
                              transferred > 3 ? operation->buffer[3] : 0, transferred >= 4 ? "yes" : "no");
                    operation->completionError = WSAECONNRESET;
                    operation->observation.stage = "unwrap";
                    return {0, operation->completionError};
                }
            }
        }

        const DWORD available = operation->applicationBufferSize;
        operation->completedSource = source;
        const DWORD copied = (std::min)(available, static_cast<DWORD>(payloadLength));
        ScatterApplicationBuffers(payload, copied, operation->applicationBuffers);
        operation->completedBytes = copied;

        if (operation->applicationFrom && operation->applicationFromLength) {
            const int copyLength =
                (std::min)(operation->applicationFromCapacity, static_cast<int>(sizeof(sockaddr_in)));
            if (copyLength > 0)
                memcpy(operation->applicationFrom, &source, copyLength);
            *operation->applicationFromLength = sizeof(sockaddr_in);
        }
        if (operation->applicationFlags)
            *operation->applicationFlags = operation->receiveFlags;

        if (static_cast<DWORD>(payloadLength) > available) {
            operation->completionError = WSAEMSGSIZE;
            operation->observation.stage = "application_buffer_size";
        }

        return {operation->completedBytes, operation->completionError};
    }

    static bool RegisterUdpIo(const std::shared_ptr<UdpIoOperation>& operation) {
        std::lock_guard<std::mutex> lock(g_udpIoMutex);
        auto existing = g_udpIoOperations.find(operation->overlapped);
        if (existing != g_udpIoOperations.end()) {
            std::lock_guard<std::mutex> completionLock(existing->second->completionMutex);
            if (!existing->second->processed) {
                LOG_WARN("event=udp.io.overlapped_reused flow=%llu socket=%llu overlapped=%p previous_flow=%llu "
                         "previous_socket=%llu",
                         operation->observation.flowId, (ULONG_PTR)operation->socket, operation->overlapped,
                         existing->second->observation.flowId, (ULONG_PTR)existing->second->socket);
                WSASetLastError(WSAEINVAL);
                return false;
            }
            g_udpIoOperations.erase(existing);
        }
        g_udpIoOperations.emplace(operation->overlapped, operation);
        {
            std::unique_lock<std::shared_mutex> stateLock(g_stateMutex);
            auto it = g_udpMap.find(operation->socket);
            if (it != g_udpMap.end() && it->second.flowId == operation->observation.flowId) {
                ++it->second.pendingIo;
                operation->observation.pending = true;
            }
        }
        LOG_TRACE("event=udp.io.register flow=%llu socket=%llu generation=%llu api=%s overlapped=%p notification=%s",
                  operation->observation.flowId, (ULONG_PTR)operation->socket, operation->observation.generation,
                  operation->observation.api, operation->overlapped,
                  operation->completionRoutine ? "callback" : "event_or_iocp");
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

    static void CALLBACK UdpIoCompletionRoutine(DWORD error, DWORD transferred, LPWSAOVERLAPPED overlapped,
                                                DWORD flags) {
        auto operation = TakeUdpIo(overlapped);
        if (!operation)
            return;

        UdpCompletion completion = CompleteUdpIo(operation, static_cast<int>(error), transferred, "callback");
        if (operation->completionRoutine) {
            operation->completionRoutine(static_cast<DWORD>(completion.error), completion.bytes, overlapped, flags);
        }
    }

    static void TrackUdpSocket(SOCKET s) {
        std::unique_lock<std::shared_mutex> lock(g_stateMutex);
        if (g_udpMap.find(s) != g_udpMap.end())
            return;

        Logger::ErrorScope preserveError;
        UdpAssociation association;
        association.flowId = g_nextUdpFlow.fetch_add(1, std::memory_order_relaxed);
        association.instanceId = association.flowId;
        association.createdAt = association.lastSummaryAt = GetTickCount64();
        g_udpMap.emplace(s, association);
        sockaddr_in local = {};
        int size = sizeof(local);
        const bool hasLocal = getsockname(s, reinterpret_cast<sockaddr*>(&local), &size) == 0;
        LOG_INFO("event=udp.track flow=%llu socket=%llu local=%s mode=%s", association.flowId, (ULONG_PTR)s,
                 hasLocal ? FormatAddress(local).c_str() : "unknown",
                 g_nonBlockingSockets.count(s) ? "nonblocking" : "blocking");
    }

    void MarkUdpAssociationPending(SOCKET s, SOCKET expectedControlSocket, const char* reason, int error) {
        Logger::ErrorScope preserveError;
        std::unique_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(s);
        if (it == g_udpMap.end())
            return;
        if (expectedControlSocket != INVALID_SOCKET && it->second.ctrlSocket != expectedControlSocket)
            return;

        LOG_WARN("event=udp.associate.pending flow=%llu socket=%llu generation=%llu from=%s reason=%s error=%d "
                 "control=%llu relay=%s",
                 it->second.flowId, (ULONG_PTR)s, it->second.generation, UdpStatusName(it->second.status), reason,
                 error, (ULONG_PTR)it->second.ctrlSocket, FormatAddress(it->second.relayAddr).c_str());
        if (it->second.ctrlSocket != INVALID_SOCKET) {
            Hooks::Real_closesocket(it->second.ctrlSocket);
            it->second.ctrlSocket = INVALID_SOCKET;
        }
        it->second.relayAddr = {};
        it->second.status = UdpAssociation::Status::PendingAssociate;
        g_pendingUdp.insert(s);
        g_udpAssociationChanged.notify_all();
    }

    bool TryUdpAssociate(UdpAssociation& out, uint64_t deadline) {
        out = {};
        out.status = UdpAssociation::Status::Idle;

        out.associationStage = "socket_create";
        SOCKET ctrl = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ctrl == INVALID_SOCKET) {
            return false;
        }

        u_long mode = 1;
        out.associationStage = "socket_mode";
        if (Hooks::Real_ioctlsocket(ctrl, FIONBIO, &mode) != 0) {
            int error = WSAGetLastError();
            Hooks::Real_closesocket(ctrl);
            WSASetLastError(error);
            return false;
        }

        if (deadline == 0)
            deadline = MakeDeadline(g_cfg.connectTimeout);
        out.associationStage = "proxy_connect";
        int rc = Hooks::Real_connect(ctrl, reinterpret_cast<const sockaddr*>(&g_proxyAddr), sizeof(g_proxyAddr));
        if (rc == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                out.associationStage = "proxy_connect_wait";
                if (!WaitForConnect(ctrl, RemainingTimeout(deadline))) {
                    int error = WSAGetLastError();
                    Hooks::Real_closesocket(ctrl);
                    WSASetLastError(error);
                    return false;
                }
                int soErr = 0;
                int soLen = sizeof(soErr);
                out.associationStage = "proxy_connect_result";
                if (getsockopt(ctrl, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &soLen) != 0) {
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

        LOG_DEBUG("event=udp.control.connected control=%llu proxy=%s", (ULONG_PTR)ctrl,
                  FormatAddress(g_proxyAddr).c_str());
        out.associationStage = "socks_greeting_auth";
        if (!Socks5::Handshake(ctrl, g_cfg.user.c_str(), g_cfg.password.c_str(), deadline)) {
            int error = WSAGetLastError();
            Hooks::Real_closesocket(ctrl);
            WSASetLastError(error);
            return false;
        }

        sockaddr_in relay;
        out.associationStage = "socks_udp_associate";
        if (!Socks5::RequestUdpAssociate(ctrl, relay, deadline)) {
            int error = WSAGetLastError();
            Hooks::Real_closesocket(ctrl);
            WSASetLastError(error);
            return false;
        }

        out.ctrlSocket = ctrl;
        out.relayAddr = relay;
        out.status = UdpAssociation::Status::Associated;
        out.associationStage = "ready";
        return true;
    }

    static bool ActivateUdpRelay(SOCKET s, std::unique_lock<std::shared_mutex>& lock,
                                 std::map<SOCKET, UdpAssociation>::iterator& it, uint64_t deadline) {
        if (it->second.status == UdpAssociation::Status::Associating)
            return false;

        LogUdpAssociationStart(s, it->second, "send");
        const uint64_t instanceId = it->second.instanceId;
        it->second.status = UdpAssociation::Status::Associating;
        lock.unlock();
        UdpAssociation temp;
        bool ok = TryUdpAssociate(temp, deadline);
        int associationError = ok ? 0 : WSAGetLastError();
        lock.lock();
        it = g_udpMap.find(s);
        if (it == g_udpMap.end() || it->second.instanceId != instanceId ||
            it->second.status != UdpAssociation::Status::Associating) {
            if (temp.ctrlSocket != INVALID_SOCKET) {
                Hooks::Real_closesocket(temp.ctrlSocket);
            }
            WSASetLastError(WSAENOTSOCK);
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
        LogUdpAssociationResult(s, it->second, ok, associationError, temp.associationStage);
        g_udpAssociationChanged.notify_all();
        return ok;
    }

    int WSAAPI Mine_bind(SOCKET s, const sockaddr* addr, int namelen) {
        bool isUdp = IsUdpSocket(s);

        int rc = Hooks::Real_bind(s, addr, namelen);
        if (isUdp && rc != 0) {
            LOG_WARN("event=udp.bind.failed socket=%llu wsa_error=%d", (ULONG_PTR)s, WSAGetLastError());
        }
        if (rc == 0 && isUdp)
            TrackUdpSocket(s);

        return rc;
    }

    static bool IsDirectUdpDestination(const sockaddr_in& destination) {
        return IsLocalAddr(destination) || IsSameAddr(destination, g_proxyAddr) || IsMulticast(destination);
    }

    static bool GetUdpRelayForSend(SOCKET s, sockaddr_in& relayAddr, SOCKET& controlSocket,
                                   const sockaddr_in& destination, const char* api) {
        std::unique_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(s);
        if (it == g_udpMap.end()) {
            WSASetLastError(WSAENOTSOCK);
            return false;
        }

        if (!IsSameAddr(destination, it->second.destination)) {
            it->second.destination = destination;
            LOG_INFO("event=udp.destination flow=%llu socket=%llu destination=%s api=%s", it->second.flowId,
                     (ULONG_PTR)s, FormatAddress(destination).c_str(), api);
        }

        const uint64_t instanceId = it->second.instanceId;
        const uint64_t deadline = MakeDeadline(g_cfg.connectTimeout);
        auto fail = [&](int error) {
            auto current = g_udpMap.find(s);
            if (current != g_udpMap.end() && current->second.instanceId == instanceId) {
                auto& a = current->second;
                ++a.blockedSends;
                LOG_TRACE("event=udp.send.blocked flow=%llu socket=%llu state=%s api=%s application_error=%d "
                          "blocked_sends=%llu",
                          a.flowId, (ULONG_PTR)s, UdpStatusName(a.status), api, error, a.blockedSends);
            }
            WSASetLastError(error);
            return false;
        };

        // ignore ICMP port errors so a vanished relay does not abort pending receives.
        // detect disconnects through the control TCP connection instead.
        if (!it->second.suppressUdpConnReset) {
            BOOL reportReset = FALSE;
            DWORD returned = 0;
            if (WSAIoctl(s, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &returned, nullptr,
                         nullptr) == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                LOG_WARN("event=udp.connreset.suppress.failed flow=%llu socket=%llu wsa_error=%d", it->second.flowId,
                         (ULONG_PTR)s, error);
                return fail(error);
            }
            it->second.suppressUdpConnReset = true;
        }

        for (;;) {
            // wait for association: a synthetic WSAEWOULDBLOCK has no readiness event.
            if (it->second.status == UdpAssociation::Status::Associating) {
                const int remaining = RemainingTimeout(deadline);
                if (remaining == 0 ||
                    !g_udpAssociationChanged.wait_for(lock, std::chrono::milliseconds(remaining), [&] {
                        auto current = g_udpMap.find(s);
                        return current == g_udpMap.end() || current->second.instanceId != instanceId ||
                               current->second.status != UdpAssociation::Status::Associating;
                    })) {
                    return fail(WSAETIMEDOUT);
                }
                it = g_udpMap.find(s);
                if (it == g_udpMap.end() || it->second.instanceId != instanceId) {
                    WSASetLastError(WSAENOTSOCK);
                    return false;
                }
            }

            if (it->second.status != UdpAssociation::Status::Associated) {
                if (RemainingTimeout(deadline) == 0) {
                    return fail(WSAETIMEDOUT);
                }
                if (!ActivateUdpRelay(s, lock, it, deadline)) {
                    const int error = WSAGetLastError();
                    return fail(error);
                }
            }

            relayAddr = it->second.relayAddr;
            controlSocket = it->second.ctrlSocket;
            int error = 0;
            const char* reason = "connected";
            if (!IsSocketDisconnected(controlSocket, &error, &reason))
                return true;

            LOG_WARN("event=udp.associate.pending flow=%llu socket=%llu generation=%llu trigger=send reason=%s "
                     "wsa_error=%d control=%llu relay=%s",
                     it->second.flowId, (ULONG_PTR)s, it->second.generation, reason, error, (ULONG_PTR)controlSocket,
                     FormatAddress(relayAddr).c_str());
            Hooks::Real_closesocket(controlSocket);
            it->second.ctrlSocket = INVALID_SOCKET;
            it->second.relayAddr = {};
            it->second.status = UdpAssociation::Status::PendingAssociate;
            g_pendingUdp.insert(s);
            if (RemainingTimeout(deadline) == 0) {
                return fail(WSAETIMEDOUT);
            }
        }
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
        if (!GetUdpRelayForSend(s, relayAddr, controlSocket, *dst, "sendto")) {
            return SOCKET_ERROR;
        }

        UdpIoObservation observation;
        CaptureUdpObservation(s, observation, "sendto");
        observation.proxied = true;

        auto packet = Socks5::EncodeUdpPacket(*dst, buf, len);
        int result =
            Hooks::Real_sendto(s, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), flags,
                               reinterpret_cast<const sockaddr*>(&relayAddr), sizeof(relayAddr));
        RecordUdpIo(s, observation, UdpIoKind::Send,
                    result == static_cast<int>(packet.size()) ? static_cast<DWORD>(len) : 0,
                    result == SOCKET_ERROR ? WSAGetLastError() : 0);
        if (result == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK)
                MarkUdpAssociationPending(s, controlSocket, "sendto.failed", error);
            WSASetLastError(error);
        }
        return result == static_cast<int>(packet.size()) ? len : result;
    }

    int WSAAPI Mine_WSASendTo(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent,
                              DWORD dwFlags, const sockaddr* lpTo, int iTolen, LPWSAOVERLAPPED lpOverlapped,
                              LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
        if (!lpTo || iTolen < static_cast<int>(sizeof(sockaddr_in)) || dwBufferCount == 0 || !lpBuffers) {
            return Hooks::Real_WSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpTo, iTolen,
                                         lpOverlapped, lpCompletionRoutine);
        }
        const sockaddr_in* dst = reinterpret_cast<const sockaddr_in*>(lpTo);
        if (dst->sin_family != AF_INET || IsDirectUdpDestination(*dst)) {
            return Hooks::Real_WSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpTo, iTolen,
                                         lpOverlapped, lpCompletionRoutine);
        }

        if (!IsUdpSocket(s)) {
            return Hooks::Real_WSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpTo, iTolen,
                                         lpOverlapped, lpCompletionRoutine);
        }
        TrackUdpSocket(s);

        sockaddr_in relayAddr = {};
        SOCKET controlSocket = INVALID_SOCKET;
        if (!GetUdpRelayForSend(s, relayAddr, controlSocket, *dst, "WSASendTo")) {
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
        operation->buffer = Socks5::EncodeUdpPacket(*dst, payload.data(), static_cast<int>(payloadSize));
        operation->proxyHeaderSize = static_cast<DWORD>(operation->buffer.size()) - payloadSize;
        operation->relayAtStart = relayAddr;
        operation->hasRelayAtStart = true;
        CaptureUdpObservation(s, operation->observation, "WSASendTo", dst);
        operation->observation.proxied = true;

        if (lpOverlapped && !RegisterUdpIo(operation))
            return SOCKET_ERROR;

        DWORD localBytesSent = 0;
        LPDWORD bytesSent = lpNumberOfBytesSent ? lpNumberOfBytesSent : &localBytesSent;
        WSABUF proxyBuffer = {static_cast<ULONG>(operation->buffer.size()),
                              reinterpret_cast<char*>(operation->buffer.data())};
        int result = Hooks::Real_WSASendTo(
            s, &proxyBuffer, 1, bytesSent, dwFlags, reinterpret_cast<const sockaddr*>(&relayAddr), sizeof(relayAddr),
            lpOverlapped, lpOverlapped && lpCompletionRoutine ? UdpIoCompletionRoutine : lpCompletionRoutine);
        int error = result == 0 ? 0 : WSAGetLastError();

        if (result == 0) {
            UdpCompletion completion = CompleteUdpIo(
                operation, 0, *bytesSent != 0 ? *bytesSent : static_cast<DWORD>(operation->buffer.size()));
            if (lpNumberOfBytesSent)
                *lpNumberOfBytesSent = completion.bytes;
        }

        if (result == SOCKET_ERROR) {
            if (error != WSA_IO_PENDING) {
                CompleteUdpIo(operation, error, 0);
                if (lpOverlapped)
                    TakeUdpIo(lpOverlapped);
            }
            if (error != WSA_IO_PENDING && error != WSAEWOULDBLOCK) {
                MarkUdpAssociationPending(s, controlSocket, "WSASendTo.failed", error);
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
        operation->applicationBuffers.push_back({static_cast<ULONG>(len), buf});
        operation->applicationFrom = from;
        operation->applicationFromLength = fromlen;
        operation->applicationFromCapacity = from && fromlen ? *fromlen : 0;
        operation->buffer.resize(static_cast<size_t>(len) + Socks5::UdpMaxHeaderSize);
        operation->hasRelayAtStart = TryGetUdpRelay(s, operation->relayAtStart);
        CaptureUdpObservation(s, operation->observation, "recvfrom");

        int received = Hooks::Real_recvfrom(
            s, reinterpret_cast<char*>(operation->buffer.data()), static_cast<int>(operation->buffer.size()), flags,
            reinterpret_cast<sockaddr*>(&operation->receivedFrom), &operation->receivedFromLength);
        if (received == SOCKET_ERROR) {
            CompleteUdpIo(operation, WSAGetLastError(), 0);
            return received;
        }

        UdpCompletion completion = CompleteUdpIo(operation, 0, static_cast<DWORD>(received));
        if (completion.error != 0) {
            WSASetLastError(completion.error);
            return SOCKET_ERROR;
        }
        return static_cast<int>(completion.bytes);
    }

    int WSAAPI Mine_WSARecvFrom(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd,
                                LPDWORD lpFlags, sockaddr* lpFrom, LPINT lpFromlen, LPWSAOVERLAPPED lpOverlapped,
                                LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
        if (!IsTrackedUdpSocket(s)) {
            return Hooks::Real_WSARecvFrom(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpFrom,
                                           lpFromlen, lpOverlapped, lpCompletionRoutine);
        }

        DWORD applicationBufferSize = 0;
        if (!GetApplicationBufferSize(lpBuffers, dwBufferCount, applicationBufferSize))
            return SOCKET_ERROR;
        if (applicationBufferSize > INT_MAX - Socks5::UdpMaxHeaderSize) {
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
        operation->buffer.resize(static_cast<size_t>(applicationBufferSize) + Socks5::UdpMaxHeaderSize);
        operation->hasRelayAtStart = TryGetUdpRelay(s, operation->relayAtStart);
        CaptureUdpObservation(s, operation->observation, "WSARecvFrom");

        if (lpOverlapped && !RegisterUdpIo(operation))
            return SOCKET_ERROR;

        WSABUF proxyBuffer = {static_cast<ULONG>(operation->buffer.size()),
                              reinterpret_cast<char*>(operation->buffer.data())};
        DWORD localReceived = 0;
        LPDWORD bytesReceived = lpNumberOfBytesRecvd ? lpNumberOfBytesRecvd : &localReceived;
        int status = Hooks::Real_WSARecvFrom(
            s, &proxyBuffer, 1, bytesReceived, &operation->receiveFlags,
            reinterpret_cast<sockaddr*>(&operation->receivedFrom), &operation->receivedFromLength, lpOverlapped,
            lpOverlapped && lpCompletionRoutine ? UdpIoCompletionRoutine : lpCompletionRoutine);
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
            CompleteUdpIo(operation, error, 0);
            if (lpOverlapped)
                TakeUdpIo(lpOverlapped);
            WSASetLastError(error);
        }

        return status;
    }

    BOOL WSAAPI Mine_WSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait,
                                            LPDWORD lpdwFlags) {
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
        UdpCompletion completion = CompleteUdpIo(operation, error, transferred, "WSAGetOverlappedResult");
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
                                               PULONG_PTR completionKey, LPOVERLAPPED* overlapped, DWORD milliseconds) {
        BOOL result = Hooks::Real_GetQueuedCompletionStatus(completionPort, bytesTransferred, completionKey, overlapped,
                                                            milliseconds);
        int error = result ? 0 : static_cast<int>(GetLastError());

        LPWSAOVERLAPPED completed = overlapped ? reinterpret_cast<LPWSAOVERLAPPED>(*overlapped) : nullptr;
        auto operation = TakeUdpIo(completed);
        if (!operation) {
            if (!result)
                SetLastError(error);
            return result;
        }

        const DWORD transferred = bytesTransferred ? *bytesTransferred : 0;
        UdpCompletion completion = CompleteUdpIo(operation, error, transferred, "GetQueuedCompletionStatus");
        if (bytesTransferred)
            *bytesTransferred = completion.bytes;

        if (completion.error != 0) {
            SetLastError(completion.error);
            return FALSE;
        }
        return TRUE;
    }

    BOOL WINAPI Mine_GetQueuedCompletionStatusEx(HANDLE completionPort, LPOVERLAPPED_ENTRY entries, ULONG count,
                                                 PULONG removed, DWORD milliseconds, BOOL alertable) {
        BOOL result =
            Hooks::Real_GetQueuedCompletionStatusEx(completionPort, entries, count, removed, milliseconds, alertable);
        DWORD error = result ? ERROR_SUCCESS : GetLastError();
        if (!result || !entries || !removed) {
            if (!result)
                SetLastError(error);
            return result;
        }

        for (ULONG i = 0; i < *removed; ++i) {
            auto operation = TakeUdpIo(reinterpret_cast<LPWSAOVERLAPPED>(entries[i].lpOverlapped));
            if (!operation)
                continue;

            const int operationError = entries[i].Internal == 0 ? 0 : WSA_OPERATION_ABORTED;
            if (entries[i].Internal != 0) {
                LOG_WARN("event=udp.iocp.status flow=%llu socket=%llu generation=%llu api=GetQueuedCompletionStatusEx "
                         "ntstatus=0x%llX mapped_error=%d",
                         operation->observation.flowId, (ULONG_PTR)operation->socket, operation->observation.generation,
                         static_cast<uint64_t>(entries[i].Internal), operationError);
            }
            UdpCompletion completion = CompleteUdpIo(operation, operationError, entries[i].dwNumberOfBytesTransferred,
                                                     "GetQueuedCompletionStatusEx");
            entries[i].dwNumberOfBytesTransferred = completion.bytes;
        }
        return TRUE;
    }

    static BOOL FinishKernelOverlappedResult(BOOL result, DWORD error, LPOVERLAPPED overlapped,
                                             LPDWORD bytesTransferred, const char* api) {
        // a timeout or interrupted wait leaves I/O pending; keep its buffers alive.
        if (!result && (error == ERROR_IO_INCOMPLETE || error == WAIT_TIMEOUT || error == WAIT_IO_COMPLETION)) {
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
        UdpCompletion completion = CompleteUdpIo(operation, result ? 0 : static_cast<int>(error), transferred, api);
        if (bytesTransferred)
            *bytesTransferred = completion.bytes;
        if (completion.error != 0) {
            SetLastError(completion.error);
            return FALSE;
        }
        return TRUE;
    }

    BOOL WINAPI Mine_GetOverlappedResult(HANDLE file, LPOVERLAPPED overlapped, LPDWORD bytesTransferred, BOOL wait) {
        BOOL result = Hooks::Real_GetOverlappedResult(file, overlapped, bytesTransferred, wait);
        DWORD error = result ? ERROR_SUCCESS : GetLastError();
        return FinishKernelOverlappedResult(result, error, overlapped, bytesTransferred, "GetOverlappedResult");
    }

    BOOL WINAPI Mine_GetOverlappedResultEx(HANDLE file, LPOVERLAPPED overlapped, LPDWORD bytesTransferred,
                                           DWORD milliseconds, BOOL alertable) {
        BOOL result = Hooks::Real_GetOverlappedResultEx(file, overlapped, bytesTransferred, milliseconds, alertable);
        DWORD error = result ? ERROR_SUCCESS : GetLastError();
        return FinishKernelOverlappedResult(result, error, overlapped, bytesTransferred, "GetOverlappedResultEx");
    }

    int WSAAPI Mine_closesocket(SOCKET s) {
        uint64_t flow = 0;
        uint64_t createdAt = 0;
        TcpConnection tcp;
        {
            std::unique_lock<std::shared_mutex> lock(g_stateMutex);
            auto it = g_udpMap.find(s);
            if (it != g_udpMap.end()) {
                flow = it->second.flowId;
                createdAt = it->second.createdAt;
                if (it->second.ctrlSocket != INVALID_SOCKET) {
                    Hooks::Real_closesocket(it->second.ctrlSocket);
                }
                LogUdpSummary(s, it->second, "udp.close");
                g_udpMap.erase(it);
                g_pendingUdp.erase(s);
                g_udpAssociationChanged.notify_all();
                LOG_DEBUG("udp socket %llu cleaned", (ULONG_PTR)s);
            }
            g_nonBlockingSockets.erase(s);
            auto connection = g_tcpConnections.find(s);
            if (connection != g_tcpConnections.end()) {
                tcp = connection->second;
                g_tcpConnections.erase(connection);
            }
        }
        const int result = Hooks::Real_closesocket(s);
        if (flow || tcp.id) {
            const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
            const LogLevel level = error ? LogLevel::Warn : LogLevel::Info;
            DROUTE_LOG(
                level,
                "event=socket.close flow=%llu connection=%llu socket=%llu result=%d wsa_error=%d lifetime_ms=%llu",
                flow, tcp.id, (ULONG_PTR)s, result, error, GetTickCount64() - (tcp.id ? tcp.startedAt : createdAt));
        }
        return result;
    }

    int WSAAPI Mine_WSAEventSelect(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents) {
        Logger::ErrorScope resultError;
        const int result = Hooks::Real_WSAEventSelect(s, hEventObject, lNetworkEvents);
        resultError.error = GetLastError();
        if (result == 0) {
            std::unique_lock<std::shared_mutex> lock(g_stateMutex);
            // cancelling notifications leaves the socket nonblocking.
            g_nonBlockingSockets.insert(s);
        }
        LOG_DEBUG("event=socket.notification socket=%llu api=WSAEventSelect event=%p mask=0x%lX result=%d wsa_error=%d",
                  (ULONG_PTR)s, hEventObject, lNetworkEvents, result,
                  result == SOCKET_ERROR ? static_cast<int>(resultError.error) : 0);
        return result;
    }

    int WSAAPI Mine_WSAAsyncSelect(SOCKET s, HWND hWnd, unsigned int wMsg, long lEvent) {
        Logger::ErrorScope resultError;
        const int result = Hooks::Real_WSAAsyncSelect(s, hWnd, wMsg, lEvent);
        resultError.error = GetLastError();
        if (result == 0) {
            std::unique_lock<std::shared_mutex> lock(g_stateMutex);
            g_nonBlockingSockets.insert(s);
        }
        LOG_DEBUG("event=socket.notification socket=%llu api=WSAAsyncSelect window=%p message=%u mask=0x%lX result=%d "
                  "wsa_error=%d",
                  (ULONG_PTR)s, hWnd, wMsg, lEvent, result,
                  result == SOCKET_ERROR ? static_cast<int>(resultError.error) : 0);
        return result;
    }

    int WSAAPI Mine_ioctlsocket(SOCKET s, long cmd, u_long* argp) {
        if (cmd != FIONBIO)
            return Hooks::Real_ioctlsocket(s, cmd, argp);

        Logger::ErrorScope resultError;
        const int result = Hooks::Real_ioctlsocket(s, cmd, argp);
        resultError.error = GetLastError();
        const char* mode = "unknown";
        // read the argument only after winsock has accepted it.
        if (result == 0) {
            const bool nonblocking = *argp != 0;
            mode = nonblocking ? "yes" : "no";
            std::unique_lock<std::shared_mutex> lock(g_stateMutex);
            if (nonblocking) {
                g_nonBlockingSockets.insert(s);
            } else {
                g_nonBlockingSockets.erase(s);
            }
        }
        LOG_DEBUG("event=socket.mode socket=%llu api=ioctlsocket requested_nonblocking=%s result=%d wsa_error=%d",
                  (ULONG_PTR)s, mode, result, result == SOCKET_ERROR ? static_cast<int>(resultError.error) : 0);
        return result;
    }

}
