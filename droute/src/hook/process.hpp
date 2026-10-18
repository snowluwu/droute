#pragma once

#include "pch.h"

namespace droute {

    extern std::wstring g_ourDir;

    namespace Hooks {
        extern BOOL(WINAPI* Real_CreateProcessW)(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                                                 LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                                 LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles,
                                                 DWORD dwCreationFlags, LPVOID lpEnvironment,
                                                 LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
                                                 LPPROCESS_INFORMATION lpProcessInformation);
    }

}

extern "C" {
BOOL WINAPI Mine_CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                                LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
                                LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
                                LPPROCESS_INFORMATION lpProcessInformation);
}
