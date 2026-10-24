#include "pch.h"
#include "src/hook/process.hpp"
#include "src/core/logger.hpp"

namespace droute {

    std::wstring g_ourDir;

    namespace Hooks {
        decltype(&CreateProcessW) Real_CreateProcessW = CreateProcessW;
    }

    static bool IsDiscordApp(const std::wstring& name) {
        return _wcsicmp(name.c_str(), L"Discord.exe") == 0 || _wcsicmp(name.c_str(), L"DiscordCanary.exe") == 0 ||
               _wcsicmp(name.c_str(), L"DiscordPTB.exe") == 0;
    }

    bool InstallProcessHook() {
        if (MH_Initialize() != MH_OK)
            return false;
        if (MH_CreateHookApi(L"kernel32.dll", "CreateProcessW", Mine_CreateProcessW,
                             reinterpret_cast<void**>(&Hooks::Real_CreateProcessW)) != MH_OK ||
            MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
            MH_Uninitialize();
            return false;
        }
        return true;
    }

    static bool DeployDlls(const std::wstring& targetDir) {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        const wchar_t* names[] = {L"weave_proxy.dll", L"droute.dll", L"version.dll"};
        std::wstring sources[3], targets[3], staged[3], backups[3];
        bool exists[3]{}, published[3]{};
        const std::wstring suffix = L".droute." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        bool complete = true;
        for (size_t i = 0; i < 3; ++i) {
            sources[i] = g_ourDir + L"\\" + names[i];
            targets[i] = targetDir + L"\\" + names[i];
            exists[i] = GetFileAttributesW(targets[i].c_str()) != INVALID_FILE_ATTRIBUTES;
            complete = complete && exists[i];
            staged[i] = targets[i] + suffix;
            backups[i] = staged[i] + L".bak";
        }
        if (complete)
            return true;
        DWORD error = ERROR_SUCCESS;
        for (size_t i = 0; i < 3; ++i) {
            if (!CopyFileW(sources[i].c_str(), staged[i].c_str(), FALSE) ||
                (exists[i] && !CopyFileW(targets[i].c_str(), backups[i].c_str(), FALSE))) {
                error = GetLastError();
                break;
            }
        }
        for (size_t i = 0; !error && i < 3; ++i) {
            if (!MoveFileExW(staged[i].c_str(), targets[i].c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                error = GetLastError();
            else
                published[i] = true;
        }
        if (error) {
            for (int i = 2; i >= 0; --i) {
                if (!published[i])
                    continue;
                const BOOL restored = exists[i]
                    ? MoveFileExW(backups[i].c_str(), targets[i].c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                    : DeleteFileW(targets[i].c_str());
                if (!restored)
                    LOG_ERROR("rollback failed path=%S backup=%S error=%lu", targets[i].c_str(), backups[i].c_str(), GetLastError());
            }
            LOG_ERROR("patch deployment failed target=%S error=%lu", targetDir.c_str(), error);
        } else {
            LOG_INFO("deployed patch to %S", targetDir.c_str());
        }
        for (size_t i = 0; i < 3; ++i) {
            DeleteFileW(staged[i].c_str());
            // keep backups when rollback failed
            if (!error || !published[i])
                DeleteFileW(backups[i].c_str());
        }
        SetLastError(error);
        return error == ERROR_SUCCESS;
    }

}

BOOL WINAPI Mine_CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
{
    // deploy before the new process loads version.dll
    std::wstring exePath;

    if (lpApplicationName && lpApplicationName[0]) {
        exePath = lpApplicationName;
    } else if (lpCommandLine && lpCommandLine[0]) {
        LPWSTR p = lpCommandLine;
        if (*p == L'"') {
            ++p;
            while (*p && *p != L'"')
                exePath += *p++;
        } else {
            while (*p && *p != L' ')
                exePath += *p++;
        }
    }

    if (!exePath.empty()) {
        if (exePath.find_first_of(L"\\/") == std::wstring::npos) {
            std::wstring dir;
            if (lpCurrentDirectory && lpCurrentDirectory[0]) {
                dir = lpCurrentDirectory;
            } else {
                const DWORD capacity = GetCurrentDirectoryW(0, nullptr);
                if (capacity) {
                    dir.resize(capacity);
                    const DWORD length = GetCurrentDirectoryW(capacity, &dir[0]);
                    if (length > 0 && length < capacity)
                        dir.resize(length);
                    else
                        dir.clear();
                }
            }
            if (!dir.empty())
                exePath = dir + L"\\" + exePath;
        }

        size_t sep = exePath.find_last_of(L"\\/");
        if (sep != std::wstring::npos) {
            std::wstring fileName = exePath.substr(sep + 1);
            std::wstring targetDir = exePath.substr(0, sep);

            if (droute::IsDiscordApp(fileName) && _wcsicmp(targetDir.c_str(), droute::g_ourDir.c_str()) != 0) {

                LOG_INFO("discord launch detected: %S", exePath.c_str());

                const size_t appSeparator = targetDir.find_last_of(L"\\/");
                const std::wstring appName = targetDir.substr(appSeparator + 1);
                const std::wstring branchRoot = targetDir.substr(0, appSeparator);
                const size_t branchSeparator = branchRoot.find_last_of(L"\\/");
                const std::wstring branchName = branchRoot.substr(branchSeparator + 1);
                if (appSeparator != std::wstring::npos && appName.compare(0, 4, L"app-") == 0 && appName.size() > 4 &&
                    _wcsicmp((branchName + L".exe").c_str(), fileName.c_str()) == 0) {
                    if (!droute::DeployDlls(targetDir))
                        return FALSE;
                }
            }
        }
    }

    return droute::Hooks::Real_CreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes,
        lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment,
        lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
}
