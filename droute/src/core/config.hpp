#pragma once

#include "pch.h"
#include "logger.hpp"

namespace droute {

    struct Config {
        std::string host = "127.0.0.1";
        uint16_t port = 1080;
        std::string user;
        std::string password;

        uint32_t connectTimeout = 5000;
        uint32_t reconnectInterval = 3000;
        LogLevel logLevel = LogLevel::Info;

        bool Load();
    };

    bool GetProxyAddress(sockaddr_in& address);
    bool IsResolvingProxy();

    extern Config g_cfg;
    extern sockaddr_in g_proxyAddr;

}
