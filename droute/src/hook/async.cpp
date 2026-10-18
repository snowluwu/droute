#include "pch.h"
#include "src/dllmain.hpp"
#include "src/hook/async.hpp"
#include "src/hook/tcp.hpp"
#include "src/hook/udp.hpp"
#include "src/core/config.hpp"
#include "src/core/logger.hpp"
#include "src/core/utils.hpp"

namespace droute {

    struct AsyncConnection {
        SOCKET socket = INVALID_SOCKET;
        SOCKET transport = INVALID_SOCKET;
        sockaddr_in destination = {};
        bool pending = true;
        bool cancelled = false;
        bool restoreBlocking = false;
        int error = 0;
        long events = 0;
        WSAEVENT event = nullptr;
        HWND window = nullptr;
        UINT message = 0;
        long completedEvents = 0;

        ~AsyncConnection() {
            if (transport != INVALID_SOCKET)
                Hooks::Real_closesocket(transport);
        }
    };

    struct CompletionPort {
        HANDLE handle;
        ULONG_PTR key;
    };

    static std::mutex g_asyncMutex;
    static std::condition_variable g_connectionChanged;
    static std::map<SOCKET, std::shared_ptr<AsyncConnection>> g_connections;
    static std::map<SOCKET, CompletionPort> g_completionPorts;
    static decltype(&CreateIoCompletionPort) Real_CreateIoCompletionPort = CreateIoCompletionPort;
    static decltype(&ExitProcess) Real_ExitProcess = ExitProcess;

    static void WINAPI Async_exit(UINT code) {
        StopRuntime();
        Real_ExitProcess(code);
    }

    void GetSocketCompletionPort(SOCKET socket, HANDLE& port, ULONG_PTR& key) {
        std::lock_guard<std::mutex> lock(g_asyncMutex);
        auto it = g_completionPorts.find(socket);
        if (it != g_completionPorts.end()) {
            port = it->second.handle;
            key = it->second.key;
        }
    }

    static HANDLE WINAPI Async_completionPort(HANDLE file, HANDLE port, ULONG_PTR key, DWORD threads) {
        HANDLE result = Real_CreateIoCompletionPort(file, port, key, threads);
        if (result && file != INVALID_HANDLE_VALUE) {
            Logger::ErrorScope preserveError;
            int type = 0, size = sizeof(type);
            if (!getsockopt(reinterpret_cast<SOCKET>(file), SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type),
                            &size)) {
                std::lock_guard<std::mutex> lock(g_asyncMutex);
                g_completionPorts.emplace(reinterpret_cast<SOCKET>(file), CompletionPort{result, key});
            }
        }
        return result;
    }

    static decltype(&select) Real_select = select;
    static decltype(&WSAPoll) Real_WSAPoll = WSAPoll;
    static decltype(&getsockopt) Real_getsockopt = getsockopt;
    static decltype(&getpeername) Real_getpeername = getpeername;
    static decltype(&WSAEnumNetworkEvents) Real_WSAEnumNetworkEvents = WSAEnumNetworkEvents;
    static decltype(&WSASend) Real_WSASend = WSASend;
    static decltype(&WSARecv) Real_WSARecv = WSARecv;
    static decltype(&WSAEventSelect) Real_eventSelect;
    static decltype(&WSAAsyncSelect) Real_asyncSelect;

    static void NotifyConnection(AsyncConnection& connection) {
        connection.completedEvents = connection.events & (FD_CONNECT | (connection.error ? 0 : FD_WRITE));
        if (connection.window) {
            if (connection.completedEvents & FD_CONNECT)
                PostMessageW(connection.window, connection.message, connection.socket,
                             WSAMAKESELECTREPLY(FD_CONNECT, connection.error));
            if (connection.completedEvents & FD_WRITE)
                PostMessageW(connection.window, connection.message, connection.socket, WSAMAKESELECTREPLY(FD_WRITE, 0));
            connection.completedEvents = 0;
        } else if (connection.event && connection.completedEvents) {
            WSASetEvent(connection.event);
        }
    }

    static void CALLBACK ConnectWorker(PTP_CALLBACK_INSTANCE instance, void* context) {
        CallbackMayRunLong(instance);

        // the callback owns the context; the connection is shared with socket hooks
        std::unique_ptr<std::shared_ptr<AsyncConnection>> owner(
            static_cast<std::shared_ptr<AsyncConnection>*>(context));

        auto connection = *owner;
        const uint64_t started = GetTickCount64();

        int error = 0;
        try {
            if (ConnectViaProxy(connection->transport, connection->destination, MakeDeadline(g_cfg.connectTimeout)))
                error = WSAGetLastError();
        } catch (...) {
            error = WSAENOBUFS;
        }

        std::lock_guard<std::mutex> lock(g_asyncMutex);

        if (connection->cancelled)
            return;

        connection->error = error;
        if (connection->restoreBlocking) {
            u_long blocking = 0;
            if (Hooks::Real_ioctlsocket(connection->socket, FIONBIO, &blocking) && !error)
                connection->error = WSAGetLastError();
        }
        connection->pending = false;
        g_connectionChanged.notify_all();

        // the application's notifications must not expose the socks handshake
        if (connection->window)
            Real_asyncSelect(connection->socket, connection->window, connection->message,
                             connection->events & ~FD_CONNECT);
        else if (connection->event)
            Real_eventSelect(connection->socket, connection->event, connection->events & ~FD_CONNECT);

        NotifyConnection(*connection);
        DROUTE_LOG(connection->error ? LogLevel::Warn : LogLevel::Debug,
                   "event=tcp.connect.completed socket=%llu error=%d elapsed_ms=%llu", (ULONG_PTR)connection->socket,
                   connection->error, GetTickCount64() - started);
    }

    int BeginTcpConnect(SOCKET socket, const sockaddr_in& destination) {
        std::lock_guard<std::mutex> lock(g_asyncMutex);
        auto& connection = g_connections[socket];
        if (connection && connection->transport != INVALID_SOCKET) {
            int error = WSAEISCONN;
            if (connection->pending)
                error = WSAEALREADY;
            else if (connection->error)
                error = connection->error;
            WSASetLastError(error);
            return SOCKET_ERROR;
        }

        if (!connection)
            connection = std::make_shared<AsyncConnection>();
        connection->socket = socket;
        connection->destination = destination;

        WSAPROTOCOL_INFOW protocol = {};
        if (WSADuplicateSocketW(socket, GetCurrentProcessId(), &protocol))
            return SOCKET_ERROR;

        connection->transport =
            WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &protocol, 0, WSA_FLAG_OVERLAPPED);

        if (connection->transport == INVALID_SOCKET)
            return SOCKET_ERROR;

        if (connection->window)
            Real_asyncSelect(socket, connection->window, 0, 0);
        else if (connection->event)
            Real_eventSelect(socket, nullptr, 0);

        auto context = std::make_unique<std::shared_ptr<AsyncConnection>>(connection);

        if (!TrySubmitThreadpoolCallback(ConnectWorker, context.get(), nullptr)) {
            connection->pending = false;
            connection->error = WSAENOBUFS;
            WSASetLastError(WSAENOBUFS);
            return SOCKET_ERROR;
        }

        context.release();
        WSASetLastError(WSAEWOULDBLOCK);

        return SOCKET_ERROR;
    }

    void ForgetAsyncSocket(SOCKET socket) {
        std::lock_guard<std::mutex> lock(g_asyncMutex);
        g_completionPorts.erase(socket);
        auto it = g_connections.find(socket);

        if (it == g_connections.end())
            return;
        it->second->cancelled = true;
        g_connectionChanged.notify_all();

        if (it->second->transport != INVALID_SOCKET && it->second->pending)
            shutdown(it->second->transport, SD_BOTH);
        g_connections.erase(it);
    }

    bool SetConnectingSocketMode(SOCKET socket, bool nonblocking, int& result) {
        std::lock_guard<std::mutex> lock(g_asyncMutex);

        auto it = g_connections.find(socket);
        if (it == g_connections.end() || it->second->transport == INVALID_SOCKET || !it->second->pending)
            return false;
        auto& connection = *it->second;

        if (!nonblocking && connection.events) {
            result = SOCKET_ERROR;
            WSASetLastError(WSAEINVAL);
        } else {
            connection.restoreBlocking = !nonblocking;
            result = 0;
        }

        return true;
    }

    static int SocketError(SOCKET socket, bool wait = false) {
        std::unique_lock<std::mutex> lock(g_asyncMutex);

        auto it = g_connections.find(socket);
        if (it == g_connections.end() || it->second->transport == INVALID_SOCKET)
            return 0;

        auto connection = it->second;
        if (wait && connection->restoreBlocking)
            g_connectionChanged.wait(lock, [&] { return !connection->pending || connection->cancelled; });

        if (connection->cancelled)
            return WSAENOTSOCK;

        return connection->pending ? WSAEWOULDBLOCK : connection->error;
    }

    static int WSAAPI Async_send(SOCKET s, const char* buffer, int length, int flags) {
        sockaddr_in peer;

        if (GetUdpPeer(s, peer))
            return Mine_sendto(s, buffer, length, flags, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));

        int error = SocketError(s, true);
        if (!error)
            return Hooks::Real_send(s, buffer, length, flags);

        WSASetLastError(error);

        return SOCKET_ERROR;
    }

    static int WSAAPI Async_recv(SOCKET s, char* buffer, int length, int flags) {
        if (IsTrackedUdpSocket(s))
            return Mine_recvfrom(s, buffer, length, flags, nullptr, nullptr);

        int error = SocketError(s, true);
        if (!error)
            return Hooks::Real_recv(s, buffer, length, flags);

        WSASetLastError(error);

        return SOCKET_ERROR;
    }

    static int WSAAPI Async_WSASend(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD bytes, DWORD flags,
        LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE callback) {
        sockaddr_in peer;
        if (GetUdpPeer(s, peer))
            return Mine_WSASendTo(s, buffers, count, bytes, flags, reinterpret_cast<const sockaddr*>(&peer),
                                  sizeof(peer), overlapped, callback);
        int error = SocketError(s, overlapped == nullptr);
        if (!error)
            return Real_WSASend(s, buffers, count, bytes, flags, overlapped, callback);

        WSASetLastError(error);

        return SOCKET_ERROR;
    }

    static int WSAAPI Async_WSARecv(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD bytes, LPDWORD flags,
        LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE callback) {
        if (IsTrackedUdpSocket(s))
            return Mine_WSARecvFrom(s, buffers, count, bytes, flags, nullptr, nullptr, overlapped, callback);

        int error = SocketError(s, overlapped == nullptr);
        if (!error)
            return Real_WSARecv(s, buffers, count, bytes, flags, overlapped, callback);

        WSASetLastError(error);

        return SOCKET_ERROR;
    }

    static int WSAAPI Async_getpeername(SOCKET s, sockaddr* address, int* length) {
        sockaddr_in peer;
        if (!GetUdpPeer(s, peer))
            return Real_getpeername(s, address, length);

        if (!address || !length || *length < static_cast<int>(sizeof(peer))) {
            WSASetLastError(WSAEFAULT);
            return SOCKET_ERROR;
        }

        memcpy(address, &peer, sizeof(peer));
        *length = sizeof(peer);

        return 0;
    }

    static int WSAAPI Async_getsockopt(SOCKET s, int level, int option, char* value, int* size) {
        if (level == SOL_SOCKET && option == SO_ERROR && value && size && *size >= sizeof(int)) {
            int error = SocketError(s);
            if (error) {
                *reinterpret_cast<int*>(value) = error == WSAEWOULDBLOCK ? 0 : error;
                *size = sizeof(int);
                return 0;
            }
        }
        return Real_getsockopt(s, level, option, value, size);
    }

    static int WSAAPI Async_eventSelect(SOCKET s, WSAEVENT event, long events) {
        std::lock_guard<std::mutex> lock(g_asyncMutex);

        auto it = g_connections.find(s);
        const bool pending =
            it != g_connections.end() && it->second->transport != INVALID_SOCKET && it->second->pending;
        int result = Real_eventSelect(s, pending ? nullptr : event, pending ? 0 : events);
        if (result != 0)
            return result;

        auto& connection = g_connections[s];
        if (!connection) {
            connection = std::make_shared<AsyncConnection>();
            connection->socket = s;
        }

        connection->event = event;
        connection->window = nullptr;
        connection->events = events;
        connection->restoreBlocking = false;
        if (connection->transport != INVALID_SOCKET && !connection->pending)
            NotifyConnection(*connection);

        return 0;
    }

    static int WSAAPI Async_asyncSelect(SOCKET s, HWND window, UINT message, long events) {
        std::lock_guard<std::mutex> lock(g_asyncMutex);

        auto it = g_connections.find(s);
        const bool pending =
            it != g_connections.end() && it->second->transport != INVALID_SOCKET && it->second->pending;
        int result = Real_asyncSelect(s, window, message, pending ? 0 : events);
        if (result != 0)
            return result;

        auto& connection = g_connections[s];
        if (!connection) {
            connection = std::make_shared<AsyncConnection>();
            connection->socket = s;
        }

        connection->window = window;
        connection->message = message;
        connection->event = nullptr;
        connection->events = events;
        connection->restoreBlocking = false;
        if (connection->transport != INVALID_SOCKET && !connection->pending)
            NotifyConnection(*connection);

        return 0;
    }

    static int WSAAPI Async_enumEvents(SOCKET s, WSAEVENT event, LPWSANETWORKEVENTS events) {
        std::lock_guard<std::mutex> lock(g_asyncMutex);

        int result = Real_WSAEnumNetworkEvents(s, event, events);
        auto it = g_connections.find(s);
        if (!result && events && it != g_connections.end() && it->second->transport != INVALID_SOCKET) {
            auto& connection = *it->second;
            if (connection.pending) {
                *events = {};
            } else {
                events->lNetworkEvents |= connection.completedEvents;
                if (connection.completedEvents & FD_CONNECT)
                    events->iErrorCode[FD_CONNECT_BIT] = connection.error;
                connection.completedEvents = 0;
            }
        }

        return result;
    }

    static int WSAAPI Async_select(int ignored, fd_set* read, fd_set* write, fd_set* except, const timeval* timeout) {
        // let Winsock handle invalid timeouts before converting them to unsigned milliseconds
        if (timeout && (timeout->tv_sec < 0 || timeout->tv_usec < 0))
            return Real_select(ignored, read, write, except, timeout);

        const fd_set inputs[] = {read ? *read : fd_set{}, write ? *write : fd_set{}, except ? *except : fd_set{}};
        uint64_t deadline = UINT64_MAX;
        if (timeout) {
            const uint64_t milliseconds =
                timeout->tv_sec * 1000ULL + (static_cast<uint64_t>(timeout->tv_usec) + 999) / 1000;
            deadline = GetTickCount64() + milliseconds;
        }
        bool waitedForConnection = false;
        for (;;) {
            fd_set sets[] = {inputs[0], inputs[1], inputs[2]};
            bool tracked = false;
            std::vector<SOCKET> failed;
            for (int index = 0; index < 3; ++index) {
                for (u_int i = 0; i < inputs[index].fd_count; ++i) {
                    SOCKET s = inputs[index].fd_array[i];
                    int error = SocketError(s);
                    if (!error)
                        continue;
                    tracked = true;
                    FD_CLR(s, &sets[index]);
                    if (index == 2 && error != WSAEWOULDBLOCK)
                        failed.push_back(s);
                }
            }

            if (!tracked) {
                // preserve the original microsecond timeout when no proxy connection is pending
                if (!waitedForConnection)
                    return Real_select(ignored, read, write, except, timeout);

                const uint64_t now = GetTickCount64();
                const uint64_t remaining = now < deadline ? deadline - now : 0;
                timeval rest = {};
                if (timeout) {
                    rest.tv_sec = static_cast<long>(remaining / 1000);
                    rest.tv_usec = static_cast<long>((remaining % 1000) * 1000);
                }
                if (read)
                    *read = inputs[0];
                if (write)
                    *write = inputs[1];
                if (except)
                    *except = inputs[2];
                return Real_select(ignored, read, write, except, timeout ? &rest : nullptr);
            }

            waitedForConnection = true;
            timeval slice = {};
            int result = 0;

            if (sets[0].fd_count || sets[1].fd_count || sets[2].fd_count) {
                result = Real_select(ignored, &sets[0], &sets[1], &sets[2], &slice);
                if (result == SOCKET_ERROR)
                    return result;
            }

            for (SOCKET s : failed)
                FD_SET(s, &sets[2]);

            result = sets[0].fd_count + sets[1].fd_count + sets[2].fd_count;
            if (result || GetTickCount64() >= deadline) {
                if (read)
                    *read = sets[0];
                if (write)
                    *write = sets[1];
                if (except)
                    *except = sets[2];
                return result;
            }

            const uint64_t now = GetTickCount64();
            if (now < deadline)
                Sleep(static_cast<DWORD>(std::min(10ULL, deadline - now)));
        }
    }

    static int WSAAPI Async_poll(LPWSAPOLLFD descriptors, ULONG count, INT timeout) {
        if (!descriptors)
            return Real_WSAPoll(descriptors, count, timeout);

        const uint64_t deadline = timeout < 0 ? UINT64_MAX : GetTickCount64() + timeout;
        for (;;) {
            std::vector<WSAPOLLFD> ready(descriptors, descriptors + count);
            bool tracked = false;
            ULONG valid = 0;
            for (ULONG i = 0; i < count; ++i) {
                if (SocketError(ready[i].fd)) {
                    tracked = true;
                    ready[i].fd = INVALID_SOCKET;
                } else
                    ++valid;
            }
            if (!tracked)
                return Real_WSAPoll(descriptors, count, timeout < 0 ? -1 : RemainingTimeout(deadline));

            int result = valid ? Real_WSAPoll(ready.data(), count, 0) : 0;
            if (result == SOCKET_ERROR)
                return result;
            result = 0;

            for (ULONG i = 0; i < count; ++i) {
                int error = SocketError(descriptors[i].fd);
                if (error == WSAEWOULDBLOCK)
                    descriptors[i].revents = 0;
                else if (error)
                    descriptors[i].revents = POLLERR;
                else if (ready[i].fd == INVALID_SOCKET)
                    descriptors[i].revents = 0;
                else
                    descriptors[i].revents = ready[i].revents;
                if (descriptors[i].revents)
                    ++result;
            }

            if (result || GetTickCount64() >= deadline)
                return result;
            Sleep(1);
        }
    }

    bool InstallAsyncHooks() {
        if (MH_CreateHookApi(L"kernel32.dll", "ExitProcess", (void*)Async_exit, (void**)&Real_ExitProcess) != MH_OK)
            return false;

        if (MH_CreateHookApi(L"kernel32.dll", "CreateIoCompletionPort", (void*)Async_completionPort,
                             (void**)&Real_CreateIoCompletionPort) != MH_OK)
            return false;

        struct Hook {
            const char* name;
            void* replacement;
            void** original;
        };

        const Hook hooks[] = {{"select", (void*)Async_select, (void**)&Real_select},
                              {"WSAPoll", (void*)Async_poll, (void**)&Real_WSAPoll},
                              {"getsockopt", (void*)Async_getsockopt, (void**)&Real_getsockopt},
                              {"getpeername", (void*)Async_getpeername, (void**)&Real_getpeername},
                              {"WSAEnumNetworkEvents", (void*)Async_enumEvents, (void**)&Real_WSAEnumNetworkEvents},
                              {"send", (void*)Async_send, (void**)&Hooks::Real_send},
                              {"recv", (void*)Async_recv, (void**)&Hooks::Real_recv},
                              {"WSASend", (void*)Async_WSASend, (void**)&Real_WSASend},
                              {"WSARecv", (void*)Async_WSARecv, (void**)&Real_WSARecv}};
        for (const auto& hook : hooks) {
            MH_STATUS result = MH_CreateHookApi(L"ws2_32.dll", hook.name, hook.replacement, hook.original);
            if (result != MH_OK) {
                LOG_ERROR("event=hooks.create.failed api=%s status=%d", hook.name, result);
                return false;
            }
        }

        Real_eventSelect = Hooks::Real_WSAEventSelect;
        Real_asyncSelect = Hooks::Real_WSAAsyncSelect;

        Hooks::Real_WSAEventSelect = Async_eventSelect;
        Hooks::Real_WSAAsyncSelect = Async_asyncSelect;

        return true;
    }

}
