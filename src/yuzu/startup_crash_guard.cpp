// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/startup_crash_guard.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace {

static wchar_t s_log_path[MAX_PATH] = {0};

static void WriteStartupLog(const char* msg) {
    if (s_log_path[0] == 0) return;
    HANDLE hFile = CreateFileW(s_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, msg, static_cast<DWORD>(strlen(msg)), &written, nullptr);
        CloseHandle(hFile);
    }
}

static LONG WINAPI EarlyCrashHandler(EXCEPTION_POINTERS* ExceptionInfo) {
    char buf[512];
    const DWORD code = ExceptionInfo->ExceptionRecord->ExceptionCode;
    const void* addr = ExceptionInfo->ExceptionRecord->ExceptionAddress;

    const char* reason = "Unknown Exception";
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        reason = "STATUS_ILLEGAL_INSTRUCTION (0xC000001D) - CPU instruction not supported!";
    } else if (code == EXCEPTION_ACCESS_VIOLATION) {
        reason = "STATUS_ACCESS_VIOLATION (0xC0000005)";
    } else if (code == 0xC00000FD) {
        reason = "STATUS_STACK_OVERFLOW (0xC00000FD)";
    }

    HMODULE hMod = nullptr;
    wchar_t mod_name[MAX_PATH] = L"Unknown";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &hMod)) {
        GetModuleFileNameW(hMod, mod_name, MAX_PATH);
    }

    snprintf(buf, sizeof(buf),
             "\r\n=======================================================\r\n"
             "[FATAL CRASH] %s\r\n"
             "Code: 0x%08lX\r\n"
             "Fault Address: %p\r\n"
             "Module: %ls\r\n"
             "=======================================================\r\n",
             reason, code, addr, mod_name);

    WriteStartupLog(buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // Anonymous namespace

namespace YuzuCrashGuard {

void InitializeEarlyCrashGuard() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    // Resolve %APPDATA%\eden\log\yngchii_startup.log
    wchar_t appdata[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        snwprintf(s_log_path, MAX_PATH, L"%ls\\eden\\log\\yngchii_startup.log", appdata);

        // Ensure directory exists
        wchar_t dir_path[MAX_PATH] = {0};
        snwprintf(dir_path, MAX_PATH, L"%ls\\eden\\log", appdata);
        CreateDirectoryW(dir_path, nullptr);

        // Overwrite on new start
        HANDLE hFile = CreateFileW(s_log_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
        }
    }

    SetUnhandledExceptionFilter(EarlyCrashHandler);

    // Hardware query using pure CPUID and Win32
    int info[4];
    __cpuid(info, static_cast<int>(0x80000000));
    char brand[49] = {0};
    if (static_cast<unsigned>(info[0]) >= 0x80000004) {
        __cpuid(reinterpret_cast<int*>(brand + 0), static_cast<int>(0x80000002));
        __cpuid(reinterpret_cast<int*>(brand + 16), static_cast<int>(0x80000003));
        __cpuid(reinterpret_cast<int*>(brand + 32), static_cast<int>(0x80000004));
    }

    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);

    const bool avx2 = IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != 0;
    const bool avx512 = IsProcessorFeaturePresent(PF_AVX512F_INSTRUCTIONS_AVAILABLE) != 0;

    char header[512];
    snprintf(header, sizeof(header),
             "=======================================================\r\n"
             " Yngchii-X3D v4.1 - Early Startup & Diagnostic Log\r\n"
             "=======================================================\r\n"
             "CPU: %s\r\n"
             "Architecture: x86_64\r\n"
             "Processors: %lu logical threads\r\n"
             "AVX2 Hardware Available: %s\r\n"
             "AVX-512 Hardware Available: %s\r\n"
             "Crash Guard: Active (SetUnhandledExceptionFilter installed)\r\n"
             "=======================================================\r\n\r\n",
             brand[0] ? brand : "Unknown CPU",
             sys_info.dwNumberOfProcessors,
             avx2 ? "YES" : "NO",
             avx512 ? "YES" : "NO");

    WriteStartupLog(header);
}

} // namespace YuzuCrashGuard

#else // !_WIN32

namespace YuzuCrashGuard {
void InitializeEarlyCrashGuard() {}
}

#endif
