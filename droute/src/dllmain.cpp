#include "pch.h"
#include "src/dllmain.hpp"
#include "src/core/config.hpp"
#include "src/core/logger.hpp"
#include "src/core/utils.hpp"
#include "src/hook/hooks.hpp"
#include "src/hook/udp.hpp"
#include "src/hook/process.hpp"

namespace droute {

    static HANDLE g_stopEvent = nullptr;
    static HANDLE g_worker = nullptr;
    static INIT_ONCE g_runtimeOnce = INIT_ONCE_STATIC_INIT;
    static thread_local bool g_initializing = false;

    bool IsInitializingRuntime() {
        return g_initializing;
    }

    static void ReconnectWorker() {
        while (WaitForSingleObject(g_stopEvent, g_cfg.reconnectInterval) == WAIT_TIMEOUT) {
            std::vector<UdpFlow> sockets;
            {
                std::shared_lock<std::shared_mutex> lock(g_stateMutex);
                for (const auto& entry : g_udpMap)
                    sockets.push_back({entry.first, entry.second.flowId});
            }
            for (const auto& flow : sockets) {
                SOCKET closedControl = INVALID_SOCKET;
                UdpAssociation summary;
                bool writeSummary = false;
                bool pending = false;
                int error = 0;
                const char* reason = "connected";
                {
                    std::lock_guard<std::shared_mutex> lock(g_stateMutex);
                    auto it = g_udpMap.find(flow.socket);
                    if (it == g_udpMap.end() || it->second.flowId != flow.flowId)
                        continue;
                    auto& association = it->second;
                    if (association.status == UdpAssociation::Status::Associated &&
                        IsSocketDisconnected(association.ctrlSocket, &error, &reason)) {
                        closedControl = association.ctrlSocket;
                        association.ctrlSocket = INVALID_SOCKET;
                        association.status = UdpAssociation::Status::PendingAssociate;
                    }
                    pending = association.status == UdpAssociation::Status::PendingAssociate;
                    const uint64_t now = GetTickCount64();
                    if (Logger::Enabled(LogLevel::Debug) && association.status != UdpAssociation::Status::Idle &&
                        now - association.lastSummaryAt >= 10000) {
                        summary = association;
                        association.reportedTx = association.txPackets;
                        association.reportedRx = association.rxPackets;
                        association.lastSummaryAt = now;
                        writeSummary = true;
                    }
                }
                if (closedControl != INVALID_SOCKET) {
                    Hooks::Real_closesocket(closedControl);
                    LOG_WARN("event=udp.associate.pending flow=%llu socket=%llu trigger=worker reason=%s error=%d",
                             flow.flowId, (ULONG_PTR)flow.socket, reason, error);
                }
                if (writeSummary)
                    LogUdpSummary(flow.socket, summary, "udp.summary");
                if (pending)
                    QueueUdpAssociation(flow.socket);
            }
        }
        LOG_TRACE("reconnect worker exiting");
    }

    static DWORD WINAPI WorkerMain(void*) {
        try {
            ReconnectWorker();
        } catch (...) {
            LOG_ERROR("UDP reconnect worker stopped unexpectedly");
        }
        return 0;
    }

    static BOOL CALLBACK InitializeRuntime(PINIT_ONCE, void*, void**) {
        g_initializing = true;

        struct InitializingScope {
            ~InitializingScope() {
                g_initializing = false;
            }
        } scope;

        const uint64_t startedAt = GetTickCount64();

        Logger::Init();
        g_cfg.Load();
        Logger::SetLevel(g_cfg.logLevel);

        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        LOG_INFO("loaded into process: %s", exePath);

        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(NULL, buf, MAX_PATH);
        g_ourDir = buf;
        size_t pos = g_ourDir.rfind(L'\\');
        if (pos != std::wstring::npos)
            g_ourDir = g_ourDir.substr(0, pos);

        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_stopEvent)
            g_worker = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
        if (!g_worker)
            LOG_ERROR("failed to start UDP reconnect worker: %lu", GetLastError());
        LOG_INFO("initialized elapsed_ms=%llu", GetTickCount64() - startedAt);
        return TRUE;
    }

    bool EnsureRuntime() {
        try {
            return InitOnceExecuteOnce(&g_runtimeOnce, InitializeRuntime, nullptr, nullptr) != FALSE;
        } catch (...) {
            OutputDebugStringA("droute: runtime initialization failed\n");
            return false;
        }
    }

    void StopRuntime() {
        if (g_stopEvent)
            SetEvent(g_stopEvent);
        if (g_worker && WaitForSingleObject(g_worker, 1000) == WAIT_OBJECT_0) {
            CloseHandle(g_worker);
            g_worker = nullptr;
            CloseHandle(g_stopEvent);
            g_stopEvent = nullptr;
        }
        Logger::Shutdown();
    }

}

extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        // install interception before the application can start networking
        if (!droute::Hooks::Install()) {
            OutputDebugStringA("droute: failed to install hooks\n");
            return FALSE;
        }
        // installed hooks remain valid until the process exits
        HMODULE pinned;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(hModule), &pinned)) {
            droute::Hooks::Remove();
            return FALSE;
        }
        break;
    }

    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
