#include "pch.h"
#include "src/core/logger.hpp"

#include <cstdarg>
#include <array>

namespace droute {

    static HANDLE g_logFile = INVALID_HANDLE_VALUE;
    static constexpr size_t QUEUE_CAPACITY = 1024;
    static constexpr size_t BATCH_CAPACITY = 32;
    static constexpr DWORD FILE_LOCK_TIMEOUT_MS = 50;

    struct LogRecord {
        SYSTEMTIME time = {};
        DWORD length = 0;
        char text[2500];
    };

    // fixed queue storage avoids destructor races during process exit.
    static std::array<LogRecord, QUEUE_CAPACITY> g_queue;
    static SRWLOCK g_queueLock = SRWLOCK_INIT;
    static size_t g_queueHead = 0, g_queueSize = 0;
    static HANDLE g_queueEvent = nullptr;
    static HANDLE g_writerThread = nullptr;
    static std::atomic<bool> g_accepting{false};
    static std::atomic<uint64_t> g_droppedBusy{0}, g_droppedFull{0};
    static std::atomic<LogLevel> g_logLevel{LogLevel::Info};
    static std::wstring g_logDirectory;
    static DWORD g_logDate = 0;
    static uint64_t g_sessionId = 0;
    static uint64_t g_startedAt = 0;

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

    // file state is owned by the writer thread.
    static bool OpenDailyLog(const SYSTEMTIME& st) {
        const DWORD date = st.wYear * 10000UL + st.wMonth * 100UL + st.wDay;
        if (g_logFile != INVALID_HANDLE_VALUE && g_logDate == date)
            return true;
        if (g_logDirectory.empty() && !PrepareLogDirectory())
            return false;

        wchar_t filename[64];
        swprintf_s(filename, L"\\droute-%04u-%02u-%02u.log", st.wYear, st.wMonth, st.wDay);
        const std::wstring path = g_logDirectory + filename;
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA | GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            ReportLogError(L"open log file", path, GetLastError());
            return false;
        }
        if (g_logFile != INVALID_HANDLE_VALUE)
            CloseHandle(g_logFile);
        g_logFile = file;
        g_logDate = date;
        return true;
    }

    static void AppendLogRecord(const char* record, DWORD length) {
        OVERLAPPED lock = {};
        lock.Offset = MAXDWORD - 1;
        lock.OffsetHigh = 0x7FFFFFFF;
        const uint64_t deadline = GetTickCount64() + FILE_LOCK_TIMEOUT_MS;
        for (;;) {
            if (LockFileEx(g_logFile, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &lock))
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

    static void FormatRecord(LogRecord& record, LogLevel level, const char* file, int line, const char* message) {
        GetLocalTime(&record.time);
        const SYSTEMTIME& st = record.time;
        const int length = snprintf(
            record.text, sizeof(record.text),
            "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [PID:%lu] [TID:%lu] [SESSION:%llX] [UP:%llu] [%s] [%s:%d] %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(),
            GetCurrentThreadId(), g_sessionId, GetTickCount64() - g_startedAt, LevelToString(level), file, line,
            message);
        record.length =
            length > 0 ? static_cast<DWORD>((std::min)(length, static_cast<int>(sizeof(record.text) - 1))) : 0;
    }

    static void Enqueue(const LogRecord& record) {
        if (!g_accepting.load(std::memory_order_acquire) || !record.length)
            return;
        if (!TryAcquireSRWLockExclusive(&g_queueLock)) {
            g_droppedBusy.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (g_accepting.load(std::memory_order_relaxed)) {
            if (g_queueSize == QUEUE_CAPACITY) {
                g_droppedFull.fetch_add(1, std::memory_order_relaxed);
            } else {
                LogRecord& slot = g_queue[(g_queueHead + g_queueSize) % QUEUE_CAPACITY];
                slot.time = record.time;
                slot.length = record.length;
                memcpy(slot.text, record.text, record.length + 1);
                ++g_queueSize;
                // signal under the lock to prevent Shutdown from closing the event first.
                SetEvent(g_queueEvent);
            }
        }
        ReleaseSRWLockExclusive(&g_queueLock);
    }

    static void Persist(const LogRecord* records, size_t count) {
        size_t first = 0;
        while (first < count) {
            const SYSTEMTIME& time = records[first].time;
            std::string bytes;
            bytes.reserve((count - first) * sizeof(records[first].text));
            size_t end = first;
            while (end < count && records[end].time.wYear == time.wYear && records[end].time.wMonth == time.wMonth &&
                   records[end].time.wDay == time.wDay) {
                bytes.append(records[end].text, records[end].length);
                ++end;
            }
            if (OpenDailyLog(time))
                AppendLogRecord(bytes.c_str(), static_cast<DWORD>(bytes.size()));
            else
                OutputDebugStringA(bytes.c_str());
            first = end;
        }
    }

    static DWORD WINAPI WriterMain(void*) {
        try {
            std::array<LogRecord, BATCH_CAPACITY> batch;
            for (;;) {
                AcquireSRWLockExclusive(&g_queueLock);
                const size_t count = (std::min)(g_queueSize, BATCH_CAPACITY);
                for (size_t i = 0; i < count; ++i) {
                    const LogRecord& slot = g_queue[(g_queueHead + i) % QUEUE_CAPACITY];
                    batch[i].time = slot.time;
                    batch[i].length = slot.length;
                    memcpy(batch[i].text, slot.text, slot.length + 1);
                }
                g_queueHead = (g_queueHead + count) % QUEUE_CAPACITY;
                g_queueSize -= count;
                const bool stopping = !g_accepting.load(std::memory_order_relaxed);
                ReleaseSRWLockExclusive(&g_queueLock);

                if (count)
                    Persist(batch.data(), count);
                const uint64_t busy = g_droppedBusy.exchange(0, std::memory_order_relaxed);
                const uint64_t full = g_droppedFull.exchange(0, std::memory_order_relaxed);
                if (busy || full) {
                    char message[256];
                    snprintf(message, sizeof(message),
                             "event=logger.dropped queue_busy=%llu queue_full=%llu capacity=%zu", busy, full,
                             QUEUE_CAPACITY);
                    LogRecord report;
                    FormatRecord(report, LogLevel::Warn, "logger.cpp", __LINE__, message);
                    Persist(&report, 1);
                }
                if (count)
                    continue;
                if (stopping)
                    break;
                const DWORD wait = WaitForSingleObject(g_queueEvent, 1000);
                if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
                    OutputDebugStringA("droute: logger event wait failed\n");
                    g_accepting.store(false, std::memory_order_release);
                    break;
                }
            }
        } catch (...) {
            g_accepting.store(false, std::memory_order_release);
            OutputDebugStringA("droute: log writer failed\n");
        }
        if (g_logFile != INVALID_HANDLE_VALUE) {
            CloseHandle(g_logFile);
            g_logFile = INVALID_HANDLE_VALUE;
        }
        g_logDirectory.clear();
        g_logDate = 0;
        return 0;
    }

    bool Logger::Init() {
        ErrorScope errorScope;
        if (g_startedAt)
            return g_accepting.load(std::memory_order_acquire);
        g_startedAt = GetTickCount64();
        FILETIME created = {}, exited = {}, kernel = {}, user = {};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            g_sessionId = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        else
            g_sessionId = g_startedAt;
        g_queueEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_queueEvent) {
            OutputDebugStringA("droute: failed to create logger event\n");
            return false;
        }

        LogRecord header;
        GetLocalTime(&header.time);
        const SYSTEMTIME& st = header.time;
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        TIME_ZONE_INFORMATION timezone = {};
        const DWORD zone = GetTimeZoneInformation(&timezone);
        const LONG offset =
            -(timezone.Bias + (zone == TIME_ZONE_ID_DAYLIGHT ? timezone.DaylightBias : timezone.StandardBias));
        const int length = snprintf(header.text, sizeof(header.text),
                                    "// droute session started at %04d-%02d-%02d %02d:%02d:%02d.%03d; pid=%lu; "
                                    "session=%llX; process=%s; build=" __DATE__ " " __TIME__
                                    "; utc_offset_minutes=%ld; log_writer=async; queue_capacity=%zu\n",
                                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                                    GetCurrentProcessId(), g_sessionId, exePath, offset, QUEUE_CAPACITY);
        header.length =
            length > 0 ? static_cast<DWORD>((std::min)(length, static_cast<int>(sizeof(header.text) - 1))) : 0;
        g_accepting.store(true, std::memory_order_release);
        Enqueue(header);
        // do not wait under DllMain: the writer starts after the loader lock is released.
        g_writerThread = CreateThread(nullptr, 0, WriterMain, nullptr, 0, nullptr);
        if (!g_writerThread) {
            g_accepting.store(false, std::memory_order_release);
            CloseHandle(g_queueEvent);
            g_queueEvent = nullptr;
            OutputDebugStringA("droute: failed to start log writer\n");
            return false;
        }
        return true;
    }

    void Logger::Shutdown() {
        ErrorScope errorScope;
        AcquireSRWLockExclusive(&g_queueLock);
        g_accepting.store(false, std::memory_order_release);
        if (g_queueEvent)
            SetEvent(g_queueEvent);
        ReleaseSRWLockExclusive(&g_queueLock);
        if (g_writerThread && WaitForSingleObject(g_writerThread, 1000) == WAIT_OBJECT_0) {
            CloseHandle(g_writerThread);
            g_writerThread = nullptr;
            AcquireSRWLockExclusive(&g_queueLock);
            CloseHandle(g_queueEvent);
            g_queueEvent = nullptr;
            ReleaseSRWLockExclusive(&g_queueLock);
        }
    }

    void Logger::SetLevel(LogLevel level) {
        g_logLevel.store(level, std::memory_order_relaxed);
    }

    bool Logger::Enabled(LogLevel level) {
        return level >= g_logLevel.load(std::memory_order_relaxed);
    }

    void Logger::Write(LogLevel level, const char* file, int line, const char* fmt, ...) {
        if (!Enabled(level) || !g_accepting.load(std::memory_order_acquire))
            return;
        ErrorScope errorScope;
        const char* basename = file;
        const char* separator = strrchr(file, '\\');
        if (separator)
            basename = separator + 1;
        separator = strrchr(file, '/');
        if (separator && separator + 1 > basename)
            basename = separator + 1;

        va_list args;
        va_start(args, fmt);
        char message[2048];
        vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
        va_end(args);
        LogRecord record;
        FormatRecord(record, level, basename, line, message);
        Enqueue(record);
    }
}
