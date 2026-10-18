#include "pch.h"
#include "src/dllmain.hpp"
#include "src/hook/process.hpp"
#include "src/hook/hooks.hpp"
#include "src/core/logger.hpp"

namespace droute {

    std::wstring g_ourDir;

    namespace Hooks {
        BOOL(WINAPI* Real_CreateProcessW)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                          LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) = CreateProcessW;
    }

    static bool IsDiscordApp(const std::wstring& name) {
        return _wcsicmp(name.c_str(), L"Discord.exe") == 0 || _wcsicmp(name.c_str(), L"DiscordCanary.exe") == 0 ||
               _wcsicmp(name.c_str(), L"DiscordPTB.exe") == 0;
    }

    static void DeployDlls(const std::wstring& targetDir) {
        std::wstring proxySrc = g_ourDir + L"\\version.dll";
        std::wstring proxyDst = targetDir + L"\\version.dll";
        std::wstring payloadSrc = g_ourDir + L"\\droute.dll";
        std::wstring payloadDst = targetDir + L"\\droute.dll";

        if (GetFileAttributesW(proxyDst.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (GetFileAttributesW(proxySrc.c_str()) != INVALID_FILE_ATTRIBUTES) {
                if (CopyFileW(proxySrc.c_str(), proxyDst.c_str(), FALSE)) {
                    LOG_INFO("deployed version.dll -> %S", proxyDst.c_str());
                } else {
                    LOG_WARN("copy version.dll failed: %u", GetLastError());
                }
            }
        }

        if (GetFileAttributesW(payloadDst.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (GetFileAttributesW(payloadSrc.c_str()) != INVALID_FILE_ATTRIBUTES) {
                if (CopyFileW(payloadSrc.c_str(), payloadDst.c_str(), FALSE)) {
                    LOG_INFO("deployed droute.dll -> %S", payloadDst.c_str());
                } else {
                    LOG_WARN("copy droute.dll failed: %u", GetLastError());
                }
            }
        }
    }

}

BOOL WINAPI Mine_CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                                LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
                                LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
                                LPPROCESS_INFORMATION lpProcessInformation) {
    if (!droute::IsInitializingRuntime())
        droute::EnsureRuntime();
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
        if (exePath.find(L'\\') == std::wstring::npos && exePath.find(L'/') == std::wstring::npos) {
            std::wstring dir;
            if (lpCurrentDirectory && lpCurrentDirectory[0]) {
                dir = lpCurrentDirectory;
            } else {
                wchar_t cwd[MAX_PATH];
                if (GetCurrentDirectoryW(MAX_PATH, cwd))
                    dir = cwd;
            }
            if (!dir.empty())
                exePath = dir + L"\\" + exePath;
        }

        size_t sep = exePath.rfind(L'\\');
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
                    droute::DeployDlls(targetDir);
                }
            }
        }
    }

    return droute::Hooks::Real_CreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
                                              bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory,
                                              lpStartupInfo, lpProcessInformation);
}
