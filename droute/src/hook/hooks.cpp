#include "pch.h"
#include "src/hook/hooks.hpp"
#include "src/core/logger.hpp"
#include "src/hook/tcp.hpp"
#include "src/hook/udp.hpp"
#include "src/hook/process.hpp"

namespace droute {

    std::shared_mutex g_stateMutex;
    std::condition_variable_any g_udpAssociationChanged;
    std::map<SOCKET, UdpAssociation> g_udpMap;
    std::set<SOCKET> g_nonBlockingSockets;
    std::set<SOCKET> g_pendingUdp;
    std::map<SOCKET, TcpConnection> g_tcpConnections;

    namespace Hooks {

        int (WSAAPI* Real_connect)(SOCKET s, const sockaddr* name, int namelen) = connect;
        int (WSAAPI* Real_bind)(SOCKET s, const sockaddr* addr, int namelen) = bind;
        int (WSAAPI* Real_closesocket)(SOCKET s) = closesocket;
        int (WSAAPI* Real_sendto)(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) = sendto;
        int (WSAAPI* Real_recvfrom)(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen) = recvfrom;
        int (WSAAPI* Real_WSASendTo)(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, const sockaddr* lpTo, int iTolen, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) = WSASendTo;
        int (WSAAPI* Real_WSARecvFrom)(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, sockaddr* lpFrom, LPINT lpFromlen, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) = WSARecvFrom;
        BOOL (WSAAPI* Real_WSAGetOverlappedResult)(SOCKET s, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags) = WSAGetOverlappedResult;
        int (WSAAPI* Real_WSAEventSelect)(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents) = WSAEventSelect;
        int (WSAAPI* Real_WSAAsyncSelect)(SOCKET s, HWND hWnd, unsigned int wMsg, long lEvent) = WSAAsyncSelect;
        int (WSAAPI* Real_ioctlsocket)(SOCKET s, long cmd, u_long* argp) = ioctlsocket;
        BOOL (WINAPI* Real_GetQueuedCompletionStatus)(HANDLE CompletionPort, LPDWORD lpNumberOfBytesTransferred, PULONG_PTR lpCompletionKey, LPOVERLAPPED* lpOverlapped, DWORD dwMilliseconds) = GetQueuedCompletionStatus;
        BOOL (WINAPI* Real_GetQueuedCompletionStatusEx)(HANDLE CompletionPort, LPOVERLAPPED_ENTRY lpCompletionPortEntries, ULONG ulCount, PULONG ulNumEntriesRemoved, DWORD dwMilliseconds, BOOL fAlertable) = GetQueuedCompletionStatusEx;
        BOOL (WINAPI* Real_GetOverlappedResult)(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait) = GetOverlappedResult;
        BOOL (WINAPI* Real_GetOverlappedResultEx)(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, DWORD dwMilliseconds, BOOL bAlertable) = GetOverlappedResultEx;

        bool Install() {
            MH_STATUS status = MH_Initialize();
            if (status != MH_OK) {
                LOG_ERROR("event=hooks.initialize.failed status=%d description=%s", status, MH_StatusToString(status));
                return false;
            }

            auto HookOne = [&](const wchar_t* mod, const char* name, void* hook, void** orig) -> bool {
                const MH_STATUS result = MH_CreateHookApi(mod, name, hook, orig);
                if (result != MH_OK) {
                    LOG_ERROR("event=hooks.create.failed module=%S api=%s status=%d description=%s", mod, name, result, MH_StatusToString(result));
                    return false;
                }
                LOG_DEBUG("hooked %s", name);
                return true;
            };

            if (!HookOne(L"ws2_32.dll", "connect", Mine_connect, (void**)&Real_connect)) return false;
            if (!HookOne(L"ws2_32.dll", "bind", Mine_bind, (void**)&Real_bind)) return false;
            if (!HookOne(L"ws2_32.dll", "closesocket", Mine_closesocket, (void**)&Real_closesocket)) return false;
            if (!HookOne(L"ws2_32.dll", "sendto", Mine_sendto, (void**)&Real_sendto)) return false;
            if (!HookOne(L"ws2_32.dll", "recvfrom", Mine_recvfrom, (void**)&Real_recvfrom)) return false;
            if (!HookOne(L"ws2_32.dll", "WSASendTo", Mine_WSASendTo, (void**)&Real_WSASendTo)) return false;
            if (!HookOne(L"ws2_32.dll", "WSARecvFrom", Mine_WSARecvFrom, (void**)&Real_WSARecvFrom)) return false;
            if (!HookOne(L"ws2_32.dll", "WSAGetOverlappedResult", Mine_WSAGetOverlappedResult, (void**)&Real_WSAGetOverlappedResult)) return false;
            if (!HookOne(L"ws2_32.dll", "WSAEventSelect", Mine_WSAEventSelect, (void**)&Real_WSAEventSelect)) return false;
            if (!HookOne(L"ws2_32.dll", "WSAAsyncSelect", Mine_WSAAsyncSelect, (void**)&Real_WSAAsyncSelect)) return false;
            if (!HookOne(L"ws2_32.dll", "ioctlsocket", Mine_ioctlsocket, (void**)&Real_ioctlsocket)) return false;

            if (!HookOne(L"kernel32.dll", "GetQueuedCompletionStatus", Mine_GetQueuedCompletionStatus, (void**)&Real_GetQueuedCompletionStatus)) return false;
            if (!HookOne(L"kernel32.dll", "GetQueuedCompletionStatusEx", Mine_GetQueuedCompletionStatusEx, (void**)&Real_GetQueuedCompletionStatusEx)) return false;
            if (!HookOne(L"kernel32.dll", "GetOverlappedResult", Mine_GetOverlappedResult, (void**)&Real_GetOverlappedResult)) return false;
            if (!HookOne(L"kernel32.dll", "GetOverlappedResultEx", Mine_GetOverlappedResultEx, (void**)&Real_GetOverlappedResultEx)) return false;

            if (!HookOne(L"kernel32.dll", "CreateProcessW", Mine_CreateProcessW, (void**)&Real_CreateProcessW)) return false;

            status = MH_EnableHook(MH_ALL_HOOKS);
            if (status != MH_OK) {
                LOG_ERROR("event=hooks.enable.failed status=%d description=%s", status, MH_StatusToString(status));
                return false;
            }

            LOG_INFO("hooks installed");
            return true;
        }

        void Remove() {
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            LOG_TRACE("hooks removed");
        }

    }

}
