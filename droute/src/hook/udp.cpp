#include "pch.h"
#include "src/dllmain.hpp"
#include "src/hook/udp.hpp"
#include "src/hook/async.hpp"
#include "src/hook/tcp.hpp"
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

    enum class UdpSendStatus {
        Ready,
        Unavailable,
        Failed
    };

    static std::atomic<uint64_t> g_nextUdpFlow{1};

    static ULONG_PTR CompletionStatus(int error) {
        // facility_ntwin32 preserves the original error code
        return error ? 0xC0070000UL | static_cast<ULONG>(error) : 0;
    }

    static bool IsUdpConnectionError(int error) {
        return error == WSAECONNRESET || error == WSAENETRESET || error == WSAECONNABORTED || error == WSAENETDOWN ||
               error == WSAENETUNREACH || error == WSAEHOSTUNREACH || error == ERROR_NETNAME_DELETED ||
               error == ERROR_CONNECTION_ABORTED || error == ERROR_NETWORK_UNREACHABLE ||
               error == ERROR_HOST_UNREACHABLE;
    }

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

    static void LogUdpAssociationStart(SOCKET s, const UdpAssociation& a, const char* trigger) {
        Logger::ErrorScope preserveError;
        LOG_DEBUG("event=udp.associate.start flow=%llu socket=%llu attempt=%llu trigger=%s proxy=%s", a.flowId,
                  (ULONG_PTR)s, a.attempts, trigger, FormatAddress(g_proxyAddr).c_str());
    }

    static void LogUdpAssociationResult(SOCKET s, const UdpAssociation& a, bool ok, int error, const char* stage) {
        Logger::ErrorScope preserveError;
        const uint64_t elapsed = GetTickCount64() - a.attemptStartedAt;
        if (ok) {
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
    }

    void LogUdpSummary(SOCKET s, UdpAssociation& a, const char* event) {
        Logger::ErrorScope preserveError;
        const uint64_t now = GetTickCount64();
        LOG_DEBUG("event=%s flow=%llu socket=%llu state=%s generation=%llu attempts=%llu control=%llu relay=%s "
                 "destination=%s lifetime_ms=%llu interval_ms=%llu tx_packets=%llu rx_packets=%llu tx_bytes=%llu "
                 "rx_bytes=%llu interval_tx=%llu interval_rx=%llu tx_age_ms=%lld rx_age_ms=%lld pending_io=%llu "
                 "io_errors=%llu would_block=%llu blocked_sends=%llu last_io_error=%d last_associate_error=%d "
                 "associate_stage=%s dropped_packets=%llu dropped_bytes=%llu",
                 event, a.flowId, (ULONG_PTR)s, UdpStatusName(a.status), a.generation, a.attempts,
                 (ULONG_PTR)a.ctrlSocket, FormatAddress(a.relayAddr).c_str(), FormatAddress(a.destination).c_str(),
                 now - a.createdAt, now - a.lastSummaryAt, a.txPackets, a.rxPackets, a.txBytes, a.rxBytes,
                 a.txPackets - a.reportedTx, a.rxPackets - a.reportedRx,
                 a.lastTxAt ? static_cast<int64_t>(now - a.lastTxAt) : -1LL,
                 a.lastRxAt ? static_cast<int64_t>(now - a.lastRxAt) : -1LL, a.pendingIo, a.ioErrors, a.wouldBlock,
                 a.blockedSends, a.lastIoError, a.lastAssociationError, a.associationStage, a.droppedPackets,
                 a.droppedBytes);
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
        bool dropped = false;
    };

    static void CaptureUdpObservation(SOCKET s, UdpIoObservation& observation, const char* api,
                                      const sockaddr_in* destination = nullptr) {
        Logger::ErrorScope preserveError;
        observation.api = api;
        observation.startedAt = GetTickCount64();
        std::lock_guard<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(s);
        if (it == g_udpMap.end())
            return;
        auto& a = it->second;
        observation.flowId = a.flowId;
        observation.generation = a.generation;
        if (destination && !IsSameAddr(*destination, a.destination)) {
            a.destination = *destination;
            LOG_DEBUG("event=udp.destination flow=%llu socket=%llu destination=%s api=%s", a.flowId, (ULONG_PTR)s,
                     FormatAddress(*destination).c_str(), api);
        }
    }

    static void RecordUdpIo(SOCKET s, const UdpIoObservation& observation, UdpIoKind kind, DWORD bytes, int error,
                            const sockaddr_in* source = nullptr) {
        Logger::ErrorScope preserveError;
        const uint64_t now = GetTickCount64();
        const char* direction = kind == UdpIoKind::Send ? "send" : "receive";
        std::lock_guard<std::shared_mutex> lock(g_stateMutex);
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
        } else if (observation.dropped) {
            ++a.droppedPackets;
            a.droppedBytes += bytes;
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
                LOG_DEBUG(
                    "event=udp.io.first flow=%llu socket=%llu generation=%llu direction=%s api=%s bytes=%lu source=%s",
                    a.flowId, (ULONG_PTR)s, a.generation, direction, observation.api, bytes,
                    source ? FormatAddress(*source).c_str() : "none");
            }
        }
        const char* route = "direct";
        if (observation.dropped)
            route = "dropped";
        else if (observation.proxied)
            route = "proxy";
        else if (error)
            route = "unknown";
        LOG_TRACE("event=udp.io.complete flow=%llu socket=%llu generation=%llu direction=%s api=%s completion_api=%s "
                  "route=%s bytes=%lu error=%d elapsed_ms=%llu",
                  a.flowId, (ULONG_PTR)s, observation.generation, direction, observation.api, observation.completionApi,
                  route, bytes, error, now - observation.startedAt);
    }

    struct UdpIoOperation {
        UdpIoKind kind = UdpIoKind::Send;
        SOCKET socket = INVALID_SOCKET;
        SOCKET controlSocket = INVALID_SOCKET;
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

        DWORD receiveFlags = 0;
        WSAOVERLAPPED nativeReceive = {};
        HANDLE receiveEvent = nullptr;
        std::mutex receiveMutex;
        bool receivePending = false;

        sockaddr_in relayAtStart = {};
        bool hasRelayAtStart = false;

        std::mutex completionMutex;
        bool processed = false;
        DWORD completedBytes = 0;
        int completionError = 0;
        UdpIoObservation observation;
        sockaddr_in completedSource = {};
        bool queued = false;
        std::atomic<bool> notified{false};
        HANDLE completedEvent = nullptr;
        HANDLE callerThread = nullptr;
        HANDLE completionPort = nullptr;
        bool completionPacketExpected = false;
        ULONG_PTR completionKey = 0;
        ULONG_PTR completionStatus = 0;
        DWORD sendFlags = 0;
        uint64_t deadline = 0;
        sockaddr_in destination = {};

        ~UdpIoOperation() {
            if (receiveEvent)
                CloseHandle(receiveEvent);
            if (completedEvent)
                CloseHandle(completedEvent);
            if (callerThread)
                CloseHandle(callerThread);
        }
    };

    struct UdpCompletion {
        DWORD bytes = 0;
        int error = 0;
    };

    static std::mutex g_udpIoMutex;
    static std::map<LPWSAOVERLAPPED, std::shared_ptr<UdpIoOperation>> g_udpIoOperations;
    static std::shared_ptr<UdpIoOperation> TakeUdpIo(LPWSAOVERLAPPED overlapped);

    static bool QueuedResult(LPWSAOVERLAPPED overlapped, DWORD timeout, BOOL alertable, LPDWORD bytes, BOOL& result,
                             LPDWORD flags = nullptr) {
        std::shared_ptr<UdpIoOperation> operation;
        {
            std::lock_guard<std::mutex> lock(g_udpIoMutex);
            auto it = g_udpIoOperations.find(overlapped);
            if (it == g_udpIoOperations.end()) {
                // winsock does not decode facility_ntwin32 statuses
                if (overlapped && (overlapped->Internal & 0xFFFF0000UL) == 0xC0070000UL) {
                    if (bytes)
                        *bytes = static_cast<DWORD>(overlapped->InternalHigh);
                    SetLastError(static_cast<DWORD>(overlapped->Internal & 0xFFFF));
                    result = FALSE;
                    return true;
                }
                return false;
            }
            if (!it->second->queued)
                return false;
            operation = it->second;
        }
        DWORD wait = WaitForSingleObjectEx(operation->completedEvent, timeout, alertable);
        if (wait != WAIT_OBJECT_0) {
            SetLastError(wait == WAIT_TIMEOUT && timeout == 0 ? ERROR_IO_INCOMPLETE : wait);
            result = FALSE;
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(operation->completionMutex);
            if (bytes)
                *bytes = operation->completedBytes;
            if (flags)
                *flags = operation->receiveFlags;
            result = operation->completionError == 0;
        }
        if (!operation->completionRoutine && !operation->completionPacketExpected)
            TakeUdpIo(overlapped);
        if (!result)
            SetLastError(operation->completionError);
        return true;
    }

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

        size_t offset = output.size();
        if (size > 65507 - offset) {
            WSASetLastError(WSAEMSGSIZE);
            return false;
        }
        output.resize(offset + size);
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
            const DWORD chunk = std::min(buffer.len, length - copied);
            if (chunk != 0)
                memcpy(buffer.buf, source + copied, chunk);
            copied += chunk;
        }
    }

    bool IsTrackedUdpSocket(SOCKET s) {
        std::shared_lock<std::shared_mutex> lock(g_stateMutex);
        return g_udpMap.find(s) != g_udpMap.end();
    }

    static bool TryGetUdpRelay(SOCKET s, sockaddr_in& relay) {
        std::shared_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(s);
        if (it == g_udpMap.end() || !it->second.relayAddr.sin_port)
            return false;
        relay = it->second.relayAddr;
        return true;
    }

    static bool IsUdpRelaySource(const UdpIoOperation& operation, const sockaddr_in& source) {
        if (operation.hasRelayAtStart && IsSameAddr(source, operation.relayAtStart))
            return true;

        std::shared_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(operation.socket);
        if (it == g_udpMap.end() || it->second.flowId != operation.observation.flowId)
            return false;
        for (const auto& relay : it->second.relays)
            if (IsSameAddr(source, relay))
                return true;
        return false;
    }

    static UdpCompletion CompleteUdpIo(UdpIoOperation& operation, int operationError, DWORD transferred,
                                       const char* completionApi = "inline") {
        std::lock_guard<std::mutex> completionLock(operation.completionMutex);
        if (operation.processed)
            return {operation.completedBytes, operation.completionError};

        operation.processed = true;
        operation.observation.completionApi = completionApi;
        operation.completionError = operationError;

        struct CompletionObservation {
            UdpIoOperation& operation;

            ~CompletionObservation() {
                if (operation.overlapped) {
                    operation.overlapped->InternalHigh = operation.completedBytes;
                    MemoryBarrier();
                    if (operation.queued || operation.overlapped->Internal == 0)
                        operation.overlapped->Internal = CompletionStatus(operation.completionError);
                }
                RecordUdpIo(operation.socket, operation.observation, operation.kind, operation.completedBytes,
                            operation.completionError,
                            operation.kind == UdpIoKind::Receive ? &operation.completedSource : nullptr);
            }
        } observation{operation};

        const bool truncated = operation.kind == UdpIoKind::Receive &&
                               (operationError == WSAEMSGSIZE || operationError == ERROR_MORE_DATA);
        if (operationError != 0 && !truncated) {
            if (operation.kind == UdpIoKind::Send && operation.controlSocket != INVALID_SOCKET &&
                IsUdpConnectionError(operationError))
                MarkUdpAssociationPending(operation.socket, operation.controlSocket, "send.failed", operationError);
            return {0, operationError};
        }

        if (operation.kind == UdpIoKind::Send) {
            if (transferred >= operation.proxyHeaderSize) {
                operation.completedBytes =
                    std::min(operation.applicationBufferSize, transferred - operation.proxyHeaderSize);
            }
            return {operation.completedBytes, 0};
        }

        if (truncated) {
            operation.completionError = WSAEMSGSIZE;
            transferred = static_cast<DWORD>(operation.buffer.size());
        }

        if (transferred > operation.buffer.size()) {
            operation.observation.stage = "receive_buffer_size";
            operation.completionError = WSAEMSGSIZE;
            return {0, operation.completionError};
        }

        const void* payload = operation.buffer.data();
        int payloadLength = static_cast<int>(transferred);
        sockaddr_in source = {};
        const sockaddr* sourceAddress = reinterpret_cast<const sockaddr*>(&operation.receivedFrom);
        int sourceLength = operation.receivedFromLength;

        if (operation.receivedFrom.ss_family == AF_INET) {
            source = *reinterpret_cast<const sockaddr_in*>(&operation.receivedFrom);
            if (IsUdpRelaySource(operation, source)) {
                operation.observation.proxied = true;
                if (!Socks5::DecodeUdpPacket(operation.buffer.data(), static_cast<int>(transferred), source, payload,
                                             payloadLength)) {
                    LOG_DEBUG("event=udp.unwrap.failed flow=%llu socket=%llu generation=%llu relay=%s wire_bytes=%lu",
                              operation.observation.flowId, (ULONG_PTR)operation.socket,
                              operation.observation.generation, FormatAddress(source).c_str(), transferred);
                    LOG_DEBUG("event=udp.unwrap.header flow=%llu reserved_hi=%u reserved_lo=%u fragment=%u atyp=%u "
                              "header_present=%s",
                              operation.observation.flowId, transferred > 0 ? operation.buffer[0] : 0,
                              transferred > 1 ? operation.buffer[1] : 0, transferred > 2 ? operation.buffer[2] : 0,
                              transferred > 3 ? operation.buffer[3] : 0, transferred >= 4 ? "yes" : "no");
                    operation.completionError = WSAECONNRESET;
                    operation.observation.stage = "unwrap";
                    return {0, operation.completionError};
                }
                sourceAddress = reinterpret_cast<const sockaddr*>(&source);
                sourceLength = sizeof(source);
            }
        }

        const DWORD available = operation.applicationBufferSize;
        operation.completedSource = source;
        const DWORD copied = std::min(available, static_cast<DWORD>(payloadLength));
        ScatterApplicationBuffers(payload, copied, operation.applicationBuffers);
        operation.completedBytes = copied;

        if (operation.applicationFrom && operation.applicationFromLength) {
            *operation.applicationFromLength = sourceLength;
            if (operation.applicationFromCapacity < sourceLength) {
                operation.completionError = WSAEFAULT;
                operation.observation.stage = "source_address_size";
                return {operation.completedBytes, operation.completionError};
            }
            memcpy(operation.applicationFrom, sourceAddress, sourceLength);
        }

        if (static_cast<DWORD>(payloadLength) > available) {
            operation.completionError = WSAEMSGSIZE;
            operation.observation.stage = "application_buffer_size";
        }

        return {operation.completedBytes, operation.completionError};
    }

    static bool RegisterUdpIo(const std::shared_ptr<UdpIoOperation>& operation) {
        GetSocketCompletionPort(operation->socket, operation->completionPort, operation->completionKey);
        operation->completionPacketExpected = operation->completionPort && !operation->completionRoutine &&
                                              !(reinterpret_cast<ULONG_PTR>(operation->overlapped->hEvent) & 1);
        std::lock_guard<std::mutex> lock(g_udpIoMutex);
        auto existing = g_udpIoOperations.find(operation->overlapped);
        if (existing != g_udpIoOperations.end()) {
            auto previous = existing->second;
            std::lock_guard<std::mutex> completionLock(previous->completionMutex);
            if (!previous->processed) {
                LOG_WARN("event=udp.io.overlapped_reused flow=%llu socket=%llu overlapped=%p previous_flow=%llu "
                         "previous_socket=%llu",
                         operation->observation.flowId, (ULONG_PTR)operation->socket, operation->overlapped,
                         previous->observation.flowId, (ULONG_PTR)previous->socket);
                WSASetLastError(WSAEINVAL);
                return false;
            }
            g_udpIoOperations.erase(existing);
        }
        g_udpIoOperations.emplace(operation->overlapped, operation);
        {
            std::lock_guard<std::shared_mutex> stateLock(g_stateMutex);
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

        UdpCompletion completion = CompleteUdpIo(*operation, static_cast<int>(error), transferred, "callback");
        if (operation->completionRoutine) {
            operation->completionRoutine(static_cast<DWORD>(completion.error), completion.bytes, overlapped, flags);
        }
    }

    static void TrackUdpSocket(SOCKET s) {
        std::lock_guard<std::shared_mutex> lock(g_stateMutex);
        if (g_udpMap.find(s) != g_udpMap.end())
            return;

        Logger::ErrorScope preserveError;
        UdpAssociation association;
        association.flowId = g_nextUdpFlow.fetch_add(1, std::memory_order_relaxed);
        association.createdAt = association.lastSummaryAt = GetTickCount64();
        g_udpMap.emplace(s, association);
        sockaddr_in local = {};
        int size = sizeof(local);
        const bool hasLocal = getsockname(s, reinterpret_cast<sockaddr*>(&local), &size) == 0;
        LOG_DEBUG("event=udp.track flow=%llu socket=%llu local=%s mode=%s", association.flowId, (ULONG_PTR)s,
                 hasLocal ? FormatAddress(local).c_str() : "unknown",
                 g_nonBlockingSockets.count(s) ? "nonblocking" : "blocking");
    }

    void MarkUdpAssociationPending(SOCKET s, SOCKET expectedControlSocket, const char* reason, int error) {
        Logger::ErrorScope preserveError;
        std::lock_guard<std::shared_mutex> lock(g_stateMutex);
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
        it->second.status = UdpAssociation::Status::PendingAssociate;
        g_udpAssociationChanged.notify_all();
    }

    static bool TryUdpAssociate(UdpAssociation& out, uint64_t deadline) {
        out = {};

        out.associationStage = "socket_create";
        SOCKET ctrl = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ctrl == INVALID_SOCKET) {
            return false;
        }

        struct ControlSocket {
            SOCKET value;

            ~ControlSocket() {
                Logger::ErrorScope preserveError;
                if (value != INVALID_SOCKET)
                    Hooks::Real_closesocket(value);
            }
        } control{ctrl};

        u_long mode = 1;
        out.associationStage = "socket_mode";
        if (Hooks::Real_ioctlsocket(ctrl, FIONBIO, &mode) != 0)
            return false;

        if (deadline == 0)
            deadline = MakeDeadline(g_cfg.connectTimeout);
        out.associationStage = "proxy_connect";
        if (ConnectToProxy(ctrl, deadline) != 0)
            return false;

        LOG_DEBUG("event=udp.control.connected control=%llu proxy=%s", (ULONG_PTR)ctrl,
                  FormatAddress(g_proxyAddr).c_str());
        out.associationStage = "socks_greeting_auth";
        if (!Socks5::Handshake(ctrl, g_cfg.user.c_str(), g_cfg.password.c_str(), deadline)) {
            return false;
        }

        sockaddr_in relay;
        out.associationStage = "socks_udp_associate";
        if (!Socks5::RequestUdpAssociate(ctrl, relay, deadline)) {
            return false;
        }

        out.ctrlSocket = ctrl;
        control.value = INVALID_SOCKET;
        out.relayAddr = relay;
        out.status = UdpAssociation::Status::Associated;
        out.associationStage = "ready";
        return true;
    }

    static bool ActivateUdpRelay(SOCKET s, std::unique_lock<std::shared_mutex>& lock,
                          std::map<SOCKET, UdpAssociation>::iterator& it, uint64_t deadline, const char* trigger) {
        if (it->second.status == UdpAssociation::Status::Associating)
            return false;
        if (GetTickCount64() < it->second.retryAt) {
            WSASetLastError(it->second.lastAssociationError);
            return false;
        }

        ++it->second.attempts;
        it->second.attemptStartedAt = GetTickCount64();
        LogUdpAssociationStart(s, it->second, trigger);
        const uint64_t flowId = it->second.flowId;
        it->second.status = UdpAssociation::Status::Associating;
        lock.unlock();
        UdpAssociation temp;
        bool ok = TryUdpAssociate(temp, deadline);
        int associationError = ok ? 0 : WSAGetLastError();
        lock.lock();
        it = g_udpMap.find(s);
        if (it == g_udpMap.end() || it->second.flowId != flowId ||
            it->second.status != UdpAssociation::Status::Associating) {
            if (temp.ctrlSocket != INVALID_SOCKET) {
                Hooks::Real_closesocket(temp.ctrlSocket);
            }
            WSASetLastError(WSAENOTSOCK);
            return false;
        }
        if (ok && it->second.connected &&
            Hooks::Real_connect(s, reinterpret_cast<const sockaddr*>(&temp.relayAddr), sizeof(temp.relayAddr)) != 0) {
            ok = false;
            associationError = WSAGetLastError();
            temp.associationStage = "udp_connect";
            Hooks::Real_closesocket(temp.ctrlSocket);
            temp.ctrlSocket = INVALID_SOCKET;
        }
        if (ok) {
            if (it->second.ctrlSocket != INVALID_SOCKET)
                Hooks::Real_closesocket(it->second.ctrlSocket);
            it->second.ctrlSocket = temp.ctrlSocket;
            it->second.relayAddr = temp.relayAddr;
            bool knownRelay = false;
            for (const auto& relay : it->second.relays) {
                if (IsSameAddr(relay, temp.relayAddr)) {
                    knownRelay = true;
                    break;
                }
            }
            if (!knownRelay)
                it->second.relays.push_back(temp.relayAddr);
            it->second.status = UdpAssociation::Status::Associated;
            ++it->second.generation;
            it->second.retryAt = 0;
        } else {
            it->second.status = UdpAssociation::Status::PendingAssociate;
            it->second.retryAt = GetTickCount64() + g_cfg.reconnectInterval;
            WSASetLastError(associationError);
        }
        LogUdpAssociationResult(s, it->second, ok, associationError, temp.associationStage);
        it->second.lastAssociationError = associationError;
        it->second.associationStage = temp.associationStage;
        g_udpAssociationChanged.notify_all();
        return ok;
    }

    static void CALLBACK AssociateWorker(PTP_CALLBACK_INSTANCE instance, void* context) {
        CallbackMayRunLong(instance);
        std::unique_ptr<UdpFlow> key(static_cast<UdpFlow*>(context));
        std::unique_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(key->socket);
        if (it == g_udpMap.end() || it->second.flowId != key->flowId)
            return;
        it->second.associationQueued = false;
        if (it->second.status != UdpAssociation::Status::PendingAssociate)
            return;
        try {
            ActivateUdpRelay(key->socket, lock, it, MakeDeadline(g_cfg.connectTimeout), "worker");
        } catch (...) {
            if (!lock.owns_lock())
                lock.lock();
            it = g_udpMap.find(key->socket);
            if (it != g_udpMap.end() && it->second.flowId == key->flowId) {
                it->second.status = UdpAssociation::Status::PendingAssociate;
                it->second.lastAssociationError = WSAENOBUFS;
                g_udpAssociationChanged.notify_all();
            }
            LOG_ERROR("event=udp.associate.worker_failed socket=%llu", (ULONG_PTR)key->socket);
        }
    }

    void QueueUdpAssociation(SOCKET socket) {
        std::lock_guard<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(socket);
        if (it == g_udpMap.end() || it->second.status != UdpAssociation::Status::PendingAssociate ||
            it->second.associationQueued || GetTickCount64() < it->second.retryAt)
            return;
        auto context = std::make_unique<UdpFlow>(UdpFlow{socket, it->second.flowId});
        if (TrySubmitThreadpoolCallback(AssociateWorker, context.get(), nullptr)) {
            it->second.associationQueued = true;
            context.release();
        } else {
            LOG_ERROR("event=udp.associate.queue_failed socket=%llu error=%lu", (ULONG_PTR)socket, GetLastError());
        }
    }

    int WSAAPI Mine_bind(SOCKET s, const sockaddr* addr, int namelen) {
        if (IsInitializingRuntime() || IsResolvingProxy())
            return Hooks::Real_bind(s, addr, namelen);
        if (!EnsureRuntime()) {
            WSASetLastError(WSAENETDOWN);
            return SOCKET_ERROR;
        }
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

    static UdpSendStatus GetUdpRelayForSend(SOCKET s, sockaddr_in& relayAddr, SOCKET& controlSocket,
                                            const sockaddr_in& destination, const char* api, uint64_t expectedFlow = 0,
                                            bool allowWait = true, uint64_t deadline = 0) {
        std::unique_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(s);
        if (it == g_udpMap.end() || (expectedFlow && it->second.flowId != expectedFlow)) {
            WSASetLastError(WSAENOTSOCK);
            return UdpSendStatus::Failed;
        }

        if (!IsSameAddr(destination, it->second.destination)) {
            it->second.destination = destination;
            LOG_DEBUG("event=udp.destination flow=%llu socket=%llu destination=%s api=%s", it->second.flowId,
                     (ULONG_PTR)s, FormatAddress(destination).c_str(), api);
        }

        const uint64_t flowId = it->second.flowId;
        if (!deadline)
            deadline = MakeDeadline(g_cfg.connectTimeout);
        auto fail = [&](int error) {
            auto current = g_udpMap.find(s);
            if (current != g_udpMap.end() && current->second.flowId == flowId) {
                auto& a = current->second;
                ++a.blockedSends;
                LOG_TRACE("event=udp.send.blocked flow=%llu socket=%llu state=%s api=%s application_error=%d "
                          "blocked_sends=%llu",
                          a.flowId, (ULONG_PTR)s, UdpStatusName(a.status), api, error, a.blockedSends);
            }
            WSASetLastError(error);
            return UdpSendStatus::Failed;
        };

        // ignore relay icmp errors to preserve pending receives
        // detect disconnects through the control socket
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
            // reconnecting loses datagrams, not the application's udp socket
            if (it->second.generation && it->second.status != UdpAssociation::Status::Associated) {
                lock.unlock();
                QueueUdpAssociation(s);
                return UdpSendStatus::Unavailable;
            }
            if (!allowWait && it->second.status != UdpAssociation::Status::Associated)
                return fail(WSAEWOULDBLOCK);
            // queued sends handle their own completion notification
            if (it->second.status == UdpAssociation::Status::Associating) {
                const int remaining = RemainingTimeout(deadline);
                if (remaining == 0 ||
                    !g_udpAssociationChanged.wait_for(lock, std::chrono::milliseconds(remaining), [&] {
                        auto current = g_udpMap.find(s);
                        return current == g_udpMap.end() || current->second.flowId != flowId ||
                               current->second.status != UdpAssociation::Status::Associating;
                    })) {
                    return fail(WSAETIMEDOUT);
                }
                it = g_udpMap.find(s);
                if (it == g_udpMap.end() || it->second.flowId != flowId) {
                    WSASetLastError(WSAENOTSOCK);
                    return UdpSendStatus::Failed;
                }
                continue;
            }

            if (it->second.status != UdpAssociation::Status::Associated) {
                if (RemainingTimeout(deadline) == 0) {
                    return fail(WSAETIMEDOUT);
                }
                if (!ActivateUdpRelay(s, lock, it, deadline, "send")) {
                    const int error = WSAGetLastError();
                    return fail(error);
                }
            }

            relayAddr = it->second.relayAddr;
            controlSocket = it->second.ctrlSocket;
            int error = 0;
            const char* reason = "connected";
            if (!IsSocketDisconnected(controlSocket, &error, &reason))
                return UdpSendStatus::Ready;

            LOG_WARN("event=udp.associate.pending flow=%llu socket=%llu generation=%llu trigger=send reason=%s "
                     "wsa_error=%d control=%llu relay=%s",
                     it->second.flowId, (ULONG_PTR)s, it->second.generation, reason, error, (ULONG_PTR)controlSocket,
                     FormatAddress(relayAddr).c_str());
            Hooks::Real_closesocket(controlSocket);
            it->second.ctrlSocket = INVALID_SOCKET;
            it->second.status = UdpAssociation::Status::PendingAssociate;
        }
    }

    bool GetUdpPeer(SOCKET socket, sockaddr_in& peer) {
        std::shared_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_udpMap.find(socket);
        if (it == g_udpMap.end() || !it->second.connected)
            return false;
        peer = it->second.peer;
        return true;
    }

    int ConnectUdpSocket(SOCKET socket, const sockaddr* address, int length) {
        if (!address || length < static_cast<int>(sizeof(sockaddr_in)) || address->sa_family != AF_INET ||
            reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr == INADDR_ANY ||
            IsDirectUdpDestination(*reinterpret_cast<const sockaddr_in*>(address))) {
            int result = Hooks::Real_connect(socket, address, length);
            if (result == 0) {
                std::lock_guard<std::shared_mutex> lock(g_stateMutex);
                auto it = g_udpMap.find(socket);
                if (it != g_udpMap.end())
                    it->second.connected = false;
            }
            return result;
        }

        sockaddr_in local = {};
        int size = sizeof(local);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &size) != 0) {
            if (WSAGetLastError() != WSAEINVAL)
                return SOCKET_ERROR;
            local.sin_family = AF_INET;
            if (Hooks::Real_bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
                return SOCKET_ERROR;
        }
        TrackUdpSocket(socket);
        const auto peer = *reinterpret_cast<const sockaddr_in*>(address);
        {
            std::lock_guard<std::shared_mutex> lock(g_stateMutex);
            auto it = g_udpMap.find(socket);
            if (it == g_udpMap.end()) {
                WSASetLastError(WSAENOTSOCK);
                return SOCKET_ERROR;
            }
            auto& association = it->second;
            if (association.status == UdpAssociation::Status::Associated &&
                Hooks::Real_connect(socket, reinterpret_cast<const sockaddr*>(&association.relayAddr),
                                    sizeof(association.relayAddr)) != 0)
                return SOCKET_ERROR;
            association.peer = peer;
            association.connected = true;
            if (association.status == UdpAssociation::Status::Idle)
                association.status = UdpAssociation::Status::PendingAssociate;
        }
        // udp connect sets the peer without waiting for the network
        QueueUdpAssociation(socket);
        return 0;
    }

    static void CALLBACK QueuedSendCallback(ULONG_PTR context) {
        std::unique_ptr<std::shared_ptr<UdpIoOperation>> owner(
            reinterpret_cast<std::shared_ptr<UdpIoOperation>*>(context));
        auto operation = *owner;
        TakeUdpIo(operation->overlapped);
        operation->completionRoutine(operation->completionError, operation->completedBytes, operation->overlapped,
                                     operation->receiveFlags);
    }

    static void FinishQueuedIo(const std::shared_ptr<UdpIoOperation>& operation, int error, DWORD bytes) {
        if (operation->notified.exchange(true))
            return;
        UdpCompletion completion = CompleteUdpIo(*operation, error, bytes, "send_queue");
        if (!operation->overlapped)
            return;
        ULONG_PTR event = reinterpret_cast<ULONG_PTR>(operation->overlapped->hEvent);
        operation->completionStatus = CompletionStatus(completion.error);
        operation->overlapped->InternalHigh = completion.bytes;
        MemoryBarrier();
        operation->overlapped->Internal = operation->completionStatus;
        SetEvent(operation->completedEvent);
        if (operation->completionRoutine) {
            auto context = std::make_unique<std::shared_ptr<UdpIoOperation>>(operation);
            if (QueueUserAPC(QueuedSendCallback, operation->callerThread, reinterpret_cast<ULONG_PTR>(context.get())))
                context.release();
            else {
                LOG_WARN("event=udp.callback.failed error=%lu", GetLastError());
                TakeUdpIo(operation->overlapped);
            }
            return;
        }
        if (event & ~ULONG_PTR(1))
            SetEvent(reinterpret_cast<HANDLE>(event & ~ULONG_PTR(1)));
        if (operation->completionPort && !(event & 1)) {
            if (!PostQueuedCompletionStatus(operation->completionPort, completion.bytes, operation->completionKey,
                                            operation->overlapped))
                LOG_WARN("event=udp.completion_post.failed error=%lu", GetLastError());
        }
    }

    static void CALLBACK ReceiveCompleted(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT wait, TP_WAIT_RESULT) {
        std::unique_ptr<std::shared_ptr<UdpIoOperation>> owner(
            static_cast<std::shared_ptr<UdpIoOperation>*>(context));
        auto operation = *owner;
        CloseThreadpoolWait(wait);

        DWORD bytes = 0;
        int error;
        {
            std::lock_guard<std::mutex> lock(operation->receiveMutex);
            BOOL result = Hooks::Real_WSAGetOverlappedResult(operation->socket, &operation->nativeReceive, &bytes,
                                                            FALSE, &operation->receiveFlags);
            error = result ? 0 : WSAGetLastError();
            if (error == WSAENOTSOCK) {
                result = Hooks::Real_GetOverlappedResult(reinterpret_cast<HANDLE>(operation->socket),
                                                         &operation->nativeReceive, &bytes, FALSE);
                error = result ? 0 : static_cast<int>(GetLastError());
            }
            operation->receivePending = false;
        }
        FinishQueuedIo(operation, error, bytes);
    }

    static int BeginUdpReceive(const std::shared_ptr<UdpIoOperation>& operation) {
        operation->queued = true;
        operation->receiveEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        operation->completedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation->receiveEvent || !operation->completedEvent ||
            (operation->completionRoutine &&
             !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &operation->callerThread,
                              THREAD_SET_CONTEXT, FALSE, 0))) {
            WSASetLastError(WSAENOBUFS);
            return SOCKET_ERROR;
        }
        auto context = std::make_unique<std::shared_ptr<UdpIoOperation>>(operation);
        PTP_WAIT wait = CreateThreadpoolWait(ReceiveCompleted, context.get(), nullptr);
        if (!wait) {
            WSASetLastError(WSAENOBUFS);
            return SOCKET_ERROR;
        }

        // only the decoded packet may signal the application's event or completion port
        operation->nativeReceive.hEvent =
            reinterpret_cast<HANDLE>(reinterpret_cast<ULONG_PTR>(operation->receiveEvent) | 1);
        std::unique_lock<std::mutex> lock(operation->receiveMutex);
        if (!RegisterUdpIo(operation)) {
            CloseThreadpoolWait(wait);
            return SOCKET_ERROR;
        }
        operation->overlapped->Internal = 0x103;
        operation->overlapped->InternalHigh = 0;
        if (!operation->completionRoutine) {
            ULONG_PTR event = reinterpret_cast<ULONG_PTR>(operation->overlapped->hEvent) & ~ULONG_PTR(1);
            if (event)
                ResetEvent(reinterpret_cast<HANDLE>(event));
        }
        WSABUF buffer{static_cast<ULONG>(operation->buffer.size()),
                      reinterpret_cast<char*>(operation->buffer.data())};
        DWORD bytes = 0;
        int result = Hooks::Real_WSARecvFrom(
            operation->socket, &buffer, 1, &bytes, &operation->receiveFlags,
            reinterpret_cast<sockaddr*>(&operation->receivedFrom), &operation->receivedFromLength,
            &operation->nativeReceive, nullptr);
        int error = result == 0 ? 0 : WSAGetLastError();
        if (error == WSA_IO_PENDING) {
            operation->receivePending = true;
            context.release();
            SetThreadpoolWait(wait, operation->receiveEvent, nullptr);
            WSASetLastError(WSA_IO_PENDING);
            return SOCKET_ERROR;
        }
        lock.unlock();
        CloseThreadpoolWait(wait);
        if (result == 0) {
            FinishQueuedIo(operation, 0, bytes);
            return 0;
        }
        TakeUdpIo(operation->overlapped);
        CompleteUdpIo(*operation, error, 0);
        WSASetLastError(error);
        return SOCKET_ERROR;
    }

    struct SendQueue {
        std::deque<std::shared_ptr<UdpIoOperation>> packets;
        size_t bytes = 0;
        bool running = false;
    };

    static std::mutex g_sendQueueMutex;
    static std::map<UdpFlow, SendQueue> g_sendQueues;

    static void FailQueuedSends(const UdpFlow& key, int error) {
        std::deque<std::shared_ptr<UdpIoOperation>> pending;
        {
            std::lock_guard<std::mutex> lock(g_sendQueueMutex);
            auto it = g_sendQueues.find(key);
            if (it == g_sendQueues.end())
                return;
            pending.swap(it->second.packets);
            it->second.bytes = 0;
        }
        for (const auto& operation : pending)
            FinishQueuedIo(operation, error, 0);
    }

    static void CALLBACK SendQueueWorker(PTP_CALLBACK_INSTANCE instance, void* context) {
        CallbackMayRunLong(instance);
        std::unique_ptr<UdpFlow> key(static_cast<UdpFlow*>(context));
        for (;;) {
            std::shared_ptr<UdpIoOperation> operation;
            {
                std::lock_guard<std::mutex> lock(g_sendQueueMutex);
                auto& queue = g_sendQueues[*key];
                if (queue.packets.empty()) {
                    g_sendQueues.erase(*key);
                    return;
                }
                operation = queue.packets.front();
                queue.packets.pop_front();
                queue.bytes -= operation->buffer.size();
            }
            if (operation->notified.load())
                continue;
            if (operation->observation.dropped) {
                FinishQueuedIo(operation, 0, static_cast<DWORD>(operation->buffer.size()));
                continue;
            }
            if (!RemainingTimeout(operation->deadline)) {
                FinishQueuedIo(operation, WSAETIMEDOUT, 0);
                continue;
            }
            try {
                sockaddr_in relay = {};
                SOCKET control = INVALID_SOCKET;
                const auto status = GetUdpRelayForSend(operation->socket, relay, control, operation->destination,
                                                       "send_queue", key->flowId, true, operation->deadline);
                if (status == UdpSendStatus::Unavailable) {
                    operation->observation.dropped = true;
                    FinishQueuedIo(operation, 0, static_cast<DWORD>(operation->buffer.size()));
                    continue;
                }
                if (status == UdpSendStatus::Failed) {
                    const int error = WSAGetLastError();
                    FinishQueuedIo(operation, error, 0);
                    if (error != WSAETIMEDOUT)
                        FailQueuedSends(*key, error);
                    continue;
                }
                operation->controlSocket = control;
                if (!RemainingTimeout(operation->deadline)) {
                    FinishQueuedIo(operation, WSAETIMEDOUT, 0);
                    continue;
                }
                HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!event) {
                    FinishQueuedIo(operation, WSAENOBUFS, 0);
                    continue;
                }
                WSAOVERLAPPED overlapped = {};
                // the worker consumes this completion; the application gets its own below
                overlapped.hEvent = reinterpret_cast<HANDLE>(reinterpret_cast<ULONG_PTR>(event) | 1);
                WSABUF buffer{static_cast<ULONG>(operation->buffer.size()),
                              reinterpret_cast<char*>(operation->buffer.data())};
                DWORD sent = 0;
                int result = SOCKET_ERROR;
                int error = WSA_OPERATION_ABORTED;
                {
                    std::shared_lock<std::shared_mutex> lock(g_stateMutex);
                    auto it = g_udpMap.find(operation->socket);
                    if (it != g_udpMap.end() && it->second.flowId == key->flowId && !operation->notified.load()) {
                        result = Hooks::Real_WSASendTo(operation->socket, &buffer, 1, &sent, operation->sendFlags,
                                                       reinterpret_cast<sockaddr*>(&relay), sizeof(relay), &overlapped,
                                                       nullptr);
                        error = result == 0 ? 0 : WSAGetLastError();
                    }
                }
                if (error == WSA_IO_PENDING) {
                    DWORD wait = WaitForSingleObject(event, RemainingTimeout(operation->deadline));
                    if (wait != WAIT_OBJECT_0) {
                        CancelIoEx(reinterpret_cast<HANDLE>(operation->socket), &overlapped);
                        WaitForSingleObject(event, INFINITE);
                    }
                    DWORD flags = 0;
                    result = Hooks::Real_WSAGetOverlappedResult(operation->socket, &overlapped, &sent, FALSE, &flags);
                    error = result ? 0 : WSAGetLastError();
                    if (wait == WAIT_TIMEOUT && error == WSA_OPERATION_ABORTED)
                        error = WSAETIMEDOUT;
                }
                CloseHandle(event);
                FinishQueuedIo(operation, error, sent);
            } catch (...) {
                FinishQueuedIo(operation, WSAENOBUFS, 0);
            }
        }
    }

    static bool QueueUdpSend(const std::shared_ptr<UdpIoOperation>& operation) {
        operation->queued = true;
        operation->deadline = operation->observation.startedAt + g_cfg.connectTimeout;
        if (operation->overlapped) {
            operation->completedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!operation->completedEvent) {
                WSASetLastError(WSAENOBUFS);
                return false;
            }
            if (operation->completionRoutine &&
                !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &operation->callerThread,
                                 THREAD_SET_CONTEXT, FALSE, 0)) {
                WSASetLastError(WSAENOBUFS);
                return false;
            }
            GetSocketCompletionPort(operation->socket, operation->completionPort, operation->completionKey);
        }
        std::lock_guard<std::mutex> lock(g_sendQueueMutex);
        const UdpFlow key{operation->socket, operation->observation.flowId};
        auto& queue = g_sendQueues[key];
        if (queue.bytes + operation->buffer.size() > 1024 * 1024 || queue.packets.size() >= 256) {
            WSASetLastError(WSAENOBUFS);
            return false;
        }
        if (operation->overlapped) {
            operation->overlapped->Internal = 0x103;
            operation->overlapped->InternalHigh = 0;
            ULONG_PTR event = reinterpret_cast<ULONG_PTR>(operation->overlapped->hEvent) & ~ULONG_PTR(1);
            if (event && !operation->completionRoutine)
                ResetEvent(reinterpret_cast<HANDLE>(event));
            if (!RegisterUdpIo(operation))
                return false;
        }
        queue.packets.push_back(operation);
        queue.bytes += operation->buffer.size();
        if (!queue.running) {
            auto context = std::make_unique<UdpFlow>(key);
            if (!TrySubmitThreadpoolCallback(SendQueueWorker, context.get(), nullptr)) {
                queue.packets.pop_back();
                queue.bytes -= operation->buffer.size();
                FinishQueuedIo(operation, WSAENOBUFS, 0);
                g_sendQueues.erase(key);
                return true;
            }
            context.release();
            queue.running = true;
        }
        return true;
    }

    static bool IsNonBlockingSocket(SOCKET socket) {
        std::shared_lock<std::shared_mutex> lock(g_stateMutex);
        return g_nonBlockingSockets.count(socket) != 0;
    }

    static bool HasQueuedSends(SOCKET socket) {
        uint64_t flow = 0;
        {
            std::shared_lock<std::shared_mutex> lock(g_stateMutex);
            auto it = g_udpMap.find(socket);
            if (it == g_udpMap.end())
                return false;
            flow = it->second.flowId;
        }
        std::lock_guard<std::mutex> lock(g_sendQueueMutex);
        return g_sendQueues.count({socket, flow}) != 0;
    }

    static bool CancelQueuedSends(SOCKET socket, LPWSAOVERLAPPED overlapped = nullptr) {
        std::vector<std::shared_ptr<UdpIoOperation>> pending;
        {
            std::lock_guard<std::mutex> lock(g_udpIoMutex);
            for (const auto& entry : g_udpIoOperations)
                if (entry.second->queued && entry.second->kind == UdpIoKind::Send &&
                    !entry.second->notified.load() && entry.second->socket == socket &&
                    (!overlapped || entry.first == overlapped))
                    pending.push_back(entry.second);
        }
        for (const auto& operation : pending)
            FinishQueuedIo(operation, WSA_OPERATION_ABORTED, 0);
        return !pending.empty();
    }

    decltype(&CancelIoEx) Real_CancelIoEx = CancelIoEx;

    BOOL WINAPI Mine_CancelIoEx(HANDLE file, LPOVERLAPPED overlapped) {
        bool cancelled = CancelQueuedSends(reinterpret_cast<SOCKET>(file), overlapped);
        std::vector<std::shared_ptr<UdpIoOperation>> receives;
        {
            std::lock_guard<std::mutex> lock(g_udpIoMutex);
            for (const auto& entry : g_udpIoOperations)
                if (entry.second->receiveEvent && entry.second->socket == reinterpret_cast<SOCKET>(file) &&
                    (!overlapped || entry.first == overlapped))
                    receives.push_back(entry.second);
        }
        for (const auto& operation : receives) {
            std::lock_guard<std::mutex> lock(operation->receiveMutex);
            if (operation->receivePending && Real_CancelIoEx(file, &operation->nativeReceive))
                cancelled = true;
        }
        BOOL result = Real_CancelIoEx(file, overlapped);
        return result || cancelled;
    }

    int WSAAPI Mine_sendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
        if (IsInitializingRuntime() || IsResolvingProxy())
            return Hooks::Real_sendto(s, buf, len, flags, to, tolen);
        if (!EnsureRuntime()) {
            WSASetLastError(WSAENETDOWN);
            return SOCKET_ERROR;
        }
        sockaddr_in peer = {};
        if (!to && GetUdpPeer(s, peer)) {
            to = reinterpret_cast<const sockaddr*>(&peer);
            tolen = sizeof(peer);
        }
        if (!to || tolen < static_cast<int>(sizeof(sockaddr_in)) || len < 0 || (len > 0 && !buf)) {
            return Hooks::Real_sendto(s, buf, len, flags, to, tolen);
        }
        const sockaddr_in* dst = reinterpret_cast<const sockaddr_in*>(to);
        if (dst->sin_family != AF_INET || IsDirectUdpDestination(*dst)) {
            return Hooks::Real_sendto(s, buf, len, flags, to, tolen);
        }

        if (!IsUdpSocket(s))
            return Hooks::Real_sendto(s, buf, len, flags, to, tolen);
        if (len > 65507 - Socks5::UdpHeaderSize) {
            WSASetLastError(WSAEMSGSIZE);
            return SOCKET_ERROR;
        }
        TrackUdpSocket(s);

        const bool nonblocking = IsNonBlockingSocket(s);
        bool queueSend = nonblocking && HasQueuedSends(s);
        sockaddr_in relayAddr = {};
        SOCKET controlSocket = INVALID_SOCKET;
        if (!queueSend) {
            const auto status = GetUdpRelayForSend(s, relayAddr, controlSocket, *dst, "sendto", 0, !nonblocking);
            if (status == UdpSendStatus::Unavailable) {
                UdpIoObservation observation;
                CaptureUdpObservation(s, observation, "sendto", dst);
                observation.dropped = true;
                RecordUdpIo(s, observation, UdpIoKind::Send, static_cast<DWORD>(len), 0);
                return len;
            }
            if (status == UdpSendStatus::Failed) {
                if (nonblocking && WSAGetLastError() == WSAEWOULDBLOCK)
                    queueSend = true;
                else
                    return SOCKET_ERROR;
            }
        }
        if (queueSend) {
            auto operation = std::make_shared<UdpIoOperation>();
            operation->socket = s;
            operation->destination = *dst;
            operation->sendFlags = flags;
            operation->buffer = Socks5::EncodeUdpPacket(*dst, buf, len);
            operation->proxyHeaderSize = Socks5::UdpHeaderSize;
            operation->applicationBufferSize = len;
            CaptureUdpObservation(s, operation->observation, "sendto", dst);
            operation->observation.proxied = true;
            return QueueUdpSend(operation) ? len : SOCKET_ERROR;
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
            if (IsUdpConnectionError(error))
                MarkUdpAssociationPending(s, controlSocket, "sendto.failed", error);
            WSASetLastError(error);
        }
        return result == static_cast<int>(packet.size()) ? len : result;
    }

    int WSAAPI Mine_WSASendTo(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent,
                              DWORD dwFlags, const sockaddr* lpTo, int iTolen, LPWSAOVERLAPPED lpOverlapped,
                              LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
        if (IsInitializingRuntime() || IsResolvingProxy())
            return Hooks::Real_WSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpTo, iTolen,
                                         lpOverlapped, lpCompletionRoutine);
        if (!EnsureRuntime()) {
            WSASetLastError(WSAENETDOWN);
            return SOCKET_ERROR;
        }
        sockaddr_in peer = {};
        if (!lpTo && GetUdpPeer(s, peer)) {
            lpTo = reinterpret_cast<const sockaddr*>(&peer);
            iTolen = sizeof(peer);
        }
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
        auto operation = std::make_shared<UdpIoOperation>();
        operation->socket = s;
        operation->destination = *dst;
        operation->sendFlags = dwFlags;
        operation->overlapped = lpOverlapped;
        operation->completionRoutine = lpCompletionRoutine;
        operation->buffer = Socks5::EncodeUdpPacket(*dst, nullptr, 0);
        operation->proxyHeaderSize = Socks5::UdpHeaderSize;
        if (!GatherApplicationBuffers(lpBuffers, dwBufferCount, operation->buffer, operation->applicationBufferSize))
            return SOCKET_ERROR;
        TrackUdpSocket(s);

        const bool asynchronous = lpOverlapped || IsNonBlockingSocket(s);
        bool queueSend = asynchronous && HasQueuedSends(s);
        bool dropSend = false;
        sockaddr_in relayAddr = {};
        SOCKET controlSocket = INVALID_SOCKET;
        if (!queueSend) {
            const auto status = GetUdpRelayForSend(s, relayAddr, controlSocket, *dst, "WSASendTo", 0, !asynchronous);
            if (status == UdpSendStatus::Unavailable) {
                queueSend = true;
                dropSend = true;
            } else if (status == UdpSendStatus::Failed) {
                if (asynchronous && WSAGetLastError() == WSAEWOULDBLOCK)
                    queueSend = true;
                else
                    return SOCKET_ERROR;
            }
        }
        CaptureUdpObservation(s, operation->observation, "WSASendTo", dst);
        operation->observation.proxied = true;
        operation->observation.dropped = dropSend;
        if (queueSend) {
            if (!QueueUdpSend(operation))
                return SOCKET_ERROR;
            if (lpOverlapped) {
                WSASetLastError(WSA_IO_PENDING);
                return SOCKET_ERROR;
            }
            if (lpNumberOfBytesSent)
                *lpNumberOfBytesSent = operation->applicationBufferSize;
            return 0;
        }

        operation->controlSocket = controlSocket;
        operation->relayAtStart = relayAddr;
        operation->hasRelayAtStart = true;

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
                *operation, 0, *bytesSent != 0 ? *bytesSent : static_cast<DWORD>(operation->buffer.size()));
            if (lpNumberOfBytesSent)
                *lpNumberOfBytesSent = completion.bytes;
        }

        if (result == SOCKET_ERROR) {
            if (error != WSA_IO_PENDING) {
                CompleteUdpIo(*operation, error, 0);
                if (lpOverlapped)
                    TakeUdpIo(lpOverlapped);
            }
            WSASetLastError(error);
        }
        return result;
    }

    int WSAAPI Mine_recvfrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
        if (IsInitializingRuntime() || IsResolvingProxy())
            return Hooks::Real_recvfrom(s, buf, len, flags, from, fromlen);
        if (!EnsureRuntime()) {
            WSASetLastError(WSAENETDOWN);
            return SOCKET_ERROR;
        }
        if (!buf || len < 0 || !IsTrackedUdpSocket(s))
            return Hooks::Real_recvfrom(s, buf, len, flags, from, fromlen);
        if (len > INT_MAX - Socks5::UdpMaxHeaderSize) {
            WSASetLastError(WSAEMSGSIZE);
            return SOCKET_ERROR;
        }

        UdpIoOperation operation;
        operation.kind = UdpIoKind::Receive;
        operation.socket = s;
        operation.applicationBufferSize = static_cast<DWORD>(len);
        operation.applicationBuffers.push_back({static_cast<ULONG>(len), buf});
        operation.applicationFrom = from;
        operation.applicationFromLength = fromlen;
        operation.applicationFromCapacity = from && fromlen ? *fromlen : 0;
        operation.buffer.resize(static_cast<size_t>(len) + Socks5::UdpMaxHeaderSize);
        operation.hasRelayAtStart = TryGetUdpRelay(s, operation.relayAtStart);
        CaptureUdpObservation(s, operation.observation, "recvfrom");

        int received = Hooks::Real_recvfrom(
            s, reinterpret_cast<char*>(operation.buffer.data()), static_cast<int>(operation.buffer.size()), flags,
            reinterpret_cast<sockaddr*>(&operation.receivedFrom), &operation.receivedFromLength);
        const int error = received == SOCKET_ERROR ? WSAGetLastError() : 0;
        UdpCompletion completion = CompleteUdpIo(operation, error, error ? 0 : static_cast<DWORD>(received));
        if (completion.error != 0) {
            WSASetLastError(completion.error);
            return SOCKET_ERROR;
        }
        return static_cast<int>(completion.bytes);
    }

    int WSAAPI Mine_WSARecvFrom(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd,
                                LPDWORD lpFlags, sockaddr* lpFrom, LPINT lpFromlen, LPWSAOVERLAPPED lpOverlapped,
                                LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
        if (IsInitializingRuntime() || IsResolvingProxy())
            return Hooks::Real_WSARecvFrom(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpFrom,
                                           lpFromlen, lpOverlapped, lpCompletionRoutine);
        if (!EnsureRuntime()) {
            WSASetLastError(WSAENETDOWN);
            return SOCKET_ERROR;
        }
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
        operation->receiveFlags = lpFlags ? *lpFlags : 0;
        operation->buffer.resize(static_cast<size_t>(applicationBufferSize) + Socks5::UdpMaxHeaderSize);
        operation->hasRelayAtStart = TryGetUdpRelay(s, operation->relayAtStart);
        CaptureUdpObservation(s, operation->observation, "WSARecvFrom");

        WSABUF proxyBuffer = {static_cast<ULONG>(operation->buffer.size()),
                              reinterpret_cast<char*>(operation->buffer.data())};
        DWORD localReceived = 0;
        LPDWORD bytesReceived = lpNumberOfBytesRecvd ? lpNumberOfBytesRecvd : &localReceived;
        int status;
        if (lpOverlapped) {
            status = BeginUdpReceive(operation);
            if (status == 0) {
                if (lpNumberOfBytesRecvd)
                    *lpNumberOfBytesRecvd = operation->completedBytes;
                if (lpFlags)
                    *lpFlags = operation->receiveFlags;
            }
            return status;
        }
        status = Hooks::Real_WSARecvFrom(
            s, &proxyBuffer, 1, bytesReceived, &operation->receiveFlags,
            reinterpret_cast<sockaddr*>(&operation->receivedFrom), &operation->receivedFromLength, nullptr,
            lpCompletionRoutine);
        int error = status == 0 ? 0 : WSAGetLastError();

        if (status == 0) {
            UdpCompletion completion = CompleteUdpIo(*operation, 0, *bytesReceived);
            if (lpFlags)
                *lpFlags = operation->receiveFlags;
            if (lpNumberOfBytesRecvd)
                *lpNumberOfBytesRecvd = completion.bytes;
            if (completion.error != 0) {
                WSASetLastError(completion.error);
                return SOCKET_ERROR;
            }
        } else if (error != WSA_IO_PENDING) {
            UdpCompletion completion = CompleteUdpIo(*operation, error, 0);
            if (lpFlags && error == WSAEMSGSIZE)
                *lpFlags = operation->receiveFlags;
            if (lpNumberOfBytesRecvd)
                *lpNumberOfBytesRecvd = completion.bytes;
            WSASetLastError(error);
        }

        return status;
    }

    BOOL WSAAPI Mine_WSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait,
                                            LPDWORD lpdwFlags) {
        BOOL queuedResult;
        if (QueuedResult(lpOverlapped, fWait ? INFINITE : 0, FALSE, lpcbTransfer, queuedResult, lpdwFlags)) {
            return queuedResult;
        }
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
        UdpCompletion completion = CompleteUdpIo(*operation, error, transferred, "WSAGetOverlappedResult");
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

        LPWSAOVERLAPPED completed = overlapped ? *overlapped : nullptr;
        auto operation = TakeUdpIo(completed);
        if (!operation) {
            if (!result)
                SetLastError(error);
            return result;
        }

        const DWORD transferred = bytesTransferred ? *bytesTransferred : 0;
        UdpCompletion completion = CompleteUdpIo(*operation, error, transferred, "GetQueuedCompletionStatus");
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
            auto operation = TakeUdpIo(entries[i].lpOverlapped);
            if (!operation)
                continue;

            int operationError = 0;
            if (!operation->queued && entries[i].Internal != 0) {
                DWORD bytes = 0, flags = 0;
                if (!Hooks::Real_WSAGetOverlappedResult(operation->socket, operation->overlapped, &bytes, FALSE,
                                                        &flags))
                    operationError = WSAGetLastError();
            }
            UdpCompletion completion = CompleteUdpIo(*operation, operationError, entries[i].dwNumberOfBytesTransferred,
                                                     "GetQueuedCompletionStatusEx");
            entries[i].dwNumberOfBytesTransferred = completion.bytes;
            if (operation->queued || entries[i].Internal == 0)
                entries[i].Internal = CompletionStatus(completion.error);
            operation->overlapped->Internal = entries[i].Internal;
            operation->overlapped->InternalHigh = completion.bytes;
        }
        return TRUE;
    }

    static BOOL FinishKernelOverlappedResult(BOOL result, DWORD error, LPOVERLAPPED overlapped,
                                             LPDWORD bytesTransferred, const char* api) {
        // timeout does not cancel pending i/o
        if (!result && (error == ERROR_IO_INCOMPLETE || error == WAIT_TIMEOUT || error == WAIT_IO_COMPLETION)) {
            SetLastError(error);
            return result;
        }

        auto operation = TakeUdpIo(overlapped);
        if (!operation) {
            if (!result)
                SetLastError(error);
            return result;
        }

        const DWORD transferred = bytesTransferred ? *bytesTransferred : 0;
        UdpCompletion completion = CompleteUdpIo(*operation, result ? 0 : static_cast<int>(error), transferred, api);
        if (bytesTransferred)
            *bytesTransferred = completion.bytes;
        if (completion.error != 0) {
            SetLastError(completion.error);
            return FALSE;
        }
        return TRUE;
    }

    BOOL WINAPI Mine_GetOverlappedResult(HANDLE file, LPOVERLAPPED overlapped, LPDWORD bytesTransferred, BOOL wait) {
        BOOL queuedResult;
        if (QueuedResult(overlapped, wait ? INFINITE : 0, FALSE, bytesTransferred, queuedResult))
            return queuedResult;
        BOOL result = Hooks::Real_GetOverlappedResult(file, overlapped, bytesTransferred, wait);
        DWORD error = result ? ERROR_SUCCESS : GetLastError();
        return FinishKernelOverlappedResult(result, error, overlapped, bytesTransferred, "GetOverlappedResult");
    }

    BOOL WINAPI Mine_GetOverlappedResultEx(HANDLE file, LPOVERLAPPED overlapped, LPDWORD bytesTransferred,
                                           DWORD milliseconds, BOOL alertable) {
        BOOL queuedResult;
        if (QueuedResult(overlapped, milliseconds, alertable, bytesTransferred, queuedResult))
            return queuedResult;
        BOOL result = Hooks::Real_GetOverlappedResultEx(file, overlapped, bytesTransferred, milliseconds, alertable);
        DWORD error = result ? ERROR_SUCCESS : GetLastError();
        return FinishKernelOverlappedResult(result, error, overlapped, bytesTransferred, "GetOverlappedResultEx");
    }

    int WSAAPI Mine_closesocket(SOCKET s) {
        CancelQueuedSends(s);
        {
            std::lock_guard<std::mutex> lock(g_udpIoMutex);
            for (auto it = g_udpIoOperations.begin(); it != g_udpIoOperations.end();) {
                auto operation = it->second;
                std::lock_guard<std::mutex> completionLock(operation->completionMutex);
                if (operation->socket == s && operation->processed && !operation->completionRoutine &&
                    !operation->completionPacketExpected)
                    it = g_udpIoOperations.erase(it);
                else
                    ++it;
            }
        }
        ForgetAsyncSocket(s);
        uint64_t flow = 0;
        uint64_t createdAt = 0;
        TcpConnection tcp;
        {
            std::lock_guard<std::shared_mutex> lock(g_stateMutex);
            auto it = g_udpMap.find(s);
            if (it != g_udpMap.end()) {
                flow = it->second.flowId;
                createdAt = it->second.createdAt;
                if (it->second.ctrlSocket != INVALID_SOCKET) {
                    Hooks::Real_closesocket(it->second.ctrlSocket);
                }
                LogUdpSummary(s, it->second, "udp.close");
                g_udpMap.erase(it);
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
            const LogLevel level = error ? LogLevel::Warn : LogLevel::Debug;
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
            std::lock_guard<std::shared_mutex> lock(g_stateMutex);
            // cancelling notifications leaves the socket nonblocking
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
            std::lock_guard<std::shared_mutex> lock(g_stateMutex);
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
        int result;
        if (!argp || !SetConnectingSocketMode(s, *argp != 0, result))
            result = Hooks::Real_ioctlsocket(s, cmd, argp);
        resultError.error = GetLastError();
        const char* mode = "unknown";
        // read the argument after winsock accepts it
        if (result == 0) {
            const bool nonblocking = *argp != 0;
            mode = nonblocking ? "yes" : "no";
            std::lock_guard<std::shared_mutex> lock(g_stateMutex);
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
