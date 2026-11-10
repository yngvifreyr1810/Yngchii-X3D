#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <intrin.h>

#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#endif
#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

typedef enum _MY_PROCESS_INFORMATION_CLASS {
    MyProcessPowerThrottling = 0x24
} MY_PROCESS_INFORMATION_CLASS;

typedef struct _MY_PROCESS_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} MY_PROCESS_POWER_THROTTLING_STATE;

typedef enum _MY_THREAD_INFORMATION_CLASS {
    MyThreadPowerThrottling = 0x10
} MY_THREAD_INFORMATION_CLASS;

typedef struct _MY_THREAD_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} MY_THREAD_POWER_THROTTLING_STATE;

typedef BOOL (WINAPI *pfnSetProcessInformation)(HANDLE, MY_PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
typedef BOOL (WINAPI *pfnSetThreadInformation)(HANDLE, MY_THREAD_INFORMATION_CLASS, LPVOID, DWORD);
typedef HRESULT (WINAPI *pfnGetThreadDescription)(HANDLE, PWSTR*);
typedef LONG (NTAPI *PFN_NtSetTimerResolution)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);

// ==============================================================================
// 1. HARDWARE CPU GATE: Zen 3 / Zen 4 / Zen 5 + Ryzen 5 / 7 / 9
// ==============================================================================
static int g_is_supported_ryzen = 0;
static char g_cpu_vendor[13] = {0};
static char g_cpu_brand[64] = {0};
static unsigned int g_cpu_display_family = 0;
static DWORD g_total_threads = 0;

static void DetectSupportedRyzenCpu(void) {
    int info[4] = {0};
    __cpuid(info, 0);
    memcpy(g_cpu_vendor + 0, &info[1], 4);
    memcpy(g_cpu_vendor + 4, &info[3], 4);
    memcpy(g_cpu_vendor + 8, &info[2], 4);
    g_cpu_vendor[12] = '\0';

    if (strcmp(g_cpu_vendor, "AuthenticAMD") != 0) {
        g_is_supported_ryzen = 0;
        return;
    }

    __cpuid(info, 1);
    unsigned int eax = (unsigned int)info[0];
    unsigned int base_family = (eax >> 8) & 0xF;
    unsigned int ext_family = (eax >> 20) & 0xFF;
    g_cpu_display_family = (base_family == 0xF) ? (base_family + ext_family) : base_family;

    if (g_cpu_display_family != 0x19 && g_cpu_display_family != 0x1A) {
        g_is_supported_ryzen = 0;
        return;
    }

    char brand[49] = {0};
    for (int i = 0; i < 3; i++) {
        __cpuid((int*)(brand + i * 16), 0x80000002 + i);
    }
    strncpy(g_cpu_brand, brand, sizeof(g_cpu_brand) - 1);

    char brand_lower[49] = {0};
    for (size_t i = 0; i < strlen(brand); i++) {
        brand_lower[i] = (char)tolower((unsigned char)brand[i]);
    }

    if (strstr(brand_lower, "ryzen 5") != NULL ||
        strstr(brand_lower, "ryzen 7") != NULL ||
        strstr(brand_lower, "ryzen 9") != NULL) {
        g_is_supported_ryzen = 1;
    } else {
        g_is_supported_ryzen = 0;
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_total_threads = si.dwNumberOfProcessors;
}

// ==============================================================================
// 2. LANGUAGE STATE & PREFERENCE STORAGE
// ==============================================================================
static volatile int g_lang_vn = 0; // 0 = English (Default), 1 = Vietnamese
static volatile int s_is_in_game = 0;
static char s_game_title[256] = {0};
static HINSTANCE g_hInst = NULL;

static int IsInGameWindow(HWND hMainWnd) {
    if (!hMainWnd || !IsWindow(hMainWnd)) return 0;
    wchar_t titleBuf[512] = {0};
    if (GetWindowTextW(hMainWnd, titleBuf, 512) <= 0) return 0;

    if (wcsstr(titleBuf, L"(64-bit)") != NULL ||
        wcsstr(titleBuf, L"(32-bit)") != NULL ||
        wcsstr(titleBuf, L"| Nvidia") != NULL ||
        wcsstr(titleBuf, L"| Vulkan") != NULL ||
        wcsstr(titleBuf, L"| AMD") != NULL ||
        wcsstr(titleBuf, L"| Intel") != NULL) {
        return 1;
    }

    int pipe_count = 0;
    for (int i = 0; titleBuf[i] != L'\0'; i++) {
        if (titleBuf[i] == L'|') pipe_count++;
    }
    if (pipe_count >= 3) return 1;

    return 0;
}

static int LoadLanguagePreference(void) {
    char appdata[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\eden\\config\\yngchii_lang.txt", appdata);
        FILE* f = fopen(path, "r");
        if (f) {
            char buf[16] = {0};
            if (fgets(buf, sizeof(buf), f)) {
                fclose(f);
                if (strstr(buf, "VN") != NULL) return 1;
            } else {
                fclose(f);
            }
        }
    }
    return 0; // Default: English (EN)
}

static void SaveLanguagePreference(int is_vn) {
    char appdata[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s\\eden\\config", appdata);
        CreateDirectoryA(dir, NULL);
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\eden\\config\\yngchii_lang.txt", appdata);
        FILE* f = fopen(path, "w");
        if (f) {
            fprintf(f, "%s\n", is_vn ? "VN" : "EN");
            fclose(f);
        }
    }
}

typedef struct {
    const char* en;
    const char* vn;
} TranslationEntry;

static const TranslationEntry s_dict[] = {
    {"\x50\x6C\x61\x79", "\x43\x68\xC6\xA1\x69\x20\x6E\x67\x61\x79"}, // Play
    {"\xE2\x96\xB6\x20\x50\x6C\x61\x79", "\xE2\x96\xB6\x20\x43\x68\xC6\xA1\x69\x20\x6E\x67\x61\x79"}, // ▶ Play
    {"\xE2\x96\xB6\x20\x20\x50\x6C\x61\x79", "\xE2\x96\xB6\x20\x20\x43\x68\xC6\xA1\x69\x20\x6E\x67\x61\x79"}, // ▶  Play
    {"\x50\x6C\x61\x79\x20\x6E\x6F\x77", "\x43\x68\xC6\xA1\x69\x20\x6E\x67\x61\x79"}, // Play now
    {"\x52\x65\x73\x75\x6D\x65", "\x54\x69\xE1\xBA\xBF\x70\x20\x74\xE1\xBB\xA5\x63"}, // Resume
    {"\xE2\x96\xB6\x20\x52\x65\x73\x75\x6D\x65", "\xE2\x96\xB6\x20\x54\x69\xE1\xBA\xBF\x70\x20\x74\xE1\xBB\xA5\x63"}, // ▶ Resume
    {"\xE2\x96\xB6\x20\x20\x52\x65\x73\x75\x6D\x65", "\xE2\x96\xB6\x20\x20\x54\x69\xE1\xBA\xBF\x70\x20\x74\xE1\xBB\xA5\x63"}, // ▶  Resume
    {"\x41\x6C\x6C\x20\x67\x61\x6D\x65\x73", "\x4B\x68\x6F\x20\x67\x61\x6D\x65"}, // All games
    {"\x41\x64\x64\x20\x66\x6F\x6C\x64\x65\x72", "\x54\x68\xC3\xAA\x6D\x20\x67\x61\x6D\x65"}, // Add folder
    {"\x50\x72\x65\x70\x61\x72\x69\x6E\x67\x20\x73\x68\x61\x64\x65\x72\x73\x2E\x2E\x2E", "\xC4\x90\x61\x6E\x67\x20\x63\x68\x75\xE1\xBA\xA9\x6E\x20\x62\xE1\xBB\x8B\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x53\x68\x61\x64\x65\x72\x20\x6C\xE1\xBA\xA7\x6E\x20\xC4\x91\xE1\xBA\xA7\x75\x20\x28\x63\x68\xE1\xBB\x89\x20\x64\x69\xE1\xBB\x85\x6E\x20\x72\x61\x20\x31\x20\x6C\xE1\xBA\xA7\x6E\x20\x64\x75\x79\x20\x6E\x68\xE1\xBA\xA5\x74\x2C\x20\x76\x75\x69\x20\x6C\xC3\xB2\x6E\x67\x20\xC4\x91\xE1\xBB\xA3\x69\x29\x2E\x2E\x2E"}, // Preparing shaders...
    {"\x50\x72\x65\x70\x61\x72\x69\x6E\x67\x20\x73\x68\x61\x64\x65\x72\x73", "\xC4\x90\x61\x6E\x67\x20\x63\x68\x75\xE1\xBA\xA9\x6E\x20\x62\xE1\xBB\x8B\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x53\x68\x61\x64\x65\x72\x20\x6C\xE1\xBA\xA7\x6E\x20\xC4\x91\xE1\xBA\xA7\x75\x20\x28\x63\x68\xE1\xBB\x89\x20\x64\x69\xE1\xBB\x85\x6E\x20\x72\x61\x20\x31\x20\x6C\xE1\xBA\xA7\x6E\x20\x64\x75\x79\x20\x6E\x68\xE1\xBA\xA5\x74\x2C\x20\x76\x75\x69\x20\x6C\xC3\xB2\x6E\x67\x20\xC4\x91\xE1\xBB\xA3\x69\x29\x2E\x2E\x2E"}, // Preparing shaders
    {"\x54\x68\x69\x73\x20\x68\x61\x70\x70\x65\x6E\x73\x20\x6F\x6E\x63\x65\x2C\x20\x73\x6F\x20\x25\x31\x20\x72\x75\x6E\x73\x20\x73\x6D\x6F\x6F\x74\x68\x6C\x79\x20\x66\x72\x6F\x6D\x20\x74\x68\x65\x20\x73\x74\x61\x72\x74\x2E", "\x51\x75\xC3\xA1\x20\x74\x72\xC3\xAC\x6E\x68\x20\x6E\xC3\xA0\x79\x20\x63\x68\xE1\xBB\x89\x20\x64\x69\xE1\xBB\x85\x6E\x20\x72\x61\x20\x31\x20\x6C\xE1\xBA\xA7\x6E\x20\x64\x75\x79\x20\x6E\x68\xE1\xBA\xA5\x74\x20\xC4\x91\xE1\xBB\x83\x20\x25\x31\x20\x63\x68\xE1\xBA\xA1\x79\x20\x6D\xC6\xB0\xE1\xBB\xA3\x74\x20\x6D\xC3\xA0\x20\x6E\x67\x61\x79\x20\x74\xE1\xBB\xAB\x20\xC4\x91\xE1\xBA\xA7\x75\x2E"}, // This happens once, so %1 runs smoothly from the start.
    {"\x42\x75\x69\x6C\x64\x69\x6E\x67\x20\x73\x68\x61\x64\x65\x72\x73\x2E\x2E\x2E", "\xC4\x90\x61\x6E\x67\x20\x64\xE1\xBB\xB1\x6E\x67\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x53\x68\x61\x64\x65\x72\x2E\x2E\x2E"}, // Building shaders...
    {"\x42\x75\x69\x6C\x64\x69\x6E\x67\x20\x73\x68\x61\x64\x65\x72\x73", "\xC4\x90\x61\x6E\x67\x20\x64\xE1\xBB\xB1\x6E\x67\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x53\x68\x61\x64\x65\x72"}, // Building shaders
    {"\x4C\x6F\x61\x64\x69\x6E\x67\x20\x73\x68\x61\x64\x65\x72\x73\x2E\x2E\x2E", "\xC4\x90\x61\x6E\x67\x20\x6E\xE1\xBA\xA1\x70\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x53\x68\x61\x64\x65\x72\x2E\x2E\x2E"}, // Loading shaders...
    {"\x4C\x6F\x61\x64\x69\x6E\x67\x2E\x2E\x2E", "\xC4\x90\x61\x6E\x67\x20\x74\xE1\xBA\xA3\x69\x2E\x2E\x2E"}, // Loading...
    {"\x4C\x61\x75\x6E\x63\x68\x69\x6E\x67\x2E\x2E\x2E", "\xC4\x90\x61\x6E\x67\x20\x6B\x68\xE1\xBB\x9F\x69\x20\x63\x68\xE1\xBA\xA1\x79\x2E\x2E\x2E"}, // Launching...
    {"\x47\x65\x74\x74\x69\x6E\x67\x20\x72\x65\x61\x64\x79", "\xC4\x90\x61\x6E\x67\x20\x63\x68\x75\xE1\xBA\xA9\x6E\x20\x62\xE1\xBB\x8B\x20\x73\xE1\xBA\xB5\x6E\x20\x73\xC3\xA0\x6E\x67"}, // Getting ready
    {"\x4C\x65\x73\x73\x20\x74\x68\x61\x6E\x20\x61\x20\x6D\x69\x6E\x75\x74\x65\x20\x6C\x65\x66\x74", "\x43\xC3\xB2\x6E\x20\x63\x68\xC6\xB0\x61\x20\xC4\x91\xE1\xBA\xA7\x79\x20\x6D\xE1\xBB\x99\x74\x20\x70\x68\xC3\xBA\x74"}, // Less than a minute left
    {"\x41\x62\x6F\x75\x74\x20\x25\x6E\x20\x6D\x69\x6E\x20\x6C\x65\x66\x74", "\x43\xC3\xB2\x6E\x20\x6B\x68\x6F\xE1\xBA\xA3\x6E\x67\x20\x25\x6E\x20\x70\x68\xC3\xBA\x74"}, // About %n min left
    {"\x41\x62\x6F\x75\x74\x20\x25\x31\x20\x68\x20\x25\x32\x20\x6D\x69\x6E\x20\x6C\x65\x66\x74", "\x43\xC3\xB2\x6E\x20\x6B\x68\x6F\xE1\xBA\xA3\x6E\x67\x20\x25\x31\x20\x67\x69\xE1\xBB\x9D\x20\x25\x32\x20\x70\x68\xC3\xBA\x74"}, // About %1 h %2 min left
    {"\x26\x41\x64\x64\x20\x4E\x65\x77\x20\x47\x61\x6D\x65\x20\x44\x69\x72\x65\x63\x74\x6F\x72\x79", "\x54\x68\xC3\xAA\x6D\x20\x74\x68\xC6\xB0\x20\x6D\xE1\xBB\xA5\x63\x20\x47\x61\x6D\x65\x20\x6D\xE1\xBB\x9B\x69"}, // &Add New Game Directory
    {"\x41\x64\x64\x20\x4E\x65\x77\x20\x47\x61\x6D\x65\x20\x44\x69\x72\x65\x63\x74\x6F\x72\x79", "\x54\x68\xC3\xAA\x6D\x20\x74\x68\xC6\xB0\x20\x6D\xE1\xBB\xA5\x63\x20\x47\x61\x6D\x65\x20\x6D\xE1\xBB\x9B\x69"}, // Add New Game Directory
    {"\x46\x61\x76\x6F\x72\x69\x74\x65", "\x59\xC3\xAA\x75\x20\x74\x68\xC3\xAD\x63\x68"}, // Favorite
    {"\x53\x74\x61\x72\x74\x20\x47\x61\x6D\x65", "\x43\x68\xC6\xA1\x69\x20\x6E\x67\x61\x79"}, // Start Game
    {"\x53\x74\x61\x72\x74\x20\x47\x61\x6D\x65\x20\x77\x69\x74\x68\x6F\x75\x74\x20\x43\x75\x73\x74\x6F\x6D\x20\x43\x6F\x6E\x66\x69\x67\x75\x72\x61\x74\x69\x6F\x6E", "\x43\x68\xC6\xA1\x69\x20\x6B\x68\xC3\xB4\x6E\x67\x20\x64\xC3\xB9\x6E\x67\x20\x63\xE1\xBA\xA5\x75\x20\x68\xC3\xAC\x6E\x68\x20\x72\x69\xC3\xAA\x6E\x67"}, // Start Game without Custom Configuration
    {"\x4F\x70\x65\x6E\x20\x53\x61\x76\x65\x20\x44\x61\x74\x61\x20\x4C\x6F\x63\x61\x74\x69\x6F\x6E", "\x4D\xE1\xBB\x9F\x20\x74\x68\xC6\xB0\x20\x6D\xE1\xBB\xA5\x63\x20\x64\xE1\xBB\xAF\x20\x6C\x69\xE1\xBB\x87\x75\x20\x53\x61\x76\x65"}, // Open Save Data Location
    {"\x4F\x70\x65\x6E\x20\x4D\x6F\x64\x20\x44\x61\x74\x61\x20\x4C\x6F\x63\x61\x74\x69\x6F\x6E", "\x4D\xE1\xBB\x9F\x20\x74\x68\xC6\xB0\x20\x6D\xE1\xBB\xA5\x63\x20\x63\x68\xE1\xBB\xA9\x61\x20\x4D\x6F\x64"}, // Open Mod Data Location
    {"\x4F\x70\x65\x6E\x20\x54\x72\x61\x6E\x73\x66\x65\x72\x61\x62\x6C\x65\x20\x50\x69\x70\x65\x6C\x69\x6E\x65\x20\x43\x61\x63\x68\x65", "\x4D\xE1\xBB\x9F\x20\x74\x68\xC6\xB0\x20\x6D\xE1\xBB\xA5\x63\x20\x53\x68\x61\x64\x65\x72\x20\x43\x61\x63\x68\x65"}, // Open Transferable Pipeline Cache
    {"\x50\x72\x65\x70\x61\x72\x65\x20\x73\x68\x61\x64\x65\x72\x73\x20\x28\x77\x61\x72\x6D\x2D\x75\x70\x29", "\x43\x68\x75\xE1\xBA\xA9\x6E\x20\x62\xE1\xBB\x8B\x20\x74\x72\xC6\xB0\xE1\xBB\x9B\x63\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x53\x68\x61\x64\x65\x72\x20\x28\x63\x68\xE1\xBB\x89\x20\x63\xE1\xBA\xA7\x6E\x20\x31\x20\x6C\xE1\xBA\xA7\x6E\x29"}, // Prepare shaders (warm-up)
    {"\x52\x65\x62\x75\x69\x6C\x64\x20\x73\x68\x61\x64\x65\x72\x20\x77\x61\x72\x6D\x2D\x75\x70", "\x54\xE1\xBA\xA1\x6F\x20\x6C\xE1\xBA\xA1\x69\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x53\x68\x61\x64\x65\x72\x20\x77\x61\x72\x6D\x2D\x75\x70"}, // Rebuild shader warm-up
    {"\x44\x65\x6C\x65\x74\x65\x20\x73\x68\x61\x64\x65\x72\x20\x77\x61\x72\x6D\x2D\x75\x70", "\x58\x6F\xC3\xA1\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x53\x68\x61\x64\x65\x72\x20\x77\x61\x72\x6D\x2D\x75\x70"}, // Delete shader warm-up
    {"\x4C\x69\x6E\x6B\x20\x74\x6F\x20\x52\x79\x75\x6A\x69\x6E\x78", "\x4C\x69\xC3\xAA\x6E\x20\x6B\xE1\xBA\xBF\x74\x20\x64\xE1\xBB\xAF\x20\x6C\x69\xE1\xBB\x87\x75\x20\x76\xE1\xBB\x9B\x69\x20\x52\x79\x75\x6A\x69\x6E\x78"}, // Link to Ryujinx
    {"\x52\x65\x6D\x6F\x76\x65", "\x47\xE1\xBB\xA1\x20\x62\xE1\xBB\x8F"}, // Remove
    {"\x52\x65\x6D\x6F\x76\x65\x20\x49\x6E\x73\x74\x61\x6C\x6C\x65\x64\x20\x55\x70\x64\x61\x74\x65", "\x47\xE1\xBB\xA1\x20\x62\xE1\xBA\xA3\x6E\x20\x63\xE1\xBA\xAD\x70\x20\x6E\x68\xE1\xBA\xAD\x74\x20\xC4\x91\xC3\xA3\x20\x63\xC3\xA0\x69"}, // Remove Installed Update
    {"\x52\x65\x6D\x6F\x76\x65\x20\x41\x6C\x6C\x20\x49\x6E\x73\x74\x61\x6C\x6C\x65\x64\x20\x44\x4C\x43", "\x47\xE1\xBB\xA1\x20\x74\x6F\xC3\xA0\x6E\x20\x62\xE1\xBB\x99\x20\x44\x4C\x43\x20\xC4\x91\xC3\xA3\x20\x63\xC3\xA0\x69"}, // Remove All Installed DLC
    {"\x52\x65\x6D\x6F\x76\x65\x20\x43\x75\x73\x74\x6F\x6D\x20\x43\x6F\x6E\x66\x69\x67\x75\x72\x61\x74\x69\x6F\x6E", "\x58\x6F\xC3\xA1\x20\x63\xE1\xBA\xA5\x75\x20\x68\xC3\xAC\x6E\x68\x20\x72\x69\xC3\xAA\x6E\x67\x20\x63\xE1\xBB\xA7\x61\x20\x47\x61\x6D\x65"}, // Remove Custom Configuration
    {"\x52\x65\x6D\x6F\x76\x65\x20\x43\x61\x63\x68\x65\x20\x53\x74\x6F\x72\x61\x67\x65", "\x58\x6F\xC3\xA1\x20\x62\xE1\xBB\x99\x20\x6E\x68\xE1\xBB\x9B\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x43\x61\x63\x68\x65"}, // Remove Cache Storage
    {"\x52\x65\x6D\x6F\x76\x65\x20\x4F\x70\x65\x6E\x47\x4C\x20\x50\x69\x70\x65\x6C\x69\x6E\x65\x20\x43\x61\x63\x68\x65", "\x58\x6F\xC3\xA1\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x4F\x70\x65\x6E\x47\x4C\x20\x50\x69\x70\x65\x6C\x69\x6E\x65"}, // Remove OpenGL Pipeline Cache
    {"\x52\x65\x6D\x6F\x76\x65\x20\x56\x75\x6C\x6B\x61\x6E\x20\x50\x69\x70\x65\x6C\x69\x6E\x65\x20\x43\x61\x63\x68\x65", "\x58\x6F\xC3\xA1\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x56\x75\x6C\x6B\x61\x6E\x20\x50\x69\x70\x65\x6C\x69\x6E\x65"}, // Remove Vulkan Pipeline Cache
    {"\x52\x65\x6D\x6F\x76\x65\x20\x41\x6C\x6C\x20\x50\x69\x70\x65\x6C\x69\x6E\x65\x20\x43\x61\x63\x68\x65\x73", "\x58\x6F\xC3\xA1\x20\x74\x6F\xC3\xA0\x6E\x20\x62\xE1\xBB\x99\x20\x50\x69\x70\x65\x6C\x69\x6E\x65\x20\x43\x61\x63\x68\x65\x73"}, // Remove All Pipeline Caches
    {"\x52\x65\x6D\x6F\x76\x65\x20\x41\x6C\x6C\x20\x49\x6E\x73\x74\x61\x6C\x6C\x65\x64\x20\x43\x6F\x6E\x74\x65\x6E\x74\x73", "\x47\xE1\xBB\xA1\x20\x74\x6F\xC3\xA0\x6E\x20\x62\xE1\xBB\x99\x20\x6E\xE1\xBB\x99\x69\x20\x64\x75\x6E\x67\x20\xC4\x91\xC3\xA3\x20\x63\xC3\xA0\x69"}, // Remove All Installed Contents
    {"\x4D\x61\x6E\x61\x67\x65\x20\x50\x6C\x61\x79\x20\x54\x69\x6D\x65", "\x51\x75\xE1\xBA\xA3\x6E\x20\x6C\xC3\xBD\x20\x67\x69\xE1\xBB\x9D\x20\x63\x68\xC6\xA1\x69"}, // Manage Play Time
    {"\x45\x64\x69\x74\x20\x50\x6C\x61\x79\x20\x54\x69\x6D\x65\x20\x44\x61\x74\x61", "\x43\x68\xE1\xBB\x89\x6E\x68\x20\x73\xE1\xBB\xAD\x61\x20\x74\x68\xE1\xBB\x9D\x69\x20\x67\x69\x61\x6E\x20\x63\x68\xC6\xA1\x69"}, // Edit Play Time Data
    {"\x52\x65\x6D\x6F\x76\x65\x20\x50\x6C\x61\x79\x20\x54\x69\x6D\x65\x20\x44\x61\x74\x61", "\x58\x6F\xC3\xA1\x20\x74\x68\xE1\xBB\x9D\x69\x20\x67\x69\x61\x6E\x20\x63\x68\xC6\xA1\x69"}, // Remove Play Time Data
    {"\x44\x75\x6D\x70\x20\x4E\x55\x50\x2E\x2E\x2E", "\x54\x72\xC3\xAD\x63\x68\x20\x78\x75\xE1\xBA\xA5\x74\x20\x67\xC3\xB3\x69\x20\x4E\x55\x50\x2E\x2E\x2E"}, // Dump NUP...
    {"\x44\x75\x6D\x70\x20\x52\x6F\x6D\x46\x53", "\x54\x72\xC3\xAD\x63\x68\x20\x78\x75\xE1\xBA\xA5\x74\x20\x52\x6F\x6D\x46\x53"}, // Dump RomFS
    {"\x44\x75\x6D\x70\x20\x52\x6F\x6D\x46\x53\x20\x74\x6F\x20\x53\x44\x4D\x43", "\x54\x72\xC3\xAD\x63\x68\x20\x78\x75\xE1\xBA\xA5\x74\x20\x52\x6F\x6D\x46\x53\x20\x76\xC3\xA0\x6F\x20\x74\x68\xE1\xBA\xBB\x20\x6E\x68\xE1\xBB\x9B\x20\x53\x44\x4D\x43"}, // Dump RomFS to SDMC
    {"\x56\x65\x72\x69\x66\x79\x20\x49\x6E\x74\x65\x67\x72\x69\x74\x79", "\x4B\x69\xE1\xBB\x83\x6D\x20\x74\x72\x61\x20\x74\x6F\xC3\xA0\x6E\x20\x76\xE1\xBA\xB9\x6E\x20\x66\x69\x6C\x65"}, // Verify Integrity
    {"\x43\x6F\x70\x79\x20\x54\x69\x74\x6C\x65\x20\x49\x44\x20\x74\x6F\x20\x43\x6C\x69\x70\x62\x6F\x61\x72\x64", "\x53\x61\x6F\x20\x63\x68\xC3\xA9\x70\x20\x54\x69\x74\x6C\x65\x20\x49\x44"}, // Copy Title ID to Clipboard
    {"\x43\x6F\x6E\x76\x65\x72\x74\x20\x74\x6F\x20\x4E\x55\x50\x2E\x2E\x2E", "\x43\x68\x75\x79\xE1\xBB\x83\x6E\x20\xC4\x91\xE1\xBB\x95\x69\x20\x73\x61\x6E\x67\x20\x4E\x55\x50\x2E\x2E\x2E"}, // Convert to NUP...
    {"\x43\x72\x65\x61\x74\x65\x20\x53\x68\x6F\x72\x74\x63\x75\x74", "\x54\xE1\xBA\xA1\x6F\x20\x6C\xE1\xBB\x91\x69\x20\x74\xE1\xBA\xAF\x74\x20\x28\x53\x68\x6F\x72\x74\x63\x75\x74\x29"}, // Create Shortcut
    {"\x43\x72\x65\x61\x74\x65\x20\x26\x53\x68\x6F\x72\x74\x63\x75\x74", "\x54\xE1\xBA\xA1\x6F\x20\x26\x6C\xE1\xBB\x91\x69\x20\x74\xE1\xBA\xAF\x74\x20\x28\x53\x68\x6F\x72\x74\x63\x75\x74\x29"}, // Create &Shortcut
    {"\x41\x64\x64\x20\x74\x6F\x20\x44\x65\x73\x6B\x74\x6F\x70", "\x54\xE1\xBA\xA1\x6F\x20\x6C\xE1\xBB\x91\x69\x20\x74\xE1\xBA\xAF\x74\x20\x72\x61\x20\x4D\xC3\xA0\x6E\x20\x68\xC3\xAC\x6E\x68\x20\x63\x68\xC3\xAD\x6E\x68"}, // Add to Desktop
    {"\x41\x64\x64\x20\x74\x6F\x20\x41\x70\x70\x6C\x69\x63\x61\x74\x69\x6F\x6E\x73\x20\x4D\x65\x6E\x75", "\x54\x68\xC3\xAA\x6D\x20\x76\xC3\xA0\x6F\x20\x6D\x65\x6E\x75\x20\xE1\xBB\xA9\x6E\x67\x20\x64\xE1\xBB\xA5\x6E\x67"}, // Add to Applications Menu
    {"\x45\x64\x69\x74\x20\x4E\x55\x50\x20\x4D\x65\x74\x61\x64\x61\x74\x61\x2E\x2E\x2E", "\x43\x68\xE1\xBB\x89\x6E\x68\x20\x73\xE1\xBB\xAD\x61\x20\x74\x68\xC3\xB4\x6E\x67\x20\x74\x69\x6E\x20\x4E\x55\x50\x2E\x2E\x2E"}, // Edit NUP Metadata...
    {"\x43\x6F\x6E\x66\x69\x67\x75\x72\x65\x20\x47\x61\x6D\x65", "\x43\xE1\xBA\xA5\x75\x20\x68\xC3\xAC\x6E\x68\x20\x47\x61\x6D\x65"}, // Configure Game
    {"\x50\x72\x6F\x70\x65\x72\x74\x69\x65\x73", "\x54\x68\x75\xE1\xBB\x99\x63\x20\x74\xC3\xAD\x6E\x68\x20\x47\x61\x6D\x65"}, // Properties
    {"\x47\x65\x6E\x65\x72\x61\x6C", "\x43\xC3\xA0\x69\x20\xC4\x91\xE1\xBA\xB7\x74\x20\x63\x68\x75\x6E\x67"}, // General
    {"\x53\x79\x73\x74\x65\x6D", "\x48\xE1\xBB\x87\x20\x74\x68\xE1\xBB\x91\x6E\x67"}, // System
    {"\x43\x50\x55", "\x42\xE1\xBB\x99\x20\x78\xE1\xBB\xAD\x20\x6C\xC3\xBD\x20\x28\x43\x50\x55\x29"}, // CPU
    {"\x47\x72\x61\x70\x68\x69\x63\x73", "\xC4\x90\xE1\xBB\x93\x20\x68\x6F\xE1\xBA\xA1\x20\x28\x47\x50\x55\x29"}, // Graphics
    {"\x41\x75\x64\x69\x6F", "\xC3\x82\x6D\x20\x74\x68\x61\x6E\x68"}, // Audio
    {"\x43\x6F\x6E\x74\x72\x6F\x6C\x73", "\xC4\x90\x69\xE1\xBB\x81\x75\x20\x6B\x68\x69\xE1\xBB\x83\x6E"}, // Controls
    {"\x44\x65\x62\x75\x67\x67\x69\x6E\x67", "\x47\xE1\xBB\xA1\x20\x6C\xE1\xBB\x97\x69"}, // Debugging
    {"\x57\x65\x62", "\x4D\xE1\xBA\xA1\x6E\x67\x20\x26\x20\x57\x65\x62"}, // Web
    {"\x41\x64\x64\x2D\x4F\x6E\x73", "\x54\x69\xE1\xBB\x87\x6E\x20\xC3\xAD\x63\x68"}, // Add-Ons
    {"\x41\x64\x76\x61\x6E\x63\x65\x64", "\x4E\xC3\xA2\x6E\x67\x20\x63\x61\x6F"}, // Advanced
    {"\x49\x6E\x66\x6F", "\x54\x68\xC3\xB4\x6E\x67\x20\x74\x69\x6E"}, // Info
    {"\x47\x61\x6D\x65\x20\x53\x65\x74\x74\x69\x6E\x67\x73", "\x43\xC3\xA0\x69\x20\xC4\x91\xE1\xBA\xB7\x74\x20\x47\x61\x6D\x65"}, // Game Settings
    {"\x41\x50\x49\x20\x53\x65\x74\x74\x69\x6E\x67\x73", "\x54\x68\x69\xE1\xBA\xBF\x74\x20\x6C\xE1\xBA\xAD\x70\x20\x41\x50\x49"}, // API Settings
    {"\x47\x72\x61\x70\x68\x69\x63\x73\x20\x53\x65\x74\x74\x69\x6E\x67\x73", "\x54\x68\x69\xE1\xBA\xBF\x74\x20\x6C\xE1\xBA\xAD\x70\x20\xC4\x90\xE1\xBB\x93\x20\x68\x6F\xE1\xBA\xA1"}, // Graphics Settings
    {"\x4F\x4B", "\xC4\x90\xE1\xBB\x93\x6E\x67\x20\xC3\xBD"}, // OK
    {"\x43\x61\x6E\x63\x65\x6C", "\x48\x75\xE1\xBB\xB7\x20\x62\xE1\xBB\x8F"}, // Cancel
    {"\x41\x70\x70\x6C\x79", "\xC3\x81\x70\x20\x64\xE1\xBB\xA5\x6E\x67"}, // Apply
    {"\x52\x65\x73\x74\x6F\x72\x65\x20\x44\x65\x66\x61\x75\x6C\x74\x73", "\x4D\xE1\xBA\xB7\x63\x20\xC4\x91\xE1\xBB\x8B\x6E\x68"}, // Restore Defaults
    {"\x52\x65\x73\x65\x74", "\xC4\x90\xE1\xBA\xB7\x74\x20\x6C\xE1\xBA\xA1\x69"}, // Reset
    {"\x43\x6C\x6F\x73\x65", "\xC4\x90\xC3\xB3\x6E\x67"}, // Close
    {"\x53\x61\x76\x65", "\x4C\xC6\xB0\x75"}, // Save
    {"\x42\x72\x6F\x77\x73\x65\x2E\x2E\x2E", "\x43\x68\xE1\xBB\x8D\x6E\x2E\x2E\x2E"}, // Browse...
    {"\x44\x65\x74\x65\x63\x74\x20\x49\x63\x6F\x6E", "\x54\xE1\xBB\xB1\x20\x74\xC3\xAC\x6D\x20\x49\x63\x6F\x6E"}, // Detect Icon
    {"\x55\x70\x6C\x6F\x61\x64\x20\x49\x63\x6F\x6E", "\x54\xE1\xBA\xA3\x69\x20\x49\x63\x6F\x6E\x20\x6C\xC3\xAA\x6E"}, // Upload Icon
    {"\x26\x46\x69\x6C\x65", "\x26\x54\xE1\xBB\x87\x70\x20\x74\x69\x6E"}, // &File
    {"\x46\x69\x6C\x65", "\x54\xE1\xBB\x87\x70\x20\x74\x69\x6E"}, // File
    {"\x26\x52\x65\x63\x65\x6E\x74\x20\x46\x69\x6C\x65\x73", "\x54\xE1\xBB\x87\x70\x20\x26\x67\xE1\xBA\xA7\x6E\x20\xC4\x91\xC3\xA2\x79"}, // &Recent Files
    {"\x4C\x26\x6F\x61\x64\x20\x46\x69\x6C\x65\x2E\x2E\x2E", "\x26\x4D\xE1\xBB\x9F\x20\x74\xE1\xBB\x87\x70\x20\x67\x61\x6D\x65\x2E\x2E\x2E"}, // L&oad File...
    {"\x4C\x6F\x61\x64\x20\x26\x46\x6F\x6C\x64\x65\x72\x2E\x2E\x2E", "\x4D\xE1\xBB\x9F\x20\x26\x74\x68\xC6\xB0\x20\x6D\xE1\xBB\xA5\x63\x2E\x2E\x2E"}, // Load &Folder...
    {"\x45\x26\x78\x69\x74", "\x26\x54\x68\x6F\xC3\xA1\x74"}, // E&xit
    {"\x45\x78\x69\x74", "\x54\x68\x6F\xC3\xA1\x74"}, // Exit
    {"\x26\x45\x6D\x75\x6C\x61\x74\x69\x6F\x6E", "\x26\x47\x69\xE1\xBA\xA3\x20\x6C\xE1\xBA\xAD\x70"}, // &Emulation
    {"\x45\x6D\x75\x6C\x61\x74\x69\x6F\x6E", "\x47\x69\xE1\xBA\xA3\x20\x6C\xE1\xBA\xAD\x70"}, // Emulation
    {"\x26\x53\x74\x61\x72\x74", "\x26\x42\xE1\xBA\xAF\x74\x20\xC4\x91\xE1\xBA\xA7\x75"}, // &Start
    {"\x53\x74\x61\x72\x74", "\x42\xE1\xBA\xAF\x74\x20\xC4\x91\xE1\xBA\xA7\x75"}, // Start
    {"\x26\x50\x61\x75\x73\x65", "\x26\x54\xE1\xBA\xA1\x6D\x20\x64\xE1\xBB\xAB\x6E\x67"}, // &Pause
    {"\x50\x61\x75\x73\x65", "\x54\xE1\xBA\xA1\x6D\x20\x64\xE1\xBB\xAB\x6E\x67"}, // Pause
    {"\x26\x53\x74\x6F\x70", "\x26\x44\xE1\xBB\xAB\x6E\x67"}, // &Stop
    {"\x53\x74\x6F\x70", "\x44\xE1\xBB\xAB\x6E\x67"}, // Stop
    {"\x26\x52\x65\x73\x74\x61\x72\x74", "\x26\x4B\x68\xE1\xBB\x9F\x69\x20\xC4\x91\xE1\xBB\x99\x6E\x67\x20\x6C\xE1\xBA\xA1\x69"}, // &Restart
    {"\x52\x65\x73\x74\x61\x72\x74", "\x4B\x68\xE1\xBB\x9F\x69\x20\xC4\x91\xE1\xBB\x99\x6E\x67\x20\x6C\xE1\xBA\xA1\x69"}, // Restart
    {"\x43\x6F\x6E\x26\x66\x69\x67\x75\x72\x65\x2E\x2E\x2E", "\x26\x43\xC3\xA0\x69\x20\xC4\x91\xE1\xBA\xB7\x74\x2E\x2E\x2E"}, // Con&figure...
    {"\x43\x6F\x6E\x66\x69\x67\x75\x72\x65\x2E\x2E\x2E", "\x43\xC3\xA0\x69\x20\xC4\x91\xE1\xBA\xB7\x74\x2E\x2E\x2E"}, // Configure...
    {"\x43\x6F\x6E\x66\x69\x67\x75\x72\x65\x20\x43\x26\x75\x72\x72\x65\x6E\x74\x20\x47\x61\x6D\x65\x2E\x2E\x2E", "\x43\xC3\xA0\x69\x20\xC4\x91\xE1\xBA\xB7\x74\x20\x26\x67\x61\x6D\x65\x20\x68\x69\xE1\xBB\x87\x6E\x20\x74\xE1\xBA\xA1\x69\x2E\x2E\x2E"}, // Configure C&urrent Game...
    {"\x26\x56\x69\x65\x77", "\x26\x48\x69\xE1\xBB\x83\x6E\x20\x74\x68\xE1\xBB\x8B"}, // &View
    {"\x56\x69\x65\x77", "\x48\x69\xE1\xBB\x83\x6E\x20\x74\x68\xE1\xBB\x8B"}, // View
    {"\x46\x26\x75\x6C\x6C\x73\x63\x72\x65\x65\x6E", "\x26\x54\x6F\xC3\xA0\x6E\x20\x6D\xC3\xA0\x6E\x20\x68\xC3\xAC\x6E\x68"}, // F&ullscreen
    {"\x46\x75\x6C\x6C\x73\x63\x72\x65\x65\x6E", "\x54\x6F\xC3\xA0\x6E\x20\x6D\xC3\xA0\x6E\x20\x68\xC3\xAC\x6E\x68"}, // Fullscreen
    {"\x53\x68\x6F\x77\x20\x26\x50\x65\x72\x66\x6F\x72\x6D\x61\x6E\x63\x65\x20\x4F\x76\x65\x72\x6C\x61\x79", "\x48\x69\xE1\xBB\x87\x6E\x20\x74\x68\xC3\xB4\x6E\x67\x20\x73\xE1\xBB\x91\x20\x26\x46\x50\x53"}, // Show &Performance Overlay
    {"\x50\x65\x72\x66\x6F\x72\x6D\x61\x6E\x63\x65\x20\x4F\x76\x65\x72\x6C\x61\x79", "\x54\x68\xC3\xB4\x6E\x67\x20\x73\xE1\xBB\x91\x20\x46\x50\x53"}, // Performance Overlay
    {"\x26\x54\x6F\x6F\x6C\x73", "\x26\x43\xC3\xB4\x6E\x67\x20\x63\xE1\xBB\xA5"}, // &Tools
    {"\x54\x6F\x6F\x6C\x73", "\x43\xC3\xB4\x6E\x67\x20\x63\xE1\xBB\xA5"}, // Tools
    {"\x26\x43\x61\x70\x74\x75\x72\x65\x20\x53\x63\x72\x65\x65\x6E\x73\x68\x6F\x74", "\x26\x43\x68\xE1\xBB\xA5\x70\x20\x6D\xC3\xA0\x6E\x20\x68\xC3\xAC\x6E\x68"}, // &Capture Screenshot
    {"\x26\x48\x65\x6C\x70", "\x26\x54\x72\xE1\xBB\xA3\x20\x67\x69\xC3\xBA\x70"}, // &Help
    {"\x48\x65\x6C\x70", "\x54\x72\xE1\xBB\xA3\x20\x67\x69\xC3\xBA\x70"}, // Help
    {"\x52\x65\x64\x75\x63\x65\x20\x52\x41\x4D\x20\x75\x73\x61\x67\x65", "\x47\x69\xE1\xBA\xA3\x69\x20\x70\x68\xC3\xB3\x6E\x67\x20\x52\x41\x4D"}, // Reduce RAM usage
    {"\x44\x65\x6C\x65\x74\x65\x20\x53\x68\x61\x64\x65\x72\x20\x43\x61\x63\x68\x65", "\x58\x6F\xC3\xA1\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x53\x68\x61\x64\x65\x72"}, // Delete Shader Cache
    {"\x44\x65\x6C\x65\x74\x65\x20\x53\x68\x61\x64\x65\x72\x20\x43\x61\x63\x68\x65\x2E\x2E\x2E", "\x58\x6F\xC3\xA1\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x53\x68\x61\x64\x65\x72\x2E\x2E\x2E"}, // Delete Shader Cache...
    {"\x26\x44\x65\x6C\x65\x74\x65\x20\x53\x68\x61\x64\x65\x72\x20\x43\x61\x63\x68\x65\x2E\x2E\x2E", "\x26\x58\x6F\xC3\xA1\x20\x62\xE1\xBB\x99\x20\xC4\x91\xE1\xBB\x87\x6D\x20\x53\x68\x61\x64\x65\x72\x2E\x2E\x2E"}, // &Delete Shader Cache...
    {"\x45\x78\x70\x6F\x72\x74\x20\x4C\x6F\x67", "\x58\x75\xE1\xBA\xA5\x74\x20\x66\x69\x6C\x65\x20\x4C\x6F\x67"}, // Export Log
    {"\x45\x78\x70\x6F\x72\x74\x20\x4C\x6F\x67\x2E\x2E\x2E", "\x58\x75\xE1\xBA\xA5\x74\x20\x66\x69\x6C\x65\x20\x4C\x6F\x67\x2E\x2E\x2E"}, // Export Log...
    {"\x26\x45\x78\x70\x6F\x72\x74\x20\x4C\x6F\x67\x2E\x2E\x2E", "\x26\x58\x75\xE1\xBA\xA5\x74\x20\x66\x69\x6C\x65\x20\x4C\x6F\x67\x2E\x2E\x2E"}, // &Export Log...
    {"\x57\x68\x61\x74\x27\x73\x20\x4E\x65\x77", "\x43\xC3\xB3\x20\x67\xC3\xAC\x20\x6D\xE1\xBB\x9B\x69"}, // What's New
    {"\x57\x68\x61\x74\x27\x73\x20\x26\x4E\x65\x77", "\x43\xC3\xB3\x20\x67\xC3\xAC\x20\x26\x6D\xE1\xBB\x9B\x69"}, // What's &New
    {"\x43\x68\x65\x63\x6B\x20\x66\x6F\x72\x20\x55\x70\x64\x61\x74\x65\x73", "\x4B\x69\xE1\xBB\x83\x6D\x20\x74\x72\x61\x20\x62\xE1\xBA\xA3\x6E\x20\x63\xE1\xBA\xAD\x70\x20\x6E\x68\xE1\xBA\xAD\x74"}, // Check for Updates
    {"\x55\x70\x64\x61\x74\x65", "\x43\xE1\xBA\xAD\x70\x20\x6E\x68\xE1\xBA\xAD\x74"}, // Update
    {"\x56\x69\x65\x77\x20\x6F\x6E\x20\x47\x69\x74\x48\x75\x62", "\x58\x65\x6D\x20\x74\x72\xC3\xAA\x6E\x20\x47\x69\x74\x48\x75\x62"}, // View on GitHub
    {"\x57\x6F\x75\x6C\x64\x20\x79\x6F\x75\x20\x6C\x69\x6B\x65\x20\x74\x6F\x20\x64\x6F\x77\x6E\x6C\x6F\x61\x64\x20\x74\x68\x69\x73\x20\x75\x70\x64\x61\x74\x65\x3F", "\x42\xE1\xBA\xA1\x6E\x20\x63\xC3\xB3\x20\x6D\x75\xE1\xBB\x91\x6E\x20\x74\xE1\xBA\xA3\x69\x20\x62\xE1\xBA\xA3\x6E\x20\x63\xE1\xBA\xAD\x70\x20\x6E\x68\xE1\xBA\xAD\x74\x20\x6E\xC3\xA0\x79\x20\x6B\x68\xC3\xB4\x6E\x67\x3F"}, // Would you like to download this update?
    {"\x49\x6E\x73\x74\x61\x6C\x6C", "\x43\xC3\xA0\x69\x20\xC4\x91\xE1\xBA\xB7\x74"}, // Install
    {"\x52\x65\x6D\x69\x6E\x64\x20\x4D\x65\x20\x4C\x61\x74\x65\x72", "\x4E\x68\xE1\xBA\xAF\x63\x20\x74\xC3\xB4\x69\x20\x73\x61\x75"}, // Remind Me Later
    {"\x53\x6B\x69\x70\x20\x54\x68\x69\x73\x20\x56\x65\x72\x73\x69\x6F\x6E", "\x42\xE1\xBB\x8F\x20\x71\x75\x61\x20\x70\x68\x69\xC3\xAA\x6E\x20\x62\xE1\xBA\xA3\x6E\x20\x6E\xC3\xA0\x79"}, // Skip This Version
    {"\x41\x62\x6F\x75\x74\x20\x46\x72\x69\x74\x63\x68\x69\x69", "\x47\x69\xE1\xBB\x9B\x69\x20\x74\x68\x69\xE1\xBB\x87\x75\x20\x59\x6E\x67\x63\x68\x69\x69\x2D\x58\x33\x44"}, // About Fritchii
    {"\x41\x62\x6F\x75\x74\x20\x59\x6E\x67\x63\x68\x69\x69\x2D\x58\x33\x44", "\x47\x69\xE1\xBB\x9B\x69\x20\x74\x68\x69\xE1\xBB\x87\x75\x20\x59\x6E\x67\x63\x68\x69\x69\x2D\x58\x33\x44"}, // About Yngchii-X3D
    {"\x41\x62\x6F\x75\x74", "\x47\x69\xE1\xBB\x9B\x69\x20\x74\x68\x69\xE1\xBB\x87\x75"}, // About
    {"\x44\x65\x70\x65\x6E\x64\x65\x6E\x63\x69\x65\x73", "\x54\x68\xC3\xA0\x6E\x68\x20\x70\x68\xE1\xBA\xA7\x6E\x20\x70\x68\xE1\xBB\xA5\x20\x74\x68\x75\xE1\xBB\x99\x63"}, // Dependencies
    {"\x4C\x69\x63\x68\x69\x69\x20\x44\x65\x70\x65\x6E\x64\x65\x6E\x63\x69\x65\x73", "\x59\x6E\x67\x63\x68\x69\x69\x2D\x58\x33\x44\x20\x44\x65\x70\x65\x6E\x64\x65\x6E\x63\x69\x65\x73"}, // Lichii Dependencies
    {"\x46\x72\x69\x74\x63\x68\x69\x69\x20\x26\x44\x69\x73\x63\x6F\x72\x64", "\x59\x6E\x67\x63\x68\x69\x69\x20\x26\x44\x69\x73\x63\x6F\x72\x64"}, // Fritchii &Discord
    {"\x44\x26\x6F\x6E\x61\x74\x65", "\xE1\xBB\xA6\x6E\x67\x20\x26\x68\xE1\xBB\x99"}, // D&onate
    {"\x4F\x70\x65\x6E\x20\x26\x52\x65\x63\x65\x6E\x74", "\x4D\xE1\xBB\x9F\x20\x67\xE1\xBA\xA7\x6E\x20\x26\xC4\x91\xC3\xA2\x79"}, // Open &Recent
    {"\x4F\x70\x65\x6E\x20\x26\x46\x6F\x6C\x64\x65\x72", "\x4D\xE1\xBB\x9F\x20\x26\x74\x68\xC6\xB0\x20\x6D\xE1\xBB\xA5\x63"}, // Open &Folder
    {"\x47\x61\x6D\x65\x20\x26\x4C\x69\x73\x74", "\x44\x61\x6E\x68\x20\x73\xC3\xA1\x63\x68\x20\x26\x47\x61\x6D\x65"}, // Game &List
    {"\x26\x49\x63\x6F\x6E\x20\x53\x69\x7A\x65", "\x4B\xC3\xAD\x63\x68\x20\x74\x68\xC6\xB0\xE1\xBB\x9B\x63\x20\x26\x49\x63\x6F\x6E"}, // &Icon Size
    {"\x26\x57\x69\x6E\x64\x6F\x77\x20\x53\x69\x7A\x65", "\x4B\xC3\xAD\x63\x68\x20\x74\x68\xC6\xB0\xE1\xBB\x9B\x63\x20\x26\x63\xE1\xBB\xAD\x61\x20\x73\xE1\xBB\x95"}, // &Window Size
    {"\x26\x4F\x70\x65\x6E\x20\x47\x61\x6D\x65\x2E\x2E\x2E", "\x26\x4D\xE1\xBB\x9F\x20\x47\x61\x6D\x65\x2E\x2E\x2E"}, // &Open Game...
    {"\x4F\x70\x65\x6E\x20\x26\x45\x78\x74\x72\x61\x63\x74\x65\x64\x20\x47\x61\x6D\x65\x2E\x2E\x2E", "\x4D\xE1\xBB\x9F\x20\x47\x61\x6D\x65\x20\xC4\x91\xC3\xA3\x20\x26\x67\x69\xE1\xBA\xA3\x69\x20\x6E\xC3\xA9\x6E\x2E\x2E\x2E"}, // Open &Extracted Game...
    {"\x26\x49\x6E\x73\x74\x61\x6C\x6C\x20\x55\x70\x64\x61\x74\x65\x20\x6F\x72\x20\x44\x4C\x43\x2E\x2E\x2E", "\x26\x43\xC3\xA0\x69\x20\xC4\x91\xE1\xBA\xB7\x74\x20\x42\xE1\xBA\xA3\x6E\x20\x63\xE1\xBA\xAD\x70\x20\x6E\x68\xE1\xBA\xAD\x74\x20\x68\x6F\xE1\xBA\xB7\x63\x20\x44\x4C\x43\x2E\x2E\x2E"}, // &Install Update or DLC...
    {"\x26\x53\x65\x74\x74\x69\x6E\x67\x73\x2E\x2E\x2E", "\x26\x43\xC3\xA0\x69\x20\xC4\x91\xE1\xBA\xB7\x74\x2E\x2E\x2E"}, // &Settings...
    {"\x47\x61\x6D\x65\x20\x26\x53\x65\x74\x74\x69\x6E\x67\x73\x2E\x2E\x2E", "\x43\xC3\xA0\x69\x20\xC4\x91\xE1\xBA\xB7\x74\x20\x26\x47\x61\x6D\x65\x2E\x2E\x2E"}, // Game &Settings...
    {"\x26\x44\x61\x74\x61", "\x44\xE1\xBB\xAF\x20\x26\x6C\x69\xE1\xBB\x87\x75\x20\x28\x44\x61\x74\x61\x29"}, // &Data
    {"\x26\x4E\x41\x4E\x44", "\x42\xE1\xBB\x99\x20\x6E\x68\xE1\xBB\x9B\x20\x26\x4E\x41\x4E\x44"}, // &NAND
    {"\x26\x53\x44\x20\x43\x61\x72\x64", "\x54\x68\xE1\xBA\xBB\x20\x26\x6E\x68\xE1\xBB\x9B\x20\x53\x44"}, // &SD Card
    {"\x26\x4D\x6F\x64\x73", "\x54\x68\xC6\xB0\x20\x6D\xE1\xBB\xA5\x63\x20\x26\x4D\x6F\x64"}, // &Mods
    {"\x4F\x70\x65\x6E\x20\x26\x4C\x6F\x67\x20\x46\x6F\x6C\x64\x65\x72", "\x4D\xE1\xBB\x9F\x20\x74\x68\xC6\xB0\x20\x6D\xE1\xBB\xA5\x63\x20\x26\x4C\x6F\x67"}, // Open &Log Folder
    {"\x26\x4C\x69\x73\x74", "\x44\x61\x6E\x68\x20\x26\x73\xC3\xA1\x63\x68"}, // &List
    {"\x26\x47\x72\x69\x64", "\x4C\x26\xC6\xB0\xE1\xBB\x9B\x69"}, // &Grid
    {"\x26\x43\x61\x72\x6F\x75\x73\x65\x6C", "\x26\x42\xC4\x83\x6E\x67\x20\x63\x68\x75\x79\xE1\xBB\x81\x6E"}, // &Carousel
};

static const size_t s_dict_size = sizeof(s_dict) / sizeof(s_dict[0]);

static const char* TranslateTextBidirectional(const char* text, int is_vn) {
    if (!text || text[0] == '\0') return NULL;
    if (is_vn) {
        for (size_t i = 0; i < s_dict_size; i++) {
            if (strcmp(text, s_dict[i].en) == 0) {
                return s_dict[i].vn;
            }
        }
    } else {
        for (size_t i = 0; i < s_dict_size; i++) {
            if (strcmp(text, s_dict[i].vn) == 0) {
                return s_dict[i].en;
            }
        }
    }
    return NULL;
}

// ==============================================================================
// 3. IAT HOOKS FOR Qt6 TRANSLATION & STRING CONVERSION
// ==============================================================================
typedef void* (*pfn_translate)(void* ret, const char* context, const char* sourceText, const char* disambiguation, int n);
typedef void* (*pfn_tr)(void* ret, const void* metaObj, const char* sourceText, const char* disambiguation, int n);

typedef struct {
    const char* data;
    int64_t size;
} QByteArrayView_C;

typedef void* (*pfn_fromUtf8)(void* ret, const QByteArrayView_C* view);
typedef void* (*pfn_fromLatin1)(void* ret, const QByteArrayView_C* view);

static pfn_translate s_orig_translate = NULL;
static pfn_tr s_orig_tr = NULL;
static pfn_fromUtf8 s_orig_fromUtf8 = NULL;
static pfn_fromLatin1 s_orig_fromLatin1 = NULL;

static void* Hook_translate(void* ret, const char* context, const char* sourceText, const char* disambiguation, int n) {
    if (!s_is_in_game && g_lang_vn && sourceText) {
        const char* vn = TranslateTextBidirectional(sourceText, 1);
        if (vn) {
            sourceText = vn;
        }
    }
    return s_orig_translate(ret, context, sourceText, disambiguation, n);
}

static void* Hook_tr(void* ret, const void* metaObj, const char* sourceText, const char* disambiguation, int n) {
    if (!s_is_in_game && g_lang_vn && sourceText) {
        const char* vn = TranslateTextBidirectional(sourceText, 1);
        if (vn) {
            sourceText = vn;
        }
    }
    return s_orig_tr(ret, metaObj, sourceText, disambiguation, n);
}

static void* Hook_fromUtf8(void* ret, const QByteArrayView_C* view) {
    QByteArrayView_C mod_view;
    if (!s_is_in_game && g_lang_vn && view && view->data && view->size > 0 && view->size < 256) {
        char buf[256];
        memcpy(buf, view->data, view->size);
        buf[view->size] = '\0';
        const char* vn = TranslateTextBidirectional(buf, 1);
        if (vn) {
            mod_view.data = vn;
            mod_view.size = (int64_t)strlen(vn);
            view = &mod_view;
        }
    }
    return s_orig_fromUtf8(ret, view);
}

static void* Hook_fromLatin1(void* ret, const QByteArrayView_C* view) {
    QByteArrayView_C mod_view;
    if (!s_is_in_game && g_lang_vn && view && view->data && view->size > 0 && view->size < 256 && s_orig_fromUtf8) {
        char buf[256];
        memcpy(buf, view->data, view->size);
        buf[view->size] = '\0';
        const char* vn = TranslateTextBidirectional(buf, 1);
        if (vn) {
            mod_view.data = vn;
            mod_view.size = (int64_t)strlen(vn);
            return s_orig_fromUtf8(ret, &mod_view);
        }
    }
    return s_orig_fromLatin1(ret, view);
}

static void HookIAT(HMODULE hTargetModule, const char* targetDll, const char* funcName, void* newFunc, void** origFunc) {
    if (!hTargetModule) return;
    BYTE* base = (BYTE*)hTargetModule;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    IMAGE_DATA_DIRECTORY* importDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir->Size == 0) return;

    IMAGE_IMPORT_DESCRIPTOR* impDesc = (IMAGE_IMPORT_DESCRIPTOR*)(base + importDir->VirtualAddress);
    for (; impDesc->Name != 0; impDesc++) {
        char* name = (char*)(base + impDesc->Name);
        if (_stricmp(name, targetDll) == 0) {
            IMAGE_THUNK_DATA* origThunk = (IMAGE_THUNK_DATA*)(base + impDesc->OriginalFirstThunk);
            IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base + impDesc->FirstThunk);
            if (!impDesc->OriginalFirstThunk) origThunk = thunk;

            for (; origThunk->u1.AddressOfData != 0; origThunk++, thunk++) {
                if (!(origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    IMAGE_IMPORT_BY_NAME* impByName = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
                    if (strcmp((char*)impByName->Name, funcName) == 0) {
                        if (*origFunc == NULL) {
                            *origFunc = (void*)thunk->u1.Function;
                        }
                        if ((void*)thunk->u1.Function != newFunc) {
                            DWORD oldProtect;
                            VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
                            thunk->u1.Function = (ULONG_PTR)newFunc;
                            VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
                        }
                        return;
                    }
                }
            }
        }
    }
}

static void InstallQt6IATHooks(void) {
    HMODULE hExe = GetModuleHandleW(NULL);
    if (!hExe) return;

    // Qt6Core.dll string conversion & translation hooks
    HookIAT(hExe, "Qt6Core.dll", "?translate@QCoreApplication@@SA?AVQString@@PEBD00H@Z", (void*)Hook_translate, (void**)&s_orig_translate);
    HookIAT(hExe, "Qt6Core.dll", "?tr@QObject@@SA?AVQString@@PEBD0H@Z", (void*)Hook_tr, (void**)&s_orig_tr);
    HookIAT(hExe, "Qt6Core.dll", "?fromUtf8@QString@@SA?AV1@V?$QBasicUtf8StringView@D@@@Z", (void*)Hook_fromUtf8, (void**)&s_orig_fromUtf8);
    HookIAT(hExe, "Qt6Core.dll", "?fromLatin1@QString@@SA?AV1@V?$QBasicLatin1StringView@D@@@Z", (void*)Hook_fromLatin1, (void**)&s_orig_fromLatin1);
}

// ==============================================================================
// 4. DYNAMIC LIVE WIDGET & ACTION RE-TRANSLATION
// ==============================================================================
static void TriggerQtRetranslate(void) {
    // Safe no-op: Direct live widget translation handles all text immediately
}

#define WM_YNGCHII_INIT_BTN    (WM_APP + 0x55)
#define WM_YNGCHII_REFRESH_VIEW (WM_APP + 0x56)
#define WM_YNGCHII_LANG_TOGGLE  (WM_APP + 0x57)
#define TIMER_ID_YNGCHII 0x7788
#define TIMER_ID_ANIM    0x7789

static HWND s_hwndMain = NULL;
static HWND s_hwndLangBtn = NULL;
static WNDPROC s_origMainWndProc = NULL;
static int s_modern_home_list = 1;
static HICON g_hChainsaw = NULL;

// Vertical Toggle Switch state:
// s_knob_pos = 0.0f (Top = EN), 1.0f (Bottom = VN)
static float s_knob_pos = 0.0f;
static int s_is_dragging = 0;
static int s_drag_moved = 0;
static int s_drag_start_y = 0;
static float s_drag_start_pos = 0.0f;

// Typedefs for Qt6 dynamic introspection
typedef void* (*pfn_allWidgets)(void* ret_qlist);
typedef BOOL (*pfn_inherits)(void* this_ptr, const char* className);
typedef void* (*pfn_objectName)(const void* this_ptr, void* ret_qstr);
typedef void* (*pfn_parentWidget)(void* this_ptr);
typedef void* (*pfn_layout)(void* this_ptr);
typedef void (*pfn_setDirection)(void* layout_ptr, int dir);
typedef void (*pfn_setToolButtonStyle)(void* this_ptr, int style);
typedef void (*pfn_setIconSize)(void* this_ptr, const void* qsize_ptr);
typedef void (*pfn_setMinimumHeight)(void* this_ptr, int h);
typedef void (*pfn_setMinimumWidth)(void* this_ptr, int w);
typedef void (*pfn_setMaximumWidth)(void* this_ptr, int w);
typedef int (*pfn_minimumWidth)(const void* this_ptr);
typedef void* (*pfn_toolTip)(const void* this_ptr, void* ret_qstr);
typedef void (*pfn_setStyleSheet)(void* this_ptr, const void* qstr_ptr);

typedef void* (*pfn_pixmapFromWinHICON)(void* ret_qpixmap, HICON hIcon);
typedef void (*pfn_setPixmap)(void* this_ptr, const void* qpixmap_ptr);
typedef void (*pfn_dtor_pixmap)(void* qpixmap_ptr);
typedef void* (*pfn_fromUtf8_str)(void* ret_qstr, const char* str, int64_t size);
typedef void* (*pfn_toUtf8_fn)(const void* qstr_this, void* ret_qba);
typedef const char* (*pfn_constData_fn)(const void* qba_this);
typedef void (*pfn_str_dtor_fn)(void* q_ptr);
typedef void (*pfn_qba_dtor_fn)(void* qba_ptr);

typedef void* (*pfn_get_text)(void* this_ptr, void* ret_qstr);
typedef void (*pfn_set_text)(void* this_ptr, const void* qstr_ptr);

typedef int (*pfn_tab_count)(void* this_ptr);
typedef void* (*pfn_tab_text)(void* this_ptr, void* ret_qstr, int index);
typedef void (*pfn_tab_setText)(void* this_ptr, int index, const void* qstr_ptr);

typedef void* (*pfn_actions)(void* this_ptr, void* ret_qlist);
typedef void* (*pfn_act_text)(void* this_ptr, void* ret_qstr);
typedef void (*pfn_act_setText)(void* this_ptr, const void* qstr_ptr);

typedef void* (*pfn_view_model)(void* view_this);
typedef void* (*pfn_model_root)(void* model_this);
typedef void* (*pfn_model_item)(void* model_this, int row, int column);
typedef int (*pfn_qitem_rowCount)(void* item_this);
typedef void* (*pfn_qitem_child)(void* item_this, int row, int column);
typedef void* (*pfn_qitem_data)(void* item_this, void* ret_qvariant, int role);
typedef void* (*pfn_qvariant_toString)(const void* qvariant_this, void* ret_qstring);
typedef void (*pfn_qvariant_dtor)(void* qvariant_ptr);

static pfn_allWidgets s_pfn_allWidgets = NULL;
static pfn_inherits s_pfn_inherits = NULL;
static pfn_objectName s_pfn_objectName = NULL;
static pfn_parentWidget s_pfn_parentWidget = NULL;
static pfn_layout s_pfn_layout = NULL;
static pfn_setDirection s_pfn_setDirection = NULL;
static pfn_setToolButtonStyle s_pfn_setToolButtonStyle = NULL;
static pfn_setIconSize s_pfn_setIconSize = NULL;
static pfn_setMinimumHeight s_pfn_setMinimumHeight = NULL;
static pfn_setMinimumWidth s_pfn_setMinimumWidth = NULL;
static pfn_setMaximumWidth s_pfn_setMaximumWidth = NULL;
static pfn_minimumWidth s_pfn_minimumWidth = NULL;
static pfn_toolTip s_pfn_toolTip = NULL;
static pfn_setStyleSheet s_pfn_setStyleSheet = NULL;
static pfn_pixmapFromWinHICON s_pfn_pixmapFromWinHICON = NULL;
static pfn_setPixmap s_pfn_setPixmap = NULL;
static pfn_dtor_pixmap s_pfn_dtor_pixmap = NULL;
static pfn_fromUtf8_str s_pfn_fromUtf8_str = NULL;
static pfn_toUtf8_fn s_pfn_toUtf8 = NULL;
static pfn_constData_fn s_pfn_constData = NULL;
static pfn_str_dtor_fn s_pfn_dtor = NULL;
static pfn_qba_dtor_fn s_pfn_qba_dtor = NULL;

static pfn_get_text s_pfn_btn_text = NULL;
static pfn_set_text s_pfn_btn_setText = NULL;
static pfn_get_text s_pfn_lbl_text = NULL;
static pfn_set_text s_pfn_lbl_setText = NULL;
static pfn_get_text s_pfn_gb_title = NULL;
static pfn_set_text s_pfn_gb_setTitle = NULL;

static pfn_tab_count s_pfn_tab_count = NULL;
static pfn_tab_text s_pfn_tab_text = NULL;
static pfn_tab_setText s_pfn_tab_setText = NULL;

static pfn_actions s_pfn_actions = NULL;
static pfn_act_text s_pfn_act_text = NULL;
static pfn_act_setText s_pfn_act_setText = NULL;

static pfn_view_model s_pfn_view_model = NULL;
static pfn_model_root s_pfn_model_root = NULL;
static pfn_model_item s_pfn_model_item = NULL;
static pfn_qitem_rowCount s_pfn_qitem_rowCount = NULL;
static pfn_qitem_child s_pfn_qitem_child = NULL;
static pfn_qitem_data s_pfn_qitem_data = NULL;
static pfn_qvariant_toString s_pfn_qvariant_toString = NULL;
static pfn_qvariant_dtor s_pfn_qvariant_dtor = NULL;

typedef struct {
    char title[256];
    char file_type[32];
    char file_size[32];
    char play_time[64];
    char add_ons[128];
} GameMetaCache;

static GameMetaCache s_game_meta[128];
static int s_game_meta_count = 0;

static void HelperGetItemText(void* item, char* out_buf, size_t buf_size) {
    if (!item || !s_pfn_qitem_data || !s_pfn_qvariant_toString || !s_pfn_toUtf8 || !s_pfn_constData) {
        if (out_buf && buf_size > 0) out_buf[0] = '\0';
        return;
    }
    char qvar[64] = {0};
    s_pfn_qitem_data(item, qvar, 0); // Qt::DisplayRole = 0
    char qstr[32] = {0};
    s_pfn_qvariant_toString(qvar, qstr);
    char qba[32] = {0};
    s_pfn_toUtf8(qstr, qba);
    const char* s = s_pfn_constData(qba);
    if (s && s[0] != '\0') {
        strncpy(out_buf, s, buf_size - 1);
        out_buf[buf_size - 1] = '\0';
    } else {
        out_buf[0] = '\0';
    }
    if (s_pfn_qba_dtor) s_pfn_qba_dtor(qba);
    if (s_pfn_dtor) s_pfn_dtor(qstr);
    if (s_pfn_qvariant_dtor) s_pfn_qvariant_dtor(qvar);
}

static void TriggerLiveQtWidgetTranslation(int force) {
    if (s_is_in_game) return;
    static int s_in_translation = 0;
    if (s_in_translation) return;
    s_in_translation = 1;

    HMODULE hWidgets = GetModuleHandleW(L"Qt6Widgets.dll");
    HMODULE hCore = GetModuleHandleW(L"Qt6Core.dll");
    HMODULE hGui = GetModuleHandleW(L"Qt6Gui.dll");
    if (!hWidgets || !hCore) {
        s_in_translation = 0;
        return;
    }

    if (!s_pfn_allWidgets) {
        s_pfn_allWidgets = (pfn_allWidgets)GetProcAddress(hWidgets, "?allWidgets@QApplication@@SA?AV?$QList@PEAVQWidget@@@@XZ");
        s_pfn_inherits = (pfn_inherits)GetProcAddress(hCore, "?inherits@QObject@@QEBA_NPEBD@Z");
        s_pfn_objectName = (pfn_objectName)GetProcAddress(hCore, "?objectName@QObject@@QEBA?AVQString@@XZ");
        s_pfn_parentWidget = (pfn_parentWidget)GetProcAddress(hWidgets, "?parentWidget@QWidget@@QEBAPEAV1@XZ");
        s_pfn_layout = (pfn_layout)GetProcAddress(hWidgets, "?layout@QWidget@@QEBAPEAVQLayout@@XZ");
        s_pfn_setDirection = (pfn_setDirection)GetProcAddress(hWidgets, "?setDirection@QBoxLayout@@QEAAXW4Direction@1@@Z");
        s_pfn_setToolButtonStyle = (pfn_setToolButtonStyle)GetProcAddress(hWidgets, "?setToolButtonStyle@QToolButton@@QEAAXW4ToolButtonStyle@Qt@@@Z");
        s_pfn_setIconSize = (pfn_setIconSize)GetProcAddress(hWidgets, "?setIconSize@QAbstractButton@@QEAAXAEBVQSize@@@Z");
        s_pfn_setMinimumHeight = (pfn_setMinimumHeight)GetProcAddress(hWidgets, "?setMinimumHeight@QWidget@@QEAAXH@Z");
        s_pfn_setMinimumWidth = (pfn_setMinimumWidth)GetProcAddress(hWidgets, "?setMinimumWidth@QWidget@@QEAAXH@Z");
        s_pfn_setMaximumWidth = (pfn_setMaximumWidth)GetProcAddress(hWidgets, "?setMaximumWidth@QWidget@@QEAAXH@Z");
        s_pfn_minimumWidth = (pfn_minimumWidth)GetProcAddress(hWidgets, "?minimumWidth@QWidget@@QEBAHXZ");
        s_pfn_toolTip = (pfn_toolTip)GetProcAddress(hWidgets, "?toolTip@QWidget@@QEBA?AVQString@@XZ");
        s_pfn_setStyleSheet = (pfn_setStyleSheet)GetProcAddress(hWidgets, "?setStyleSheet@QWidget@@QEAAXAEBVQString@@@Z");
        if (hGui) {
            s_pfn_pixmapFromWinHICON = (pfn_pixmapFromWinHICON)GetProcAddress(hGui, "?qt_pixmapFromWinHICON@@YA?AVQPixmap@@PEAUHICON__@@@Z");
            s_pfn_dtor_pixmap = (pfn_dtor_pixmap)GetProcAddress(hGui, "??1QPixmap@@UEAA@XZ");
            s_pfn_model_root = (pfn_model_root)GetProcAddress(hGui, "?invisibleRootItem@QStandardItemModel@@QEBAPEAVQStandardItem@@XZ");
            s_pfn_model_item = (pfn_model_item)GetProcAddress(hGui, "?item@QStandardItemModel@@QEBAPEAVQStandardItem@@HH@Z");
            s_pfn_qitem_rowCount = (pfn_qitem_rowCount)GetProcAddress(hGui, "?rowCount@QStandardItem@@QEBAHXZ");
            s_pfn_qitem_child = (pfn_qitem_child)GetProcAddress(hGui, "?child@QStandardItem@@QEBAPEAV1@HH@Z");
            s_pfn_qitem_data = (pfn_qitem_data)GetProcAddress(hGui, "?data@QStandardItem@@UEBA?AVQVariant@@H@Z");
        }
        s_pfn_setPixmap = (pfn_setPixmap)GetProcAddress(hWidgets, "?setPixmap@QLabel@@QEAAXAEBVQPixmap@@@Z");
        s_pfn_fromUtf8_str = (pfn_fromUtf8_str)GetProcAddress(hCore, "?fromUtf8@QString@@SA?AV1@PEBD_J@Z");
        s_pfn_toUtf8 = (pfn_toUtf8_fn)GetProcAddress(hCore, "?toUtf8@QString@@QEGBA?AVQByteArray@@XZ");
        if (!s_pfn_toUtf8) s_pfn_toUtf8 = (pfn_toUtf8_fn)GetProcAddress(hCore, "?toUtf8@QString@@QEHAA?AVQByteArray@@XZ");
        s_pfn_constData = (pfn_constData_fn)GetProcAddress(hCore, "?constData@QByteArray@@QEBAPEBDXZ");
        s_pfn_dtor = (pfn_str_dtor_fn)GetProcAddress(hCore, "??1QString@@QEAA@XZ");
        s_pfn_qba_dtor = (pfn_qba_dtor_fn)GetProcAddress(hCore, "??1QByteArray@@QEAA@XZ");

        s_pfn_btn_text = (pfn_get_text)GetProcAddress(hWidgets, "?text@QAbstractButton@@QEBA?AVQString@@XZ");
        s_pfn_btn_setText = (pfn_set_text)GetProcAddress(hWidgets, "?setText@QAbstractButton@@QEAAXAEBVQString@@@Z");
        s_pfn_lbl_text = (pfn_get_text)GetProcAddress(hWidgets, "?text@QLabel@@QEBA?AVQString@@XZ");
        s_pfn_lbl_setText = (pfn_set_text)GetProcAddress(hWidgets, "?setText@QLabel@@QEAAXAEBVQString@@@Z");
        s_pfn_gb_title = (pfn_get_text)GetProcAddress(hWidgets, "?title@QGroupBox@@QEBA?AVQString@@XZ");
        s_pfn_gb_setTitle = (pfn_set_text)GetProcAddress(hWidgets, "?setTitle@QGroupBox@@QEAAXAEBVQString@@@Z");

        s_pfn_tab_count = (pfn_tab_count)GetProcAddress(hWidgets, "?count@QTabBar@@QEBAHXZ");
        s_pfn_tab_text = (pfn_tab_text)GetProcAddress(hWidgets, "?tabText@QTabBar@@QEBA?AVQString@@H@Z");
        s_pfn_tab_setText = (pfn_tab_setText)GetProcAddress(hWidgets, "?setTabText@QTabBar@@QEAAXHAEBVQString@@@Z");

        s_pfn_actions = (pfn_actions)GetProcAddress(hWidgets, "?actions@QWidget@@QEBA?AV?$QList@PEAVQAction@@@@XZ");
        if (hGui) {
            s_pfn_act_text = (pfn_act_text)GetProcAddress(hGui, "?text@QAction@@QEBA?AVQString@@XZ");
            s_pfn_act_setText = (pfn_act_setText)GetProcAddress(hGui, "?setText@QAction@@QEAAXAEBVQString@@@Z");
        }
        s_pfn_view_model = (pfn_view_model)GetProcAddress(hWidgets, "?model@QAbstractItemView@@QEBAPEAVQAbstractItemModel@@XZ");
        s_pfn_qvariant_toString = (pfn_qvariant_toString)GetProcAddress(hCore, "?toString@QVariant@@QEBA?AVQString@@XZ");
        s_pfn_qvariant_dtor = (pfn_qvariant_dtor)GetProcAddress(hCore, "??1QVariant@@QEAA@XZ");
    }

    if (!s_pfn_allWidgets || !s_pfn_inherits || !s_pfn_fromUtf8_str || !s_pfn_toUtf8 || !s_pfn_constData || !s_pfn_dtor) {
        s_in_translation = 0;
        return;
    }

    struct {
        void* d;
        void** ptr;
        int64_t size;
    } widget_list;
    memset(&widget_list, 0, sizeof(widget_list));

    s_pfn_allWidgets(&widget_list);

    static int s_last_lang = -1;
    static int s_last_mode = -1;
    int lang_changed = (s_last_lang != g_lang_vn);
    int mode_changed = (s_last_mode != s_modern_home_list);
    s_last_lang = g_lang_vn;
    s_last_mode = s_modern_home_list;

    if (widget_list.ptr && widget_list.size > 0) {
        for (int64_t i = 0; i < widget_list.size; i++) {
            void* w = widget_list.ptr[i];
            if (!w) continue;

            // Check ObjectName
            char obj_name_buf[128] = {0};
            if (s_pfn_objectName) {
                char qstr_n[32] = {0};
                s_pfn_objectName(w, qstr_n);
                char qba_n[32] = {0};
                s_pfn_toUtf8(qstr_n, qba_n);
                const char* oname = s_pfn_constData(qba_n);
                if (oname) strncpy(obj_name_buf, oname, sizeof(obj_name_buf) - 1);
                if (s_pfn_qba_dtor) s_pfn_qba_dtor(qba_n);
                s_pfn_dtor(qstr_n);
            }

            // Modern Home List View on FritchiiTile
            if (strcmp(obj_name_buf, "FritchiiTile") == 0) {
                int cur_min_w = s_pfn_minimumWidth ? s_pfn_minimumWidth(w) : 0; // Read BEFORE setMinimumWidth!
                void* parent = s_pfn_parentWidget ? s_pfn_parentWidget(w) : NULL;
                void* layout = (parent && s_pfn_layout) ? s_pfn_layout(parent) : NULL;
                if (layout && s_pfn_setDirection) {
                    s_pfn_setDirection(layout, s_modern_home_list ? 2 : 0);
                }

                if (s_modern_home_list) {
                    if (s_pfn_setToolButtonStyle) s_pfn_setToolButtonStyle(w, 2);
                    if (s_pfn_setIconSize) {
                        struct { int w, h; } sz = {56, 56};
                        s_pfn_setIconSize(w, &sz);
                    }
                    if (s_pfn_setMinimumHeight) s_pfn_setMinimumHeight(w, 68);
                    if (s_pfn_setMinimumWidth) s_pfn_setMinimumWidth(w, 720);
                    if (s_pfn_setMaximumWidth) s_pfn_setMaximumWidth(w, 16777215);

                    if (s_pfn_toolTip && s_pfn_btn_setText) {
                        char q_tip[32] = {0};
                        s_pfn_toolTip(w, q_tip);
                        char q_ba[32] = {0};
                        s_pfn_toUtf8(q_tip, q_ba);
                        const char* full_title = s_pfn_constData(q_ba);
                        if (full_title && full_title[0] != '\0') {
                            char desc_line[512] = {0};
                            for (int m = 0; m < s_game_meta_count; m++) {
                                if (strcmp(s_game_meta[m].title, full_title) == 0) {
                                    char parts[4][128] = {0};
                                    int pcount = 0;
                                    if (s_game_meta[m].file_type[0] != '\0') {
                                        strncpy(parts[pcount++], s_game_meta[m].file_type, 127);
                                    }
                                    if (s_game_meta[m].file_size[0] != '\0') {
                                        strncpy(parts[pcount++], s_game_meta[m].file_size, 127);
                                    }
                                    if (s_game_meta[m].play_time[0] != '\0') {
                                        snprintf(parts[pcount++], 127, g_lang_vn ? "Giờ chơi: %s" : "Play time: %s", s_game_meta[m].play_time);
                                    }
                                    if (s_game_meta[m].add_ons[0] != '\0') {
                                        strncpy(parts[pcount++], s_game_meta[m].add_ons, 127);
                                    }
                                    for (int pi = 0; pi < pcount; pi++) {
                                        if (pi > 0) strncat(desc_line, "  •  ", sizeof(desc_line) - strlen(desc_line) - 1);
                                        strncat(desc_line, parts[pi], sizeof(desc_line) - strlen(desc_line) - 1);
                                    }
                                    break;
                                }
                            }

                            char combined[1024];
                            if (desc_line[0] != '\0') {
                                snprintf(combined, sizeof(combined), "%s\n%s", full_title, desc_line);
                            } else {
                                strncpy(combined, full_title, sizeof(combined) - 1);
                                combined[sizeof(combined) - 1] = '\0';
                            }

                            char q_new[32] = {0};
                            s_pfn_fromUtf8_str(q_new, combined, -1);
                            s_pfn_btn_setText(w, q_new);
                            s_pfn_dtor(q_new);
                        }
                        if (s_pfn_qba_dtor) s_pfn_qba_dtor(q_ba);
                        s_pfn_dtor(q_tip);
                    }

                    if (force || lang_changed || mode_changed || cur_min_w < 600) {
                        if (s_pfn_setStyleSheet) {
                            const char* ss = "QToolButton#FritchiiTile { background: #1a1a1e; border: 1px solid #2a2a30; border-radius: 10px; padding: 10px 16px; text-align: left; font-size: 14px; font-weight: 600; color: #f4f4f5; } QToolButton#FritchiiTile:hover { background: #26262e; border-color: #8b5cf6; color: #ffffff; }";
                            char q_ss[32] = {0};
                            s_pfn_fromUtf8_str(q_ss, ss, -1);
                            s_pfn_setStyleSheet(w, q_ss);
                            s_pfn_dtor(q_ss);
                        }
                    }
                } else {
                    if (s_pfn_setToolButtonStyle) s_pfn_setToolButtonStyle(w, 3);
                    if (s_pfn_setIconSize) {
                        struct { int w, h; } sz = {72, 72};
                        s_pfn_setIconSize(w, &sz);
                    }
                    if (s_pfn_setMinimumHeight) s_pfn_setMinimumHeight(w, 0);
                    if (s_pfn_setMinimumWidth) s_pfn_setMinimumWidth(w, 0);
                    if (s_pfn_setMaximumWidth) s_pfn_setMaximumWidth(w, 16777215);
                    if (force || lang_changed || mode_changed) {
                        if (s_pfn_setStyleSheet) {
                            char q_ss[32] = {0};
                            s_pfn_fromUtf8_str(q_ss, "", -1);
                            s_pfn_setStyleSheet(w, q_ss);
                            s_pfn_dtor(q_ss);
                        }
                    }
                }
                continue;
            }

            // Pochita Dialog Logo Replacement (always check on every timer tick)
            if (strcmp(obj_name_buf, "labelLogo") == 0) {
                if (!g_hChainsaw) {
                    HMODULE hExe = GetModuleHandleW(NULL);
                    g_hChainsaw = (HICON)LoadImageW(hExe, L"IDI_ICON1", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
                    if (!g_hChainsaw) g_hChainsaw = (HICON)LoadImageW(hExe, MAKEINTRESOURCEW(568), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
                    if (!g_hChainsaw) g_hChainsaw = (HICON)LoadImageW(hExe, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
                }
                static int s_pochita_pix_inited = 0;
                static char s_pochita_pix[64] = {0};
                if (!s_pochita_pix_inited && g_hChainsaw && s_pfn_pixmapFromWinHICON) {
                    s_pfn_pixmapFromWinHICON(s_pochita_pix, g_hChainsaw);
                    s_pochita_pix_inited = 1;
                }
                if (s_pochita_pix_inited && s_pfn_setPixmap) {
                    s_pfn_setPixmap(w, s_pochita_pix);
                }
                continue;
            }

            if (strcmp(obj_name_buf, "labelYuzu") == 0) {
                if (s_pfn_lbl_setText) {
                    char q_yz[32] = {0};
                    s_pfn_fromUtf8_str(q_yz, "Yngchii-X3D", -1);
                    s_pfn_lbl_setText(w, q_yz);
                    s_pfn_dtor(q_yz);
                }
                continue;
            }

            // Context Menu Actions (always check on every timer tick so right-click popups translate immediately)
            if (s_pfn_actions && s_pfn_act_text && s_pfn_act_setText && s_pfn_inherits(w, "QMenu")) {
                struct {
                    void* d;
                    void** ptr;
                    int64_t size;
                } act_list;
                memset(&act_list, 0, sizeof(act_list));
                s_pfn_actions(w, &act_list);
                if (act_list.ptr && act_list.size > 0) {
                    for (int64_t a = 0; a < act_list.size; a++) {
                        void* act = act_list.ptr[a];
                        if (!act) continue;
                        char act_qstr[32] = {0};
                        s_pfn_act_text(act, act_qstr);
                        char act_qba[32] = {0};
                        s_pfn_toUtf8(act_qstr, act_qba);
                        const char* act_text = s_pfn_constData(act_qba);
                        if (act_text && act_text[0] != '\0') {
                            const char* act_target = TranslateTextBidirectional(act_text, g_lang_vn);
                            if (act_target && strcmp(act_text, act_target) != 0) {
                                char new_act_qstr[32] = {0};
                                s_pfn_fromUtf8_str(new_act_qstr, act_target, -1);
                                s_pfn_act_setText(act, new_act_qstr);
                                s_pfn_dtor(new_act_qstr);
                            }
                        }
                        if (s_pfn_qba_dtor) s_pfn_qba_dtor(act_qba);
                        s_pfn_dtor(act_qstr);
                    }
                }
                continue;
            }

            // QTreeView game metadata extraction
            if (s_pfn_inherits(w, "QTreeView") && s_pfn_view_model && s_pfn_model_root) {
                void* model = s_pfn_view_model(w);
                if (model && s_pfn_inherits(model, "QStandardItemModel")) {
                    void* root = s_pfn_model_root(model);
                    if (root && s_pfn_qitem_rowCount && s_pfn_qitem_child) {
                        int top_rows = s_pfn_qitem_rowCount(root);
                        s_game_meta_count = 0;
                        for (int r = 0; r < top_rows && s_game_meta_count < 128; r++) {
                            void* top_item = s_pfn_qitem_child(root, r, 0);
                            if (top_item) {
                                int sub_rows = s_pfn_qitem_rowCount(top_item);
                                if (sub_rows > 0) {
                                    for (int sub = 0; sub < sub_rows && s_game_meta_count < 128; sub++) {
                                        void* game_item = s_pfn_qitem_child(top_item, sub, 0);
                                        if (game_item) {
                                            char t[256] = {0};
                                            HelperGetItemText(game_item, t, sizeof(t));
                                            if (t[0] != '\0') {
                                                GameMetaCache* entry = &s_game_meta[s_game_meta_count++];
                                                strncpy(entry->title, t, sizeof(entry->title) - 1);
                                                HelperGetItemText(s_pfn_qitem_child(top_item, sub, 1), entry->file_type, sizeof(entry->file_type));
                                                HelperGetItemText(s_pfn_qitem_child(top_item, sub, 2), entry->file_size, sizeof(entry->file_size));
                                                HelperGetItemText(s_pfn_qitem_child(top_item, sub, 3), entry->play_time, sizeof(entry->play_time));
                                                HelperGetItemText(s_pfn_qitem_child(top_item, sub, 4), entry->add_ons, sizeof(entry->add_ons));
                                            }
                                        }
                                    }
                                } else {
                                    char t[256] = {0};
                                    HelperGetItemText(top_item, t, sizeof(t));
                                    if (t[0] != '\0') {
                                        GameMetaCache* entry = &s_game_meta[s_game_meta_count++];
                                        strncpy(entry->title, t, sizeof(entry->title) - 1);
                                        if (s_pfn_model_item) {
                                            HelperGetItemText(s_pfn_model_item(model, r, 1), entry->file_type, sizeof(entry->file_type));
                                            HelperGetItemText(s_pfn_model_item(model, r, 2), entry->file_size, sizeof(entry->file_size));
                                            HelperGetItemText(s_pfn_model_item(model, r, 3), entry->play_time, sizeof(entry->play_time));
                                            HelperGetItemText(s_pfn_model_item(model, r, 4), entry->add_ons, sizeof(entry->add_ons));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                continue;
            }

            // For all other static widgets: skip if language did not change and not forced
            if (!force && !lang_changed) {
                continue;
            }

            // 1. Handle QTabBar tabs
            if (s_pfn_tab_count && s_pfn_tab_text && s_pfn_tab_setText && s_pfn_inherits(w, "QTabBar")) {
                int count = s_pfn_tab_count(w);
                for (int t = 0; t < count; t++) {
                    char qstr_buf[32] = {0};
                    s_pfn_tab_text(w, qstr_buf, t);
                    char qba_buf[32] = {0};
                    s_pfn_toUtf8(qstr_buf, qba_buf);
                    const char* cur_text = s_pfn_constData(qba_buf);
                    if (cur_text && cur_text[0] != '\0') {
                        const char* target_text = TranslateTextBidirectional(cur_text, g_lang_vn);
                        if (target_text && strcmp(cur_text, target_text) != 0) {
                            char new_qstr_buf[32] = {0};
                            s_pfn_fromUtf8_str(new_qstr_buf, target_text, -1);
                            s_pfn_tab_setText(w, t, new_qstr_buf);
                            s_pfn_dtor(new_qstr_buf);
                        }
                    }
                    if (s_pfn_qba_dtor) s_pfn_qba_dtor(qba_buf);
                    s_pfn_dtor(qstr_buf);
                }
                continue;
            }

            // 2. Buttons
            if (s_pfn_btn_text && s_pfn_btn_setText && (s_pfn_inherits(w, "QPushButton") || s_pfn_inherits(w, "QToolButton") || s_pfn_inherits(w, "QRadioButton") || s_pfn_inherits(w, "QCheckBox"))) {
                char qstr_buf[32] = {0};
                s_pfn_btn_text(w, qstr_buf);
                char qba_buf[32] = {0};
                s_pfn_toUtf8(qstr_buf, qba_buf);
                const char* cur_text = s_pfn_constData(qba_buf);
                if (cur_text && cur_text[0] != '\0') {
                    const char* target_text = TranslateTextBidirectional(cur_text, g_lang_vn);
                    if (target_text && strcmp(cur_text, target_text) != 0) {
                        char new_qstr_buf[32] = {0};
                        s_pfn_fromUtf8_str(new_qstr_buf, target_text, -1);
                        s_pfn_btn_setText(w, new_qstr_buf);
                        s_pfn_dtor(new_qstr_buf);
                    }
                }
                if (s_pfn_qba_dtor) s_pfn_qba_dtor(qba_buf);
                s_pfn_dtor(qstr_buf);
                continue;
            }

            // 3. Labels
            if (s_pfn_lbl_text && s_pfn_lbl_setText && s_pfn_inherits(w, "QLabel")) {
                char qstr_buf[32] = {0};
                s_pfn_lbl_text(w, qstr_buf);
                char qba_buf[32] = {0};
                s_pfn_toUtf8(qstr_buf, qba_buf);
                const char* cur_text = s_pfn_constData(qba_buf);
                if (cur_text && cur_text[0] != '\0') {
                    const char* target_text = TranslateTextBidirectional(cur_text, g_lang_vn);
                    if (target_text && strcmp(cur_text, target_text) != 0) {
                        char new_qstr_buf[32] = {0};
                        s_pfn_fromUtf8_str(new_qstr_buf, target_text, -1);
                        s_pfn_lbl_setText(w, new_qstr_buf);
                        s_pfn_dtor(new_qstr_buf);
                    }
                }
                if (s_pfn_qba_dtor) s_pfn_qba_dtor(qba_buf);
                s_pfn_dtor(qstr_buf);
                continue;
            }

            // 4. GroupBoxes
            if (s_pfn_gb_title && s_pfn_gb_setTitle && s_pfn_inherits(w, "QGroupBox")) {
                char qstr_buf[32] = {0};
                s_pfn_gb_title(w, qstr_buf);
                char qba_buf[32] = {0};
                s_pfn_toUtf8(qstr_buf, qba_buf);
                const char* cur_text = s_pfn_constData(qba_buf);
                if (cur_text && cur_text[0] != '\0') {
                    const char* target_text = TranslateTextBidirectional(cur_text, g_lang_vn);
                    if (target_text && strcmp(cur_text, target_text) != 0) {
                        char new_qstr_buf[32] = {0};
                        s_pfn_fromUtf8_str(new_qstr_buf, target_text, -1);
                        s_pfn_gb_setTitle(w, new_qstr_buf);
                        s_pfn_dtor(new_qstr_buf);
                    }
                }
                if (s_pfn_qba_dtor) s_pfn_qba_dtor(qba_buf);
                s_pfn_dtor(qstr_buf);
                continue;
            }
        }
    }

    s_in_translation = 0;
}

static void ApplyLanguageState(HWND hBtnWnd, int new_vn) {
    if (g_lang_vn != new_vn) {
        g_lang_vn = new_vn;
        SaveLanguagePreference(g_lang_vn);
        if (s_hwndMain && IsWindow(s_hwndMain)) {
            PostMessageW(s_hwndMain, WM_YNGCHII_LANG_TOGGLE, 0, 0);
        } else {
            TriggerLiveQtWidgetTranslation(1);
        }
    }
    SetTimer(hBtnWnd, TIMER_ID_ANIM, 15, NULL);
    InvalidateRect(hBtnWnd, NULL, FALSE);
}

static LRESULT CALLBACK LangBtnWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdcScreen = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            HDC hdc = CreateCompatibleDC(hdcScreen);
            HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, w, h);
            HGDIOBJ oldBm = SelectObject(hdc, hbm);

            // Clear outer background
            HBRUSH sidebarBrush = CreateSolidBrush(RGB(12, 12, 14));
            FillRect(hdc, &rc, sidebarBrush);
            DeleteObject(sidebarBrush);

            // Track
            int is_vn_visual = (s_knob_pos >= 0.5f) ? 1 : 0;
            COLORREF trackBg = is_vn_visual ? RGB(31, 24, 54) : RGB(24, 24, 28);
            COLORREF trackBorder = is_vn_visual ? RGB(109, 82, 205) : RGB(56, 56, 64);

            HBRUSH trackBrush = CreateSolidBrush(trackBg);
            HPEN trackPen = CreatePen(PS_SOLID, 1, trackBorder);
            HGDIOBJ oldBr = SelectObject(hdc, trackBrush);
            HGDIOBJ oldPn = SelectObject(hdc, trackPen);

            int corner = w / 2;
            if (corner > 14) corner = 14;
            RoundRect(hdc, 0, 0, w, h, corner, corner);

            SelectObject(hdc, oldBr);
            SelectObject(hdc, oldPn);
            DeleteObject(trackBrush);
            DeleteObject(trackPen);

            // Labels
            SetBkMode(hdc, TRANSPARENT);
            int fontSize = (w * 36) / 100;
            if (fontSize < 10) fontSize = 10;
            HFONT hFontBg = CreateFontW(-fontSize, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT hFontKnob = CreateFontW(-(fontSize + 1), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

            HGDIOBJ oldFont = SelectObject(hdc, hFontBg);
            SetTextColor(hdc, RGB(110, 110, 122));

            RECT rcTop = {0, 2, w, h / 2};
            RECT rcBot = {0, h / 2, w, h - 2};
            DrawTextW(hdc, L"EN", -1, &rcTop, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(hdc, L"VN", -1, &rcBot, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Sliding knob
            int pad = 3;
            int knobW = w - pad * 2;
            int knobH = (h - pad * 2) / 2;
            int travel = (h - pad * 2) - knobH;
            int knobY = pad + (int)(s_knob_pos * (float)travel + 0.5f);

            COLORREF knobBg = is_vn_visual ? RGB(139, 92, 246) : RGB(63, 63, 72);
            COLORREF knobBorder = is_vn_visual ? RGB(196, 181, 253) : RGB(113, 113, 126);

            HBRUSH knobBrush = CreateSolidBrush(knobBg);
            HPEN knobPen = CreatePen(PS_SOLID, 1, knobBorder);
            SelectObject(hdc, knobBrush);
            SelectObject(hdc, knobPen);

            int knobCorner = corner - 2;
            if (knobCorner < 6) knobCorner = 6;
            RoundRect(hdc, pad, knobY, pad + knobW, knobY + knobH, knobCorner, knobCorner);

            SelectObject(hdc, oldBr);
            SelectObject(hdc, oldPn);
            DeleteObject(knobBrush);
            DeleteObject(knobPen);

            // Active text
            SelectObject(hdc, hFontKnob);
            SetTextColor(hdc, RGB(255, 255, 255));
            RECT rcKnob = {pad, knobY, pad + knobW, knobY + knobH};
            DrawTextW(hdc, is_vn_visual ? L"VN" : L"EN", -1, &rcKnob, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            BitBlt(hdcScreen, 0, 0, w, h, hdc, 0, 0, SRCCOPY);

            SelectObject(hdc, oldFont);
            DeleteObject(hFontBg);
            DeleteObject(hFontKnob);
            SelectObject(hdc, oldBm);
            DeleteObject(hbm);
            DeleteDC(hdc);

            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetCapture(hWnd);
            s_is_dragging = 1;
            s_drag_moved = 0;
            s_drag_start_y = GET_Y_LPARAM(lParam);
            s_drag_start_pos = s_knob_pos;
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (s_is_dragging) {
                int curY = GET_Y_LPARAM(lParam);
                int deltaY = curY - s_drag_start_y;
                if (abs(deltaY) > 3) s_drag_moved = 1;

                RECT rc;
                GetClientRect(hWnd, &rc);
                int h = rc.bottom - rc.top;
                int pad = 3;
                int knobH = (h - pad * 2) / 2;
                int travel = (h - pad * 2) - knobH;
                if (travel > 0) {
                    float new_pos = s_drag_start_pos + ((float)deltaY / (float)travel);
                    if (new_pos < 0.0f) new_pos = 0.0f;
                    if (new_pos > 1.0f) new_pos = 1.0f;
                    s_knob_pos = new_pos;
                }

                int target_vn = (s_knob_pos >= 0.5f) ? 1 : 0;
                if (target_vn != g_lang_vn) {
                    g_lang_vn = target_vn;
                    SaveLanguagePreference(g_lang_vn);
                    TriggerQtRetranslate();
                    TriggerLiveQtWidgetTranslation(0);
                }
                InvalidateRect(hWnd, NULL, FALSE);
                UpdateWindow(hWnd);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (s_is_dragging) {
                ReleaseCapture();
                s_is_dragging = 0;
                int target_vn;
                if (s_drag_moved) {
                    target_vn = (s_knob_pos >= 0.5f) ? 1 : 0;
                } else {
                    RECT rc;
                    GetClientRect(hWnd, &rc);
                    int h = rc.bottom - rc.top;
                    int clickY = GET_Y_LPARAM(lParam);
                    int clicked_half = (clickY >= h / 2) ? 1 : 0;
                    target_vn = (clicked_half != g_lang_vn) ? clicked_half : !g_lang_vn;
                }
                ApplyLanguageState(hWnd, target_vn);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0) {
                ApplyLanguageState(hWnd, 0); // Up -> EN
            } else if (delta < 0) {
                ApplyLanguageState(hWnd, 1); // Down -> VN
            }
            return 0;
        }
        case WM_TIMER: {
            if (wParam == TIMER_ID_ANIM) {
                float target = g_lang_vn ? 1.0f : 0.0f;
                float diff = target - s_knob_pos;
                if (diff > -0.05f && diff < 0.05f) {
                    s_knob_pos = target;
                    KillTimer(hWnd, TIMER_ID_ANIM);
                } else {
                    s_knob_pos += diff * 0.45f;
                }
                InvalidateRect(hWnd, NULL, FALSE);
                UpdateWindow(hWnd);
                return 0;
            }
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static void GetToggleSwitchGeometry(HWND hMainWnd, int* out_x, int* out_y, int* out_w, int* out_h) {
    typedef UINT (WINAPI *pfnGetDpiForWindow)(HWND);
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    pfnGetDpiForWindow pGetDpi = (pfnGetDpiForWindow)GetProcAddress(hUser32, "GetDpiForWindow");
    UINT dpi = pGetDpi ? pGetDpi(hMainWnd) : 96;
    if (!dpi) dpi = 96;

    // Horizontally center with sidebar buttons at X = 28 (x = 15, w = 26 at 96 DPI)
    int centerX = (28 * (int)dpi) / 96;
    int bw = (26 * (int)dpi) / 96;
    int bh = (52 * (int)dpi) / 96;
    int bx = centerX - (bw / 2);
    int by = (200 * (int)dpi) / 96;

    *out_x = bx;
    *out_y = by;
    *out_w = bw;
    *out_h = bh;
}

static LRESULT CALLBACK MainWndProcHook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_YNGCHII_LANG_TOGGLE || uMsg == WM_YNGCHII_REFRESH_VIEW) {
        TriggerLiveQtWidgetTranslation(1);
        return 0;
    }

    if (uMsg == WM_YNGCHII_INIT_BTN) {
        s_is_in_game = IsInGameWindow(hWnd);
        if (s_is_in_game) {
            if (s_hwndLangBtn && IsWindow(s_hwndLangBtn) && IsWindowVisible(s_hwndLangBtn)) {
                ShowWindow(s_hwndLangBtn, SW_HIDE);
            }
            LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
            if (style & WS_CLIPCHILDREN) {
                SetWindowLongPtrW(hWnd, GWL_STYLE, style & ~WS_CLIPCHILDREN);
            }
            return 0;
        }

        LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
        if (!(style & WS_CLIPCHILDREN)) {
            SetWindowLongPtrW(hWnd, GWL_STYLE, style | WS_CLIPCHILDREN);
        }

        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = LangBtnWndProc;
        wc.hInstance = g_hInst;
        wc.lpszClassName = L"YngchiiLangSwitch";
        wc.hCursor = LoadCursor(NULL, IDC_HAND);
        RegisterClassExW(&wc);

        int bx, by, bw, bh;
        GetToggleSwitchGeometry(hWnd, &bx, &by, &bw, &bh);

        s_knob_pos = g_lang_vn ? 1.0f : 0.0f;

        if (!s_hwndLangBtn || !IsWindow(s_hwndLangBtn)) {
            s_hwndLangBtn = CreateWindowExW(
                0,
                L"YngchiiLangSwitch",
                L"EN/VN",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                bx, by, bw, bh,
                hWnd,
                (HMENU)0x9876,
                g_hInst,
                NULL
            );
            if (s_hwndLangBtn) {
                SetWindowPos(s_hwndLangBtn, HWND_TOP, bx, by, bw, bh, SWP_SHOWWINDOW);
                InvalidateRect(s_hwndLangBtn, NULL, FALSE);
                UpdateWindow(s_hwndLangBtn);
            }
        }

        SetTimer(hWnd, TIMER_ID_YNGCHII, 250, NULL);
        return 0;
    }

    if (uMsg == WM_SIZE) {
        s_is_in_game = IsInGameWindow(hWnd);
        if (s_is_in_game) {
            if (s_hwndLangBtn && IsWindow(s_hwndLangBtn) && IsWindowVisible(s_hwndLangBtn)) {
                ShowWindow(s_hwndLangBtn, SW_HIDE);
            }
            LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
            if (style & WS_CLIPCHILDREN) {
                SetWindowLongPtrW(hWnd, GWL_STYLE, style & ~WS_CLIPCHILDREN);
            }
        } else {
            LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
            if (!(style & WS_CLIPCHILDREN)) {
                SetWindowLongPtrW(hWnd, GWL_STYLE, style | WS_CLIPCHILDREN);
            }
            if (s_hwndLangBtn && IsWindow(s_hwndLangBtn)) {
                int bx, by, bw, bh;
                GetToggleSwitchGeometry(hWnd, &bx, &by, &bw, &bh);
                SetWindowPos(s_hwndLangBtn, HWND_TOP, bx, by, bw, bh, SWP_NOACTIVATE);
                if (!IsWindowVisible(s_hwndLangBtn)) {
                    ShowWindow(s_hwndLangBtn, SW_SHOW);
                }
            }
        }
    }

    if (uMsg == WM_TIMER && wParam == TIMER_ID_YNGCHII) {
        s_is_in_game = IsInGameWindow(hWnd);
        if (s_is_in_game) {
            if (s_hwndLangBtn && IsWindow(s_hwndLangBtn) && IsWindowVisible(s_hwndLangBtn)) {
                ShowWindow(s_hwndLangBtn, SW_HIDE);
            }
            LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
            if (style & WS_CLIPCHILDREN) {
                SetWindowLongPtrW(hWnd, GWL_STYLE, style & ~WS_CLIPCHILDREN);
            }
        } else {
            LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
            if (!(style & WS_CLIPCHILDREN)) {
                SetWindowLongPtrW(hWnd, GWL_STYLE, style | WS_CLIPCHILDREN);
            }
            if (s_hwndLangBtn && IsWindow(s_hwndLangBtn)) {
                int bx, by, bw, bh;
                GetToggleSwitchGeometry(hWnd, &bx, &by, &bw, &bh);
                SetWindowPos(s_hwndLangBtn, HWND_TOP, bx, by, bw, bh, SWP_NOACTIVATE);
                if (!IsWindowVisible(s_hwndLangBtn)) {
                    ShowWindow(s_hwndLangBtn, SW_SHOW);
                }
            }
            TriggerLiveQtWidgetTranslation(0);
        }
        return 0;
    }

    if (uMsg == WM_KEYDOWN) {
        if (wParam == VK_F8 || (wParam == 'G' && (GetKeyState(VK_CONTROL) & 0x8000))) {
            s_modern_home_list = !s_modern_home_list;
            TriggerLiveQtWidgetTranslation(0);
            return 0;
        }
        if (wParam == VK_F9 || (wParam == 'L' && (GetKeyState(VK_CONTROL) & 0x8000))) {
            if (s_hwndLangBtn && IsWindow(s_hwndLangBtn) && !s_is_in_game) {
                ApplyLanguageState(s_hwndLangBtn, !g_lang_vn);
            }
            return 0;
        }
    }

    return CallWindowProcW(s_origMainWndProc, hWnd, uMsg, wParam, lParam);
}

// ==============================================================================
// 5. HARDWARE INITIALIZATION & ENVIRONMENT SETUP
// ==============================================================================
static void SetEnvPair(const char* name, const char* value) {
    _putenv_s(name, value);
    SetEnvironmentVariableA(name, value);
}

static void LoadNx2SwitchesFile(const char* filepath) {
    FILE* f = fopen(filepath, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') continue;
        char* eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            char* val = eq + 1;
            char* end_k = eq - 1;
            while (end_k >= p && (*end_k == ' ' || *end_k == '\t')) *end_k-- = '\0';
            char* end_v = val + strlen(val) - 1;
            while (end_v >= val && (*end_v == ' ' || *end_v == '\t' || *end_v == '\r' || *end_v == '\n')) *end_v-- = '\0';
            if (strlen(p) > 0) {
                SetEnvPair(p, val);
            }
        }
    }
    fclose(f);
}

static void InitializeStep7KernelTimerAndQoS(ULONG* pActualRes) {
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) {
        pfnSetProcessInformation pSetProcessInformation = (pfnSetProcessInformation)GetProcAddress(hK32, "SetProcessInformation");
        if (pSetProcessInformation) {
            MY_PROCESS_POWER_THROTTLING_STATE ppt = {0};
            ppt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            ppt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
            ppt.StateMask = 0;
            pSetProcessInformation(GetCurrentProcess(), MyProcessPowerThrottling, &ppt, sizeof(ppt));
        }
    }

    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    timeBeginPeriod(1);

    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        PFN_NtSetTimerResolution pNtSetTimerResolution = (PFN_NtSetTimerResolution)GetProcAddress(hNtdll, "NtSetTimerResolution");
        if (pNtSetTimerResolution) {
            ULONG actual = 0;
            pNtSetTimerResolution(5000, TRUE, &actual);
            if (pActualRes) {
                *pActualRes = actual;
            }
        }
    }
}

static void LogActiveStepSwitches(const char* step_tag, ULONG actual_res) {
    char appdata[MAX_PATH] = {0};
    char log_path[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
        snprintf(log_path, sizeof(log_path), "%s\\eden\\log\\yngchii_step_audit.log", appdata);
    } else {
        snprintf(log_path, sizeof(log_path), "%s", "C:\\Users\\Vinha\\AppData\\Roaming\\eden\\log\\yngchii_step_audit.log");
    }

    FILE* fp = fopen(log_path, "a");
    if (!fp) return;

    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(fp, "================================================================================\n");
    fprintf(fp, "[%s] YNGCHII-X3D STEP RUN AUDIT: %s\n", time_str, step_tag);
    fprintf(fp, "CPU Hardware Detection:\n");
    fprintf(fp, "  Vendor: %s\n", g_cpu_vendor);
    fprintf(fp, "  Brand: %s\n", g_cpu_brand);
    fprintf(fp, "  Display Family: 0x%X (%u)\n", g_cpu_display_family, g_cpu_display_family);
    fprintf(fp, "  Total Logical Threads: %lu\n", g_total_threads);
    fprintf(fp, "  Optimization Status: %s\n", g_is_supported_ryzen ? "ENABLED (Zen 3/4/5 Ryzen 5/7/9)" : "SAFE FALLBACK (Unsupported CPU - Optimizations Bypassed)");
    fprintf(fp, "  Language Setting: %s\n", g_lang_vn ? "Vietnamese (VN)" : "English (EN)");
    if (g_is_supported_ryzen) {
        fprintf(fp, "  Kernel Timer Resolution: %lu units (0.5ms = 5000 units)\n", actual_res);
        fprintf(fp, "  Process Priority Class: NORMAL_PRIORITY_CLASS (32)\n");
    }
    fprintf(fp, "================================================================================\n\n");
    fclose(fp);
}

// ==============================================================================
// 6. OPTIMIZED PRESET AUTO-APPLICATION (ENHANCEMENT 3)
// ==============================================================================
static void ApplyOptimizedPresetIfNeeded(void) {
    char appdata[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) <= 0) return;

    char stamp_path[MAX_PATH];
    snprintf(stamp_path, sizeof(stamp_path), "%s\\eden\\config\\yngchii_preset_v56_applied.stamp", appdata);

    FILE* f_stamp = fopen(stamp_path, "r");
    if (f_stamp) {
        fclose(f_stamp);
        return; // Already applied
    }

    char exe_dir[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exe_dir, MAX_PATH) <= 0) return;
    char* slash = strrchr(exe_dir, '\\');
    if (slash) *slash = '\0';

    char preset_qt[MAX_PATH];
    snprintf(preset_qt, sizeof(preset_qt), "%s\\config_preset\\qt-config.ini", exe_dir);

    char dest_qt[MAX_PATH];
    snprintf(dest_qt, sizeof(dest_qt), "%s\\eden\\config\\qt-config.ini", appdata);

    FILE* f_src = fopen(preset_qt, "r");
    if (f_src) {
        // Read existing gamedirs from dest_qt to preserve user's game library
        char gamedirs_buf[4096] = {0};
        size_t gamedirs_len = 0;

        FILE* f_old = fopen(dest_qt, "r");
        if (f_old) {
            char line[512];
            while (fgets(line, sizeof(line), f_old)) {
                if (strstr(line, "paths\\gamedirs") != NULL || strstr(line, "Paths\\gamedirs") != NULL) {
                    size_t l = strlen(line);
                    if (gamedirs_len + l < sizeof(gamedirs_buf) - 1) {
                        memcpy(gamedirs_buf + gamedirs_len, line, l);
                        gamedirs_len += l;
                        gamedirs_buf[gamedirs_len] = '\0';
                    }
                }
            }
            fclose(f_old);
        }

        // Write merged file
        char dest_dir[MAX_PATH];
        snprintf(dest_dir, sizeof(dest_dir), "%s\\eden\\config", appdata);
        CreateDirectoryA(dest_dir, NULL);

        FILE* f_out = fopen(dest_qt, "w");
        if (f_out) {
            char line[1024];
            int in_ui_sec = 0;
            int gamedirs_written = 0;

            while (fgets(line, sizeof(line), f_src)) {
                if (strncmp(line, "[UI]", 4) == 0) {
                    in_ui_sec = 1;
                    fputs(line, f_out);
                    if (gamedirs_len > 0 && !gamedirs_written) {
                        fputs(gamedirs_buf, f_out);
                        gamedirs_written = 1;
                    }
                    continue;
                }
                if (line[0] == '[' && strncmp(line, "[UI]", 4) != 0) {
                    in_ui_sec = 0;
                }
                if (in_ui_sec && gamedirs_len > 0 &&
                    (strstr(line, "paths\\gamedirs") != NULL || strstr(line, "Paths\\gamedirs") != NULL)) {
                    continue; // Skip template gamedirs, keep user's
                }
                fputs(line, f_out);
            }
            fclose(f_out);
        }
        fclose(f_src);
    }

    // Copy custom game config
    char preset_custom[MAX_PATH];
    snprintf(preset_custom, sizeof(preset_custom), "%s\\config_preset\\custom\\0400486024DEC000.ini", exe_dir);
    char dest_custom_dir[MAX_PATH];
    snprintf(dest_custom_dir, sizeof(dest_custom_dir), "%s\\eden\\config\\custom", appdata);
    CreateDirectoryA(dest_custom_dir, NULL);
    char dest_custom[MAX_PATH];
    snprintf(dest_custom, sizeof(dest_custom), "%s\\eden\\config\\custom\\0400486024DEC000.ini", appdata);

    CopyFileA(preset_custom, dest_custom, FALSE);

    // Write stamp
    FILE* f_out_stamp = fopen(stamp_path, "w");
    if (f_out_stamp) {
        fprintf(f_out_stamp, "Yngchii-X3D v5.6 preset applied successfully at %lld\n", (long long)time(NULL));
        fclose(f_out_stamp);
    }
}

// ==============================================================================
// 7. DISCORD RICH PRESENCE (ENHANCEMENT 5)
// ==============================================================================
static DWORD WINAPI DiscordRpcThread(LPVOID lpParam) {
    (void)lpParam;
    DWORD myPid = GetCurrentProcessId();
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    time_t session_start = time(NULL);
    time_t game_start = 0;
    int was_in_game = 0;

    while (1) {
        if (hPipe == INVALID_HANDLE_VALUE) {
            for (int i = 0; i < 10; i++) {
                wchar_t pipe_name[64];
                swprintf(pipe_name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
                hPipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
                if (hPipe != INVALID_HANDLE_VALUE) {
                    // Send Handshake
                    const char* hs = "{\"v\":1,\"client_id\":\"1569019855057715350\"}";
                    uint32_t op = 0;
                    uint32_t len = (uint32_t)strlen(hs);
                    DWORD written = 0;
                    WriteFile(hPipe, &op, 4, &written, NULL);
                    WriteFile(hPipe, &len, 4, &written, NULL);
                    WriteFile(hPipe, hs, len, &written, NULL);

                    uint32_t resp_op = 0, resp_len = 0;
                    DWORD read_bytes = 0;
                    if (ReadFile(hPipe, &resp_op, 4, &read_bytes, NULL) &&
                        ReadFile(hPipe, &resp_len, 4, &read_bytes, NULL)) {
                        char resp_buf[1024];
                        DWORD to_read = resp_len < 1023 ? resp_len : 1023;
                        ReadFile(hPipe, resp_buf, to_read, &read_bytes, NULL);
                    }
                    break;
                }
            }
        }

        if (hPipe != INVALID_HANDLE_VALUE) {
            if (s_is_in_game && !was_in_game) {
                game_start = time(NULL);
            }
            was_in_game = s_is_in_game;

            char payload[2048];
            time_t act_time = s_is_in_game ? (game_start ? game_start : session_start) : session_start;

            char details[256];
            char state[256];

            if (s_is_in_game) {
                if (s_game_title[0] != '\0') {
                    snprintf(details, sizeof(details), "Playing %s", s_game_title);
                } else {
                    snprintf(details, sizeof(details), "Playing in In-Game Session");
                }
                snprintf(state, sizeof(state), "⚡ Yngvi's X3D Optimize: Active");
            } else {
                snprintf(details, sizeof(details), "Idle in Home Menu");
                snprintf(state, sizeof(state), "⚡ Yngvi's X3D Optimize: Ready");
            }

            snprintf(payload, sizeof(payload),
                "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{"
                "\"details\":\"%s\","
                "\"state\":\"%s\","
                "\"timestamps\":{\"start\":%lld},"
                "\"assets\":{"
                "\"large_image\":\"pochita\","
                "\"large_text\":\"Yngchii-X3D v5.6\","
                "\"small_image\":\"pochita\","
                "\"small_text\":\"Ryzen X3D Edition\""
                "}}},\"nonce\":\"1\"}",
                myPid, details, state, (long long)act_time);

            uint32_t op = 1;
            uint32_t len = (uint32_t)strlen(payload);
            DWORD written = 0;
            BOOL b1 = WriteFile(hPipe, &op, 4, &written, NULL);
            BOOL b2 = WriteFile(hPipe, &len, 4, &written, NULL);
            BOOL b3 = WriteFile(hPipe, payload, len, &written, NULL);

            if (!b1 || !b2 || !b3) {
                CloseHandle(hPipe);
                hPipe = INVALID_HANDLE_VALUE;
            } else {
                DWORD avail = 0;
                if (PeekNamedPipe(hPipe, NULL, 0, NULL, &avail, NULL) && avail >= 8) {
                    uint32_t resp_op = 0, resp_len = 0;
                    DWORD read_bytes = 0;
                    ReadFile(hPipe, &resp_op, 4, &read_bytes, NULL);
                    ReadFile(hPipe, &resp_len, 4, &read_bytes, NULL);
                    char drain[2048];
                    while (resp_len > 0) {
                        DWORD chunk = resp_len > sizeof(drain) ? (DWORD)sizeof(drain) : resp_len;
                        ReadFile(hPipe, drain, chunk, &read_bytes, NULL);
                        resp_len -= read_bytes;
                        if (read_bytes == 0) break;
                    }
                }
            }
        }

        Sleep(2500);
    }
    return 0;
}

// ==============================================================================
// 8. MONITOR THREAD: WINDOW TITLE, ICON, EN/VN SWITCH, & THREAD PRIORITIES
// ==============================================================================
static DWORD WINAPI MonitorThread(LPVOID lpParam) {
    (void)lpParam;
    DWORD myPid = GetCurrentProcessId();
    HMODULE hExe = GetModuleHandleW(NULL);
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");

    if (g_is_supported_ryzen) {
        if (g_total_threads <= 12) {
            SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)0x0F00);
        } else {
            SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)0xFF00);
        }
    }

    if (!g_hChainsaw) {
        g_hChainsaw = (HICON)LoadImageW(hExe, L"IDI_ICON1", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        if (!g_hChainsaw) g_hChainsaw = (HICON)LoadImageW(hExe, MAKEINTRESOURCEW(568), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        if (!g_hChainsaw) g_hChainsaw = (HICON)LoadImageW(hExe, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    }
    HICON hChainsaw = g_hChainsaw;

    pfnSetThreadInformation pSetThreadInformation = (pfnSetThreadInformation)GetProcAddress(hK32, "SetThreadInformation");
    pfnGetThreadDescription pGetThreadDescription = (pfnGetThreadDescription)GetProcAddress(hK32, "GetThreadDescription");

    timeBeginPeriod(1);

    while (1) {
        Sleep(200);

        HWND hwnd = GetTopWindow(NULL);
        while (hwnd) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == myPid) {
                wchar_t title[512] = {0};
                int len = GetWindowTextW(hwnd, title, 512);
                wchar_t cls[128] = {0};
                GetClassNameW(hwnd, cls, 128);
                if (len > 0) {
                    wchar_t target_prefix[128];
                    if (g_is_supported_ryzen) {
                        wcscpy(target_prefix, L"Yngchii-X3D | v5.6");
                    } else {
                        wcscpy(target_prefix, L"Yngchii-X3D | v5.6 (Safe Mode - Non-Zen3/4/5)");
                    }

                    // Dependencies window title
                    if (wcsstr(title, L"Dependencies") != NULL) {
                        SetWindowTextW(hwnd, L"Yngchii-X3D Dependencies");
                        if (hChainsaw) {
                            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hChainsaw);
                            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hChainsaw);
                        }
                    }
                    if (wcsstr(title, L"About Fritchii") != NULL || wcsstr(title, L"About Eden") != NULL || wcsstr(title, L"About Lichii") != NULL) {
                        SetWindowTextW(hwnd, L"About Yngchii-X3D");
                        if (hChainsaw) {
                            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hChainsaw);
                            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hChainsaw);
                        }
                    }
                    if (wcscmp(title, L"Update") == 0) {
                        SetWindowTextW(hwnd, L"Yngchii-X3D Update");
                        if (hChainsaw) {
                            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hChainsaw);
                            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hChainsaw);
                        }
                    }
                    if (0 && wcsstr(title, L"Lichii Dependencies") != NULL) {
                        SetWindowTextW(hwnd, L"Yngchii-X3D Dependencies");
                        if (hChainsaw) {
                            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hChainsaw);
                            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hChainsaw);
                        }
                    }

                    // Main emulator window title
                    wchar_t* p = wcsstr(title, L"Fritchii | v5.6");
                    if (p != NULL) {
                        wchar_t new_title[512] = {0};
                        const wchar_t* suffix = p + wcslen(L"Fritchii | v5.6");
                        swprintf(new_title, 512, L"%ls%ls", target_prefix, suffix);
                        SetWindowTextW(hwnd, new_title);
                        if (hChainsaw) {
                            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hChainsaw);
                            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hChainsaw);
                        }
                        if (IsInGameWindow(hwnd)) {
                            s_is_in_game = 1;
                            if (s_hwndLangBtn && IsWindow(s_hwndLangBtn) && IsWindowVisible(s_hwndLangBtn)) {
                                ShowWindow(s_hwndLangBtn, SW_HIDE);
                            }
                            // Extract game title from suffix
                            // e.g. suffix = " | Clang ... | The Legend of Zelda: Ocarina of Time (64-bit) | Nvidia"
                            wchar_t* p_pipe3 = wcsstr(suffix, L" | ");
                            if (p_pipe3) {
                                wchar_t* p_pipe4 = wcsstr(p_pipe3 + 3, L" | ");
                                if (p_pipe4) {
                                    wchar_t* p_pipe5 = wcsstr(p_pipe4 + 3, L" | ");
                                    if (p_pipe5) {
                                        int name_len = (int)(p_pipe5 - (p_pipe4 + 3));
                                        if (name_len > 0 && name_len < 250) {
                                            wchar_t gname_w[256] = {0};
                                            wcsncpy(gname_w, p_pipe4 + 3, name_len);
                                            WideCharToMultiByte(CP_UTF8, 0, gname_w, -1, s_game_title, sizeof(s_game_title), NULL, NULL);
                                        }
                                    }
                                }
                            }
                        }
                    } else {
                        wchar_t* p2 = wcsstr(title, L"Fritchii | v5.");
                        if (p2 != NULL) {
                            wchar_t* next_pipe = wcsstr(p2 + 11, L" |");
                            if (next_pipe != NULL) {
                                wchar_t new_title[512] = {0};
                                swprintf(new_title, 512, L"%ls%ls", target_prefix, next_pipe);
                                SetWindowTextW(hwnd, new_title);
                                if (hChainsaw) {
                                    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hChainsaw);
                                    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hChainsaw);
                                }
                            }
                        }
                    }

                    if (!s_origMainWndProc && GetParent(hwnd) == NULL && wcsstr(cls, L"QWindowIcon") != NULL) {
                        s_hwndMain = hwnd;
                        s_origMainWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)MainWndProcHook);
                        PostMessageW(hwnd, WM_YNGCHII_INIT_BTN, 0, 0);
                    }
                }
            }
            hwnd = GetNextWindow(hwnd, GW_HWNDNEXT);
        }

        if (g_is_supported_ryzen && pGetThreadDescription) {
            HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hSnap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te;
                te.dwSize = sizeof(te);
                if (Thread32First(hSnap, &te)) {
                    do {
                        if (te.th32OwnerProcessID == myPid) {
                            HANDLE hThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION | THREAD_SET_INFORMATION, FALSE, te.th32ThreadID);
                            if (hThread) {
                                PWSTR pDesc = NULL;
                                if (SUCCEEDED(pGetThreadDescription(hThread, &pDesc)) && pDesc) {
                                    if (wcscmp(pDesc, L"GPU") == 0 || wcscmp(pDesc, L"GPUBack") == 0) {
                                        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
                                        if (pSetThreadInformation) {
                                            MY_THREAD_POWER_THROTTLING_STATE tpt = {1, 1, 0};
                                            pSetThreadInformation(hThread, MyThreadPowerThrottling, &tpt, sizeof(tpt));
                                        }
                                    } else if (wcscmp(pDesc, L"CPUCore_0") == 0 ||
                                               wcscmp(pDesc, L"CPUCore_1") == 0 ||
                                               wcscmp(pDesc, L"CPUCore_2") == 0 ||
                                               wcscmp(pDesc, L"CPUCore_3") == 0) {
                                        SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);
                                        if (pSetThreadInformation) {
                                            MY_THREAD_POWER_THROTTLING_STATE tpt = {1, 1, 0};
                                            pSetThreadInformation(hThread, MyThreadPowerThrottling, &tpt, sizeof(tpt));
                                        }
                                    } else if (wcscmp(pDesc, L"CPUCore_4") == 0 ||
                                               wcscmp(pDesc, L"CPUCore_5") == 0) {
                                        SetThreadPriority(hThread, THREAD_PRIORITY_NORMAL);
                                    } else if (wcsstr(pDesc, L"VkPipelineBuilder") != NULL ||
                                               wcsstr(pDesc, L"ShaderWarm") != NULL ||
                                               wcsstr(pDesc, L"Transcode") != NULL ||
                                               wcsstr(pDesc, L"ASTC") != NULL ||
                                               wcsstr(pDesc, L"nvoglv") != NULL) {
                                        SetThreadPriority(hThread, THREAD_PRIORITY_BELOW_NORMAL);
                                        DWORD_PTR shieldMask = (g_total_threads <= 12) ? (DWORD_PTR)0x0FC3 : (DWORD_PTR)0xFFC3;
                                        SetThreadAffinityMask(hThread, shieldMask);
                                    }
                                    LocalFree(pDesc);
                                }
                                CloseHandle(hThread);
                            }
                        }
                    } while (Thread32Next(hSnap, &te));
                }
                CloseHandle(hSnap);
            }
        }
    }
    return 0;
}

// ==============================================================================
// 9. DLL ENTRY POINT
// ==============================================================================
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        g_hInst = hinstDLL;

        DetectSupportedRyzenCpu();
        g_lang_vn = LoadLanguagePreference();
        InstallQt6IATHooks();

        // Environment variables for In-App Updater & What's New
        SetEnvPair("EDEN_UPDATE_URL", "https://api.github.com/repos/yngvifreyr1810/Yngchii-X3D/releases?per_page=100");

        char exe_path[MAX_PATH] = {0};
        if (GetModuleFileNameA(NULL, exe_path, MAX_PATH) > 0) {
            char* last_slash = strrchr(exe_path, '\\');
            if (last_slash) {
                *last_slash = '\0';
                char notes_path[MAX_PATH];
                snprintf(notes_path, MAX_PATH, "%s\\whats-new.md", exe_path);
                SetEnvPair("FRITCHII_NOTES_PREVIEW", notes_path);
            }
        }

        // Apply optimized preset on first launch
        ApplyOptimizedPresetIfNeeded();

        ULONG actual_res = 0;
        if (g_is_supported_ryzen) {
            SetEnvPair("VK_LOADER_LAYERS_DISABLE", "*NvCamera*,*NV_nomad*,*NVIDIA_Overlay*,*GalaxyOverlay*,*EOSOverlay*");
            SetEnvPair("NV_ALLOW_ANSEL", "0");
            SetEnvPair("ANSEL_SDK_DISABLE", "1");

            SetEnvPair("EDEN_NX2_IDLE_PAUSE", "1");
            SetEnvPair("EDEN_NX2_CORE_LOCK", "1");
            SetEnvPair("EDEN_NX2_IDLE_SPIN_US", "20");
            SetEnvPair("EDEN_NX2_PIN_THREADS", "1");
            SetEnvPair("EDEN_NX2_GUEST_PIN_EACH", "1");

            SetEnvPair("EDEN_NX2_OOT_POLL_BACKOFF", "1");
            SetEnvPair("EDEN_NX2_EXP25", "1");
            SetEnvPair("EDEN_NX2_EXP24", "0");

            SetEnvPair("EDEN_NX2_EXP12", "1");
            SetEnvPair("EDEN_NX2_EXP22", "7");
            SetEnvPair("EDEN_NX2_EXP26", "2");
            SetEnvPair("EDEN_NX2_EXP27", "5");
            SetEnvPair("EDEN_NX2_EXP30", "1");

            SetEnvPair("EDEN_NX2_EXP17", "0");
            SetEnvPair("EDEN_NX2_EXP18", "4");
            SetEnvPair("EDEN_NX2_EXP19", "0");
            SetEnvPair("EDEN_NX2_EXP29", "6");
            SetEnvPair("EDEN_NX2_EXP31", "0");
            SetEnvPair("EDEN_NX2_EXP32", "0");

            SetEnvPair("EDEN_NX2_EXP23", "0");
            SetEnvPair("EDEN_NX2_EXP37", "0");
            SetEnvPair("EDEN_NX2_ARB_SPIN", "0");
            SetEnvPair("EDEN_NX2_EXP60", "0");

            SetEnvPair("EDEN_NX2_EXP8", "15");
            SetEnvPair("EDEN_NX2_EXP9", "3");
            SetEnvPair("EDEN_NX2_EXP10", "1");
            SetEnvPair("EDEN_NX2_EXP14", "1");
            SetEnvPair("EDEN_NX2_EXP21", "1");
            SetEnvPair("EDEN_NX2_EXP34", "1");
            SetEnvPair("EDEN_NX2_EXP35", "2");
            SetEnvPair("EDEN_NX2_EXP39", "3");
            SetEnvPair("EDEN_NX2_EXP42", "1");
            SetEnvPair("EDEN_NX2_EXP43", "3");

            SetEnvPair("__GL_SHADER_DISK_CACHE", "1");
            SetEnvPair("__GL_SHADER_DISK_CACHE_SIZE", "10737418240");
            SetEnvPair("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1");

            if (exe_path[0] != '\0') {
                char switch_path[MAX_PATH];
                snprintf(switch_path, MAX_PATH, "%s\\nx2_switches.txt", exe_path);
                LoadNx2SwitchesFile(switch_path);
            }

            char appdata[MAX_PATH] = {0};
            if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
                char appdata_switch[MAX_PATH];
                snprintf(appdata_switch, MAX_PATH, "%s\\eden\\nx2_switches.txt", appdata);
                LoadNx2SwitchesFile(appdata_switch);
            }

            InitializeStep7KernelTimerAndQoS(&actual_res);
        }

        LogActiveStepSwitches("v5.6", actual_res);
        CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
        CreateThread(NULL, 0, DiscordRpcThread, NULL, 0, NULL);
    }
    return TRUE;
}
