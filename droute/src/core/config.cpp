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
            if (tmp >= 100 && tmp <= INT_MAX)
                connectTimeout = tmp;
            else
                LOG_WARN("invalid ConnectTimeout=%u, using default %u", tmp, connectTimeout);
        }
        if (ReadDword("ReconnectInterval", tmp)) {
            if (tmp >= 100 && tmp <= INT_MAX)
                reconnectInterval = tmp;
            else
                LOG_WARN("invalid ReconnectInterval=%u, using default %u", tmp, reconnectInterval);
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

        g_proxyAddr = {};
        g_proxyAddr.sin_family = AF_INET;
        g_proxyAddr.sin_port = htons(port);

        if (inet_pton(AF_INET, host.c_str(), &g_proxyAddr.sin_addr) != 1)
            g_proxyAddr.sin_addr.s_addr = INADDR_NONE;

        const char* authState = "set";
        if (user.empty() && password.empty()) {
            authState = "none";
        } else if (user.empty() || password.empty()) {
            authState = "incomplete";
            LOG_WARN("proxy credentials are incomplete; authentication is disabled");
        }

        LOG_INFO("config source=%s proxy=%s:%u auth=%s connect_timeout_ms=%u reconnect_interval_ms=%u log_level=%s",
                 hasRegistryConfig ? "registry" : "defaults", host.c_str(), port, authState, connectTimeout,
                 reconnectInterval, LevelToString(logLevel));

        return hasRegistryConfig;
    }

    static INIT_ONCE g_resolveOnce = INIT_ONCE_STATIC_INIT;
    static sockaddr_in g_resolvedProxy = {};
    static thread_local bool g_resolvingProxy = false;

    bool IsResolvingProxy() {
        return g_resolvingProxy;
    }

    static BOOL CALLBACK ResolveProxy(PINIT_ONCE, void*, void**) {
        g_resolvingProxy = true;

        struct ResolveScope {
            ~ResolveScope() {
                g_resolvingProxy = false;
            }
        } scope;

        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        int error = getaddrinfo(g_cfg.host.c_str(), nullptr, &hints, &result);
        if (error || !result) {
            LOG_ERROR("failed to resolve proxy host '%s': %d", g_cfg.host.c_str(), error);
            return FALSE;
        }
        g_resolvedProxy = g_proxyAddr;
        g_resolvedProxy.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
        freeaddrinfo(result);
        return TRUE;
    }

    bool GetProxyAddress(sockaddr_in& address) {
        if (g_proxyAddr.sin_addr.s_addr != INADDR_NONE) {
            address = g_proxyAddr;
            return true;
        }
        if (!InitOnceExecuteOnce(&g_resolveOnce, ResolveProxy, nullptr, nullptr)) {
            WSASetLastError(WSAHOST_NOT_FOUND);
            return false;
        }
        address = g_resolvedProxy;
        return true;
    }

}
