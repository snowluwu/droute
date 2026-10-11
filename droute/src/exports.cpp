#include "pch.h"
#include "src/exports.hpp"
#include "src/core/config.hpp"
#include "src/core/logger.hpp"
#include "src/core/utils.hpp"
#include "src/hook/hooks.hpp"
#include "src/hook/udp.hpp"
#include "src/hook/process.hpp"

namespace droute {

    static std::atomic<bool> g_running{ false };

    void ReconnectWorker() {
        while (g_running.load(std::memory_order_relaxed)) {
            Sleep(g_cfg.reconnectInterval);

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

                    LOG_WARN("event=udp.associate.pending flow=%llu socket=%llu generation=%llu trigger=worker reason=%s wsa_error=%d control=%llu relay=%s",
                        association.flowId, (ULONG_PTR)entry.first, association.generation, reason, error,
                        (ULONG_PTR)association.ctrlSocket, AddrToString(association.relayAddr).c_str());
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

                {
                    std::unique_lock<std::shared_mutex> lock(g_stateMutex);
                    auto it = g_udpMap.find(s);
                    if (it == g_udpMap.end() || it->second.status != UdpAssociation::Status::PendingAssociate)
                        continue;
                    LogUdpAssociationStart(s, it->second, "worker");
                    it->second.status = UdpAssociation::Status::Associating;
                }

                bool ok = TryUdpAssociate(temp);
                const int error = ok ? 0 : WSAGetLastError();

                std::unique_lock<std::shared_mutex> lock(g_stateMutex);
                auto it = g_udpMap.find(s);
                if (it == g_udpMap.end()) {
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
            }
        }
        LOG_TRACE("reconnect worker exiting");
    }

    static LONG g_initDone = 0;

    void DoInit() {
        if (InterlockedCompareExchange(&g_initDone, 1, 0) != 0) {
            return;
        }

        const uint64_t startedAt = GetTickCount64();

        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            OutputDebugStringA("droute: WSAStartup failed\n");
            return;
        }

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

        if (!Hooks::Install()) {
            LOG_ERROR("hook installation failed");
            return;
        }

        g_running = true;
        try {
            std::thread(ReconnectWorker).detach();
        } catch (...) {
            g_running = false;
            LOG_ERROR("failed to start UDP reconnect worker");
        }
        LOG_INFO("initialized elapsed_ms=%llu", GetTickCount64() - startedAt);
    }

}

extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        droute::DoInit();
        break;

    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
