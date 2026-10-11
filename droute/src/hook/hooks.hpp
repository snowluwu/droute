#pragma once

#include "pch.h"

namespace droute {

    struct UdpAssociation {
        enum class Status { Idle, Associating, PendingAssociate, Associated };
        Status status = Status::Idle;
        SOCKET ctrlSocket = INVALID_SOCKET;
        sockaddr_in relayAddr = {};
        // Per-process flow identity survives reassociation and distinguishes reused handles.
        uint64_t flowId = 0;
        uint64_t createdAt = 0, attemptStartedAt = 0, lastSummaryAt = 0;
        uint64_t attempts = 0, generation = 0;
        uint64_t txPackets = 0, rxPackets = 0, txBytes = 0, rxBytes = 0;
        uint64_t reportedTx = 0, reportedRx = 0;
        uint64_t lastTxAt = 0, lastRxAt = 0;
        uint64_t firstTxGeneration = 0, firstRxGeneration = 0;
        uint64_t ioErrors = 0, wouldBlock = 0, blockedSends = 0, pendingIo = 0;
        uint64_t lastIoErrorLogAt = 0;
        int lastIoError = 0, lastAssociationError = 0;
        const char* associationStage = "none";
        sockaddr_in destination = {};
    };

    extern std::shared_mutex g_stateMutex;
    extern std::map<SOCKET, UdpAssociation> g_udpMap;
    extern std::set<SOCKET> g_nonBlockingSockets;
    extern std::set<SOCKET> g_pendingUdp;
    struct TcpConnection {
        uint64_t id = 0;
        uint64_t startedAt = 0;
        sockaddr_in destination = {};
    };
    extern std::map<SOCKET, TcpConnection> g_tcpConnections;

    namespace Hooks {

        extern int (WSAAPI* Real_connect)(SOCKET s, const sockaddr* name, int namelen);
        extern int (WSAAPI* Real_bind)(SOCKET s, const sockaddr* addr, int namelen);
        extern int (WSAAPI* Real_closesocket)(SOCKET s);
        extern int (WSAAPI* Real_sendto)(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen);
        extern int (WSAAPI* Real_recvfrom)(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen);
        extern int (WSAAPI* Real_WSASendTo)(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, const sockaddr* lpTo, int iTolen, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
        extern int (WSAAPI* Real_WSARecvFrom)(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, sockaddr* lpFrom, LPINT lpFromlen, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
        extern BOOL (WSAAPI* Real_WSAGetOverlappedResult)(SOCKET s, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags);
        extern int (WSAAPI* Real_WSAEventSelect)(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents);
        extern int (WSAAPI* Real_WSAAsyncSelect)(SOCKET s, HWND hWnd, unsigned int wMsg, long lEvent);
        extern int (WSAAPI* Real_ioctlsocket)(SOCKET s, long cmd, u_long* argp);
        extern BOOL (WINAPI* Real_GetQueuedCompletionStatus)(HANDLE CompletionPort, LPDWORD lpNumberOfBytesTransferred, PULONG_PTR lpCompletionKey, LPOVERLAPPED* lpOverlapped, DWORD dwMilliseconds);
        extern BOOL (WINAPI* Real_GetQueuedCompletionStatusEx)(HANDLE CompletionPort, LPOVERLAPPED_ENTRY lpCompletionPortEntries, ULONG ulCount, PULONG ulNumEntriesRemoved, DWORD dwMilliseconds, BOOL fAlertable);
        extern BOOL (WINAPI* Real_GetOverlappedResult)(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait);
        extern BOOL (WINAPI* Real_GetOverlappedResultEx)(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, DWORD dwMilliseconds, BOOL bAlertable);
        extern BOOL (WINAPI* Real_CreateProcessW)(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);

        bool Install();
        void Remove();

    }

    extern "C" {
        int WSAAPI Mine_connect(SOCKET s, const sockaddr* name, int namelen);
        int WSAAPI Mine_bind(SOCKET s, const sockaddr* addr, int namelen);
        int WSAAPI Mine_closesocket(SOCKET s);
        int WSAAPI Mine_sendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen);
        int WSAAPI Mine_recvfrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen);
        int WSAAPI Mine_WSASendTo(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, const sockaddr* lpTo, int iTolen, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
        int WSAAPI Mine_WSARecvFrom(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, sockaddr* lpFrom, LPINT lpFromlen, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
        BOOL WSAAPI Mine_WSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags);
        int WSAAPI Mine_WSAEventSelect(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents);
        int WSAAPI Mine_WSAAsyncSelect(SOCKET s, HWND hWnd, unsigned int wMsg, long lEvent);
        int WSAAPI Mine_ioctlsocket(SOCKET s, long cmd, u_long* argp);
        BOOL WINAPI Mine_GetQueuedCompletionStatus(HANDLE CompletionPort, LPDWORD lpNumberOfBytesTransferred, PULONG_PTR lpCompletionKey, LPOVERLAPPED* lpOverlapped, DWORD dwMilliseconds);
        BOOL WINAPI Mine_GetQueuedCompletionStatusEx(HANDLE CompletionPort, LPOVERLAPPED_ENTRY lpCompletionPortEntries, ULONG ulCount, PULONG ulNumEntriesRemoved, DWORD dwMilliseconds, BOOL fAlertable);
        BOOL WINAPI Mine_GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait);
        BOOL WINAPI Mine_GetOverlappedResultEx(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, DWORD dwMilliseconds, BOOL bAlertable);
        BOOL WINAPI Mine_CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);
    }

}
