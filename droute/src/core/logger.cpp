#include "pch.h"
#include "src/core/logger.hpp"

#include <cstdarg>

namespace droute {

    static HANDLE g_logFile = INVALID_HANDLE_VALUE;
    static std::mutex g_logMutex;
    static std::atomic<LogLevel> g_logLevel{ LogLevel::Info };
    static std::wstring g_logDirectory;
    static DWORD g_logDate = 0;
    static bool g_initialized = false;
    static uint64_t g_sessionId = 0;
    static uint64_t g_startedAt = 0;

    static void ReportLogError(const wchar_t* operation, const std::wstring& path, DWORD error) {
        wchar_t message[1024];
        _snwprintf_s(message, _countof(message), _TRUNCATE,
            L"droute: failed to %s '%s': %lu\n", operation, path.c_str(), error);
        OutputDebugStringW(message);
    }

    static bool CreateLogDirectory(const std::wstring& path) {
        if (CreateDirectoryW(path.c_str(), nullptr)) return true;
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
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();

        std::wstring exePath(32768, L'\0');
        const DWORD exeLength = GetModuleFileNameW(nullptr, &exePath[0], static_cast<DWORD>(exePath.size()));
        const wchar_t* branch = L"unknown";
        if (exeLength && exeLength < exePath.size()) {
            exePath.resize(exeLength);
            const size_t separator = exePath.find_last_of(L"\\/");
            const wchar_t* name = exePath.c_str() + (separator == std::wstring::npos ? 0 : separator + 1);
            if (_wcsicmp(name, L"Discord.exe") == 0) branch = L"stable";
            else if (_wcsicmp(name, L"DiscordCanary.exe") == 0) branch = L"canary";
            else if (_wcsicmp(name, L"DiscordPTB.exe") == 0) branch = L"ptb";
        }

        path += L"\\droute";
        if (!CreateLogDirectory(path)) return false;
        path += L"\\logs";
        if (!CreateLogDirectory(path)) return false;
        path += L"\\";
        path += branch;
        if (!CreateLogDirectory(path)) return false;
        g_logDirectory = std::move(path);
        return true;
    }

    // Called under g_logMutex. Each process independently switches to the same daily file.
    static bool OpenDailyLog(const SYSTEMTIME& st) {
        const DWORD date = st.wYear * 10000UL + st.wMonth * 100UL + st.wDay;
        if (g_logFile != INVALID_HANDLE_VALUE && g_logDate == date) return true;
        if (g_logDirectory.empty() && !PrepareLogDirectory()) return false;

        wchar_t filename[64];
        swprintf_s(filename, L"\\droute-%04u-%02u-%02u.log", st.wYear, st.wMonth, st.wDay);
        const std::wstring path = g_logDirectory + filename;
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA | GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            ReportLogError(L"open log file", path, GetLastError());
            return false;
        }
        if (g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
        g_logFile = file;
        g_logDate = date;
        return true;
    }

    static void AppendLogRecord(const char* record, DWORD length) {
        // A byte beyond EOF acts as an interprocess mutex without blocking log readers.
        // Windows releases the lock if the owning process closes the file or exits.
        OVERLAPPED lock = {};
        lock.Offset = MAXDWORD - 1;
        lock.OffsetHigh = 0x7FFFFFFF;
        if (!LockFileEx(g_logFile, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &lock)) {
            ReportLogError(L"lock log file", g_logDirectory, GetLastError());
            OutputDebugStringA(record);
            return;
        }
        DWORD written = 0;
        const BOOL ok = WriteFile(g_logFile, record, length, &written, nullptr);
        const DWORD error = ok ? ERROR_WRITE_FAULT : GetLastError();
        if (!UnlockFileEx(g_logFile, 0, 1, 0, &lock)) {
            ReportLogError(L"unlock log file", g_logDirectory, GetLastError());
            CloseHandle(g_logFile);
            g_logFile = INVALID_HANDLE_VALUE;
        }
        if (!ok || written != length) {
            ReportLogError(L"write log file", g_logDirectory, error);
            OutputDebugStringA(record);
        }
    }

    const char* LevelToString(LogLevel level) {
        switch (level) {
            case LogLevel::Trace: return "TRACE";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO";
            case LogLevel::Warn:  return "WARN";
            case LogLevel::Error: return "ERROR";
        }
        return "?????";
    }

    bool Logger::Init() {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_initialized = true;
        g_startedAt = GetTickCount64();
        FILETIME created = {}, exited = {}, kernel = {}, user = {};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            g_sessionId = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        else
            g_sessionId = g_startedAt;
        SYSTEMTIME st;
        GetLocalTime(&st);
        if (!OpenDailyLog(st)) return false;
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        TIME_ZONE_INFORMATION timezone = {};
        const DWORD zone = GetTimeZoneInformation(&timezone);
        const LONG offset = -(timezone.Bias + (zone == TIME_ZONE_ID_DAYLIGHT
            ? timezone.DaylightBias : timezone.StandardBias));
        char header[1024];
        int headerLen = snprintf(header, sizeof(header),
            "// droute session started at %04d-%02d-%02d %02d:%02d:%02d.%03d; pid=%lu; session=%llX; process=%s; build=" __DATE__ " " __TIME__ "; utc_offset_minutes=%ld\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentProcessId(), g_sessionId, exePath, offset);

        if (headerLen > 0) {
            DWORD length = static_cast<DWORD>(min(headerLen, static_cast<int>(sizeof(header) - 1)));
            AppendLogRecord(header, length);
        }

        return true;
    }

    void Logger::Shutdown() {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_initialized = false;
        if (g_logFile != INVALID_HANDLE_VALUE) {
            CloseHandle(g_logFile);
            g_logFile = INVALID_HANDLE_VALUE;
        }
        g_logDate = 0;
        g_logDirectory.clear();
    }

    void Logger::SetLevel(LogLevel level) {
        g_logLevel.store(level, std::memory_order_relaxed);
    }

    bool Logger::Enabled(LogLevel level) {
        return level >= g_logLevel.load(std::memory_order_relaxed);
    }

    void Logger::Write(LogLevel level, const char* file, int line, const char* fmt, ...) {
        if (level < g_logLevel.load(std::memory_order_relaxed)) return;

        ErrorScope errorScope;

        const char* basename = file;
        const char* p = strrchr(file, '\\');
        if (p) basename = p + 1;
        p = strrchr(file, '/');
        if (p && p + 1 > basename) basename = p + 1;

        va_list args;
        va_start(args, fmt);

        char msg[2048];
        vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);
        va_end(args);

        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            SYSTEMTIME st;
            GetLocalTime(&st);
            char buf[2500];
            int bufLen = snprintf(buf, sizeof(buf),
                "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [PID:%lu] [TID:%lu] [SESSION:%llX] [UP:%llu] [%s] [%s:%d] %s\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                GetCurrentProcessId(),
                GetCurrentThreadId(),
                g_sessionId, GetTickCount64() - g_startedAt,
                LevelToString(level),
                basename, line,
                msg);

            if (g_initialized && OpenDailyLog(st)) {
                if (bufLen > 0) {
                    DWORD length = static_cast<DWORD>(min(bufLen, static_cast<int>(sizeof(buf) - 1)));
                    AppendLogRecord(buf, length);
                }
            } else {
                OutputDebugStringA(buf);
            }
        }

    }

}
