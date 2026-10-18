#pragma once

#include "src/hook/hooks.hpp"

namespace droute {

    int BeginTcpConnect(SOCKET socket, const sockaddr_in& destination);
    void ForgetAsyncSocket(SOCKET socket);
    bool InstallAsyncHooks();
    void GetSocketCompletionPort(SOCKET socket, HANDLE& port, ULONG_PTR& key);
    bool SetConnectingSocketMode(SOCKET socket, bool nonblocking, int& result);

}
