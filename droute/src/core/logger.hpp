#pragma once

#include "pch.h"

namespace droute {

    enum class LogLevel {
        Trace = 0,
        Debug = 1,
        Info  = 2,
        Warn  = 3,
        Error = 4
    };

    const char* LevelToString(LogLevel level);

    namespace Logger {

        bool Init();
        void Shutdown();
        void SetLevel(LogLevel level);
        bool Enabled(LogLevel level);

        // Observability must not change the error returned by an intercepted API.
        struct ErrorScope {
            DWORD error = GetLastError();
            ~ErrorScope() { SetLastError(error); }
        };

        void Write(LogLevel level, const char* file, int line, const char* fmt, ...);

    }

}

#define DROUTE_LOG(level, fmt, ...) do { if (droute::Logger::Enabled(level)) { droute::Logger::ErrorScope errorScope; droute::Logger::Write(level, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } } while(0)
#define LOG_TRACE(fmt, ...) DROUTE_LOG(droute::LogLevel::Trace, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) DROUTE_LOG(droute::LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  DROUTE_LOG(droute::LogLevel::Info, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  DROUTE_LOG(droute::LogLevel::Warn, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) DROUTE_LOG(droute::LogLevel::Error, fmt, ##__VA_ARGS__)
