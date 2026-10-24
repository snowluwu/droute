#include "pch.h"
#include "src/core/config.hpp"

namespace droute {

    Config g_cfg;

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

        const char* authState = "set";
        if (user.empty() && password.empty()) {
            authState = "none";
        } else if (user.empty() || password.empty()) {
            authState = "incomplete";
            LOG_WARN("proxy credentials are incomplete; authentication is disabled");
            user.clear();
            password.clear();
        }

        LOG_INFO("config source=%s proxy=%s:%u auth=%s connect_timeout_ms=%u reconnect_interval_ms=%u log_level=%s",
                 hasRegistryConfig ? "registry" : "defaults", host.c_str(), port, authState, connectTimeout,
                 reconnectInterval, LevelToString(logLevel));

        return hasRegistryConfig;
    }

}
