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

    void ReconnectWorker() {
        while (WaitForSingleObject(g_stopEvent, g_cfg.reconnectInterval) == WAIT_TIMEOUT) {

            std::vector<SOCKET> pending;
            {
                std::unique_lock<std::shared_mutex> lock(g_stateMutex);
                for (auto& entry : g_udpMap) {
                    UdpAssociation& association = entry.second;
                    int error = 0;
                    const char* reason = "connected";
                    if (association.status != UdpAssociation::Status::Associated ||
                        !IsSocketDisconnected(association.ctrlSocket, &error, &reason))
                        continue;

                    LOG_WARN("event=udp.associate.pending flow=%llu socket=%llu generation=%llu trigger=worker "
                             "reason=%s wsa_error=%d control=%llu relay=%s",
                             association.flowId, (ULONG_PTR)entry.first, association.generation, reason, error,
                             (ULONG_PTR)association.ctrlSocket, FormatAddress(association.relayAddr).c_str());
                    if (association.ctrlSocket != INVALID_SOCKET)
                        Hooks::Real_closesocket(association.ctrlSocket);
                    association.ctrlSocket = INVALID_SOCKET;
                    association.relayAddr = {};
                    association.status = UdpAssociation::Status::PendingAssociate;
                    g_pendingUdp.insert(entry.first);
                }
                const uint64_t now = GetTickCount64();
                for (auto& entry : g_udpMap) {
                    auto& a = entry.second;
                    if (a.status != UdpAssociation::Status::Idle && now - a.lastSummaryAt >= 10000)
                        LogUdpSummary(entry.first, a, "udp.summary");
                }
                pending.assign(g_pendingUdp.begin(), g_pendingUdp.end());
            }

            for (SOCKET s : pending) {
                UdpAssociation temp;
                uint64_t instanceId = 0;

                {
                    std::unique_lock<std::shared_mutex> lock(g_stateMutex);
                    auto it = g_udpMap.find(s);
                    if (it == g_udpMap.end() || it->second.status != UdpAssociation::Status::PendingAssociate)
                        continue;
                    LogUdpAssociationStart(s, it->second, "worker");
                    instanceId = it->second.instanceId;
                    it->second.status = UdpAssociation::Status::Associating;
                }

                bool ok = TryUdpAssociate(temp);
                const int error = ok ? 0 : WSAGetLastError();

                std::unique_lock<std::shared_mutex> lock(g_stateMutex);
                auto it = g_udpMap.find(s);
                if (it == g_udpMap.end() || it->second.instanceId != instanceId ||
                    it->second.status != UdpAssociation::Status::Associating) {
                    if (temp.ctrlSocket != INVALID_SOCKET) {
                        Hooks::Real_closesocket(temp.ctrlSocket);
                    }
                    continue;
                }

                if (ok) {
                    it->second.ctrlSocket = temp.ctrlSocket;
                    it->second.relayAddr = temp.relayAddr;
                    it->second.status = UdpAssociation::Status::Associated;
                    g_pendingUdp.erase(s);
                } else {
                    it->second.status = UdpAssociation::Status::PendingAssociate;
                }
                LogUdpAssociationResult(s, it->second, ok, error, temp.associationStage);
                g_udpAssociationChanged.notify_all();
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
