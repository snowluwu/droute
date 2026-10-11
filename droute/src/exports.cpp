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
                    if (association.status != UdpAssociation::Status::Associated ||
                        !IsSocketDisconnected(association.ctrlSocket))
                        continue;

                    if (association.ctrlSocket != INVALID_SOCKET)
                        Hooks::Real_closesocket(association.ctrlSocket);
                    association.ctrlSocket = INVALID_SOCKET;
                    association.relayAddr = {};
                    association.status = UdpAssociation::Status::PendingAssociate;
                    g_pendingUdp.insert(entry.first);
                    LOG_WARN("udp %llu control connection lost, scheduling reassociation", (ULONG_PTR)entry.first);
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
                    instanceId = it->second.instanceId;
                    it->second.status = UdpAssociation::Status::Associating;
                }

                bool ok = TryUdpAssociate(temp);

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
                    LOG_INFO("udp %llu reassociated", (ULONG_PTR)s);
                } else {
                    it->second.status = UdpAssociation::Status::PendingAssociate;
                    LOG_TRACE("udp %llu retry failed", (ULONG_PTR)s);
                }
                g_udpAssociationChanged.notify_all();
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
