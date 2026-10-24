#include "pch.h"
#include "src/core/logger.hpp"
#include <cstdarg>
#include "version.h"

namespace droute {
    static constexpr DWORD FILE_LOCK_TIMEOUT_MS = 50;
    static std::wstring g_logDirectory;
    static uint64_t g_sessionId = 0;
    static std::atomic<bool> g_ready{false};
    static std::atomic<LogLevel> g_level{LogLevel::Info};

    static void ReportLogError(const wchar_t* operation, const std::wstring& path, DWORD error) {
        wchar_t message[1024];
        _snwprintf_s(message, _countof(message), _TRUNCATE, L"droute: failed to %s '%s': %lu\n", operation,
                     path.c_str(), error);
        OutputDebugStringW(message);
    }

    static bool CreateLogDirectory(const std::wstring& path) {
        if (CreateDirectoryW(path.c_str(), nullptr))
            return true;
        const DWORD error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS) {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
                return true;
        }
        ReportLogError(L"create log directory", path, error);
        return false;
    }

    static bool PrepareLogDirectory() {
        const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (!size) {
            ReportLogError(L"read environment variable", L"LOCALAPPDATA", GetLastError());
            return false;
        }
        std::wstring path(size, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", &path[0], size);
        if (!length || length >= size) {
            ReportLogError(L"read environment variable", L"LOCALAPPDATA", GetLastError());
            return false;
        }
        path.resize(length);
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
            path.pop_back();

        std::wstring exePath(32768, L'\0');
        const DWORD exeLength = GetModuleFileNameW(nullptr, &exePath[0], static_cast<DWORD>(exePath.size()));
        const wchar_t* branch = L"unknown";
        if (exeLength && exeLength < exePath.size()) {
            exePath.resize(exeLength);
            const size_t separator = exePath.find_last_of(L"\\/");
            const wchar_t* name = exePath.c_str() + (separator == std::wstring::npos ? 0 : separator + 1);
            if (_wcsicmp(name, L"Discord.exe") == 0)
                branch = L"stable";
            else if (_wcsicmp(name, L"DiscordCanary.exe") == 0)
                branch = L"canary";
            else if (_wcsicmp(name, L"DiscordPTB.exe") == 0)
                branch = L"ptb";
        }

        path += L"\\droute";
        if (!CreateLogDirectory(path))
            return false;
        path += L"\\logs";
        if (!CreateLogDirectory(path))
            return false;
        path += L"\\";
        path += branch;
        if (!CreateLogDirectory(path))
            return false;
        g_logDirectory = std::move(path);
        return true;
    }

    static HANDLE OpenDailyLog(const SYSTEMTIME& st) {
        if (g_logDirectory.empty() && !PrepareLogDirectory())
            return INVALID_HANDLE_VALUE;

        wchar_t filename[64];
        swprintf_s(filename, L"\\droute-%04u-%02u-%02u.log", st.wYear, st.wMonth, st.wDay);
        const std::wstring path = g_logDirectory + filename;
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA | GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            ReportLogError(L"open log file", path, GetLastError());
        return file;
    }

    static void AppendLogRecord(HANDLE file, const char* record, DWORD length) {
        OVERLAPPED lock = {};
        lock.Offset = MAXDWORD - 1;
        lock.OffsetHigh = 0x7FFFFFFF;
        const uint64_t deadline = GetTickCount64() + FILE_LOCK_TIMEOUT_MS;
        for (;;) {
            if (LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &lock))
                break;
            const DWORD error = GetLastError();
            if (error != ERROR_LOCK_VIOLATION || GetTickCount64() >= deadline) {
                ReportLogError(L"lock log file", g_logDirectory, error == ERROR_LOCK_VIOLATION ? ERROR_TIMEOUT : error);
                OutputDebugStringA(record);
                return;
            }
            Sleep(1);
        }
        DWORD written = 0;
        const BOOL ok = WriteFile(file, record, length, &written, nullptr);
        const DWORD error = ok ? ERROR_WRITE_FAULT : GetLastError();
        if (!UnlockFileEx(file, 0, 1, 0, &lock))
            ReportLogError(L"unlock log file", g_logDirectory, GetLastError());
        if (!ok || written != length) {
            ReportLogError(L"write log file", g_logDirectory, error);
            OutputDebugStringA(record);
        }
    }

    const char* LevelToString(LogLevel level) {
        switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        }
        return "?????";
    }


    void Logger::Init() {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            g_sessionId = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        char process[MAX_PATH]{}, library[MAX_PATH]{}, proxy[MAX_PATH]{};
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Init), &module);
        GetModuleFileNameA(nullptr, process, sizeof(process));
        GetModuleFileNameA(module, library, sizeof(library));
        GetModuleFileNameA(GetModuleHandleW(L"weave_proxy.dll"), proxy, sizeof(proxy));
        char message[2000];
        _snprintf_s(message, sizeof(message), _TRUNCATE,
                    "event=session.start droute=" DROUTE_VERSION " weave=%s process=%s droute_path=%s weave_path=%s",
                    weave::GetVersion(), process, library, proxy);
        const weave::LogRecord record{sizeof(weave::LogRecord), WEAVE_LOG_INFO, GetCurrentProcessId(), GetCurrentThreadId(),
            (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime, "droute", "", 0, message};
        Receive(&record, nullptr);
    }

    void Logger::Ready() { g_ready.store(true); }
    void Logger::SetLevel(LogLevel level) { g_level.store(level); }
    bool Logger::Enabled(LogLevel level) { return level >= g_level.load(); }

    void __cdecl Logger::Receive(const weave::LogRecord* record, void*) {
        const FILETIME utc{static_cast<DWORD>(record->timestamp_utc_100ns),
                           static_cast<DWORD>(record->timestamp_utc_100ns >> 32)};
        FILETIME local{};
        SYSTEMTIME time{};
        FileTimeToLocalFileTime(&utc, &local);
        FileTimeToSystemTime(&local, &time);
        char text[3500];
        char location[300]{};
        if (*record->file)
            _snprintf_s(location, sizeof(location), _TRUNCATE, "[%s:%u] ", record->file, record->line);
        const char* source = strcmp(record->source, "host") == 0 ? "droute" : record->source;
        const int length = _snprintf_s(text, sizeof(text), _TRUNCATE,
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [PID:%u] [TID:%u] [SESSION:%llX] [UP:%llu] [%s] [%s] %s%s\n",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
            record->process_id, record->thread_id, g_sessionId,
            g_sessionId && record->timestamp_utc_100ns >= g_sessionId ? (record->timestamp_utc_100ns - g_sessionId) / 10000 : 0,
            LevelToString(static_cast<LogLevel>(record->level)), source, location, record->message);
        HANDLE file = OpenDailyLog(time);
        if (file != INVALID_HANDLE_VALUE) {
            AppendLogRecord(file, text, length < 0 ? static_cast<DWORD>(strlen(text)) : length);
            CloseHandle(file);
        } else {
            OutputDebugStringA(text);
        }
    }

    void Logger::Write(LogLevel level, const char* file, int line, const char* fmt, ...) {
        ErrorScope preserve;
        const char* separator = strrchr(file, '\\');
        if (!separator)
            separator = strrchr(file, '/');
        if (separator)
            file = separator + 1;
        char message[2500];
        va_list args;
        va_start(args, fmt);
        vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
        va_end(args);
        if (g_ready.load()) {
            weave::WriteLog(static_cast<weave::LogLevel>(level), message);
        } else {
            // startup messages precede the weave writer
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            const weave::LogRecord record{sizeof(weave::LogRecord), static_cast<uint32_t>(level),
                GetCurrentProcessId(), GetCurrentThreadId(),
                (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime,
                "droute", file, static_cast<uint32_t>(line), message};
            Receive(&record, nullptr);
        }
    }
}
