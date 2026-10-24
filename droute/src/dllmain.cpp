#include "pch.h"
#include "src/core/config.hpp"
#include "src/core/logger.hpp"
#include "src/hook/process.hpp"
#include <weave.h>

namespace droute {
    static BYTE* g_entry = nullptr;
    static BYTE g_entryBytes[14]{};

    static bool ReplaceEntry(const BYTE* bytes) {
        DWORD protection;
        if (!VirtualProtect(g_entry, sizeof(g_entryBytes), PAGE_EXECUTE_READWRITE, &protection))
            return false;
        memcpy(g_entry, bytes, sizeof(g_entryBytes));
        DWORD ignored;
        const BOOL restored = VirtualProtect(g_entry, sizeof(g_entryBytes), protection, &ignored);
        return FlushInstructionCache(GetCurrentProcess(), g_entry, sizeof(g_entryBytes)) && restored;
    }

    static bool Start() {
        HMODULE pinned;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&Start), &pinned))
            return false;
        Logger::Init();
        g_cfg.Load();
        Logger::SetLevel(g_cfg.logLevel);
        weave::Options options{};
        options.size = sizeof(options);
        options.api_version = WEAVE_API_VERSION;
        options.host = g_cfg.host.c_str();
        options.port = g_cfg.port;
        options.username = g_cfg.user.c_str();
        options.password = g_cfg.password.c_str();
        options.connect_timeout_ms = g_cfg.connectTimeout;
        options.reconnect_interval_ms = g_cfg.reconnectInterval;
        options.log_level = static_cast<uint32_t>(g_cfg.logLevel);
        options.log_callback = Logger::Receive;
        const auto configured = weave::Configure(options);
        if (configured != WEAVE_OK) {
            LOG_ERROR("weave configuration failed: result=%d", configured);
            return false;
        }
        const auto started = weave::Start();
        if (started != WEAVE_OK) {
            LOG_ERROR("weave startup failed: result=%d error=%lu", started, GetLastError());
            return false;
        }
        Logger::Ready();
        wchar_t path[32768];
        const DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
        if (!length || length >= _countof(path))
            return false;
        g_ourDir.assign(path, length);
        g_ourDir.resize(g_ourDir.find_last_of(L"\\/"));
        if (!InstallProcessHook()) {
            LOG_ERROR("failed to install process hook");
            weave::Stop();
            return false;
        }
        LOG_INFO("initialized process=%S weave=%s", path, weave::GetVersion());
        return true;
    }

    static int WINAPI Enter() {
        // restore the executable before entering its crt, outside the loader lock
        if (!ReplaceEntry(g_entryBytes))
            ExitProcess(ERROR_DLL_INIT_FAILED);
        try {
            if (!Start())
                ExitProcess(ERROR_DLL_INIT_FAILED);
        } catch (...) {
            OutputDebugStringA("droute: startup failed\n");
            ExitProcess(ERROR_DLL_INIT_FAILED);
        }
        return reinterpret_cast<int (WINAPI*)()>(g_entry)();
    }

    static bool ScheduleStart() {
        auto image = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
        const DWORD offset = nt->OptionalHeader.AddressOfEntryPoint;
        if (!offset || offset > nt->OptionalHeader.SizeOfImage - sizeof(g_entryBytes))
            return false;
        g_entry = image + offset;
        memcpy(g_entryBytes, g_entry, sizeof(g_entryBytes));
        BYTE jump[14]{0xFF, 0x25, 0, 0, 0, 0};
        const auto target = reinterpret_cast<uintptr_t>(&Enter);
        memcpy(jump + 6, &target, sizeof(target));
        return ReplaceEntry(jump);
    }
}

extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        // this integration is an executable import, not a loadlibrary plugin
        if (!reserved || !droute::ScheduleStart())
            return FALSE;
    }
    return TRUE;
}
