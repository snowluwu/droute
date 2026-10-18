#include "pch.h"
#include "src/core/config.hpp"
#include "src/core/utils.hpp"

namespace droute {

    Config g_cfg;
    sockaddr_in g_proxyAddr;

    bool Config::Load() {
        HKEY hKey = nullptr;
        LONG res = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\droute", 0, KEY_READ, &hKey);
        const bool hasRegistryConfig = res == ERROR_SUCCESS;
        if (!hasRegistryConfig)
            LOG_WARN("registry key not found (error=%ld), using defaults", res);

        auto ReadString = [&](const char* name, std::string& out) -> bool {
            if (!hasRegistryConfig)
                return false;

            DWORD type = 0;
            DWORD size = 0;
            if (RegQueryValueExA(hKey, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ ||
                size == 0)
                return false;

            std::vector<char> buf(size + 1, '\0');
            if (RegQueryValueExA(hKey, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &size) !=
                ERROR_SUCCESS)
                return false;

            out.assign(buf.data(), strnlen_s(buf.data(), size));
            return true;
        };

        auto ReadDword = [&](const char* name, uint32_t& out) -> bool {
            if (!hasRegistryConfig)
                return false;
            DWORD type = 0, val = 0, size = sizeof(val);
            if (RegQueryValueExA(hKey, name, nullptr, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS &&
                type == REG_DWORD) {
                out = val;
                return true;
            }
            return false;
        };

        ReadString("Host", host);
        if (host.empty()) {
            host = "127.0.0.1";
            LOG_WARN("proxy host is empty, using default %s", host.c_str());
        }
        uint32_t portTmp = port;
        if (ReadDword("Port", portTmp)) {
            if (portTmp > 0 && portTmp <= UINT16_MAX) {
                port = static_cast<uint16_t>(portTmp);
            } else {
                LOG_WARN("invalid proxy port %u, using default %u", portTmp, port);
            }
        }
        ReadString("User", user);
        ReadString("Password", password);

        uint32_t tmp;
        if (ReadDword("ConnectTimeout", tmp)) {
            if (tmp > 0 && tmp <= INT_MAX)
                connectTimeout = tmp;
            else
                LOG_WARN("invalid ConnectTimeout=%u, using default %u", tmp, connectTimeout);
        }
        if (ReadDword("ReconnectInterval", tmp)) {
            if (tmp > 0)
                reconnectInterval = tmp;
            else
                LOG_WARN("invalid ReconnectInterval=0, using default %u", reconnectInterval);
        }
        if (ReadDword("LogLevel", tmp)) {
            if (tmp <= static_cast<uint32_t>(LogLevel::Error)) {
                logLevel = static_cast<LogLevel>(tmp);
            } else {
                LOG_WARN("invalid LogLevel=%u, using default %s", tmp, LevelToString(logLevel));
            }
        }

        if (hKey)
            RegCloseKey(hKey);

        memset(&g_proxyAddr, 0, sizeof(g_proxyAddr));
        g_proxyAddr.sin_family = AF_INET;
        g_proxyAddr.sin_port = htons(static_cast<uint16_t>(port));

        if (inet_pton(AF_INET, host.c_str(), &g_proxyAddr.sin_addr) == 1) {
        } else {
            addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* result = nullptr;
            const int gaiError = getaddrinfo(host.c_str(), nullptr, &hints, &result);
            if (gaiError == 0 && result) {
                g_proxyAddr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
                freeaddrinfo(result);
            } else {
                LOG_ERROR("failed to resolve '%s': %d; using 127.0.0.1", host.c_str(), gaiError);
                inet_pton(AF_INET, "127.0.0.1", &g_proxyAddr.sin_addr);
            }
        }

        const char* authState =
            user.empty() && password.empty() ? "none" : (!user.empty() && !password.empty() ? "set" : "incomplete");
        if (strcmp(authState, "incomplete") == 0)
            LOG_WARN("proxy credentials are incomplete; authentication is disabled");

        LOG_INFO("config source=%s proxy=%s auth=%s connect_timeout_ms=%u reconnect_interval_ms=%u log_level=%s",
                 hasRegistryConfig ? "registry" : "defaults", FormatAddress(g_proxyAddr).c_str(), authState,
                 connectTimeout, reconnectInterval, LevelToString(logLevel));
        return hasRegistryConfig;
    }

}
