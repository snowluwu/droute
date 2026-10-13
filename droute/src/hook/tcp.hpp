#pragma once

#include "src/hook/hooks.hpp"

namespace droute {

    int ConnectToProxy(SOCKET s, uint64_t deadline);
    int Socks5ProxyConnect(SOCKET s, const sockaddr_in& target, uint64_t deadline);
    int ConnectViaProxy(SOCKET s, const sockaddr_in* target, uint64_t deadline);

}
