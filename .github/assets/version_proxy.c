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
// 2. LANGUAGE PREFERENCE & HEX-ESCAPED UTF-8 VIETNAMESE DICTIONARY
// ==============================================================================
static volatile int g_lang_vn = 0; // 0 = English (Default), 1 = Vietnamese
static volatile int s_is_in_game = 0;
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

static void LogUiDebug(const char* format, ...) {
    char appdata[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s\\eden\\log", appdata);
        CreateDirectoryA(dir, NULL);
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\eden\\log\\yngchii_ui_debug.log", appdata);
        FILE* f = fopen(path, "a");
        if (f) {
            va_list args;
            va_start(args, format);
            vfprintf(f, format, args);
            va_end(args);
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
    {"\x50\x72\x65\x70\x61\x72\x69\x6E\x67\x20\x73\x68\x61\x64\x65\x72\x73", "\xC4\x90\x61\x6E\x67\x20\x63\x68\x75\xE1\xBA\xA9\x6E\x20\x62\xE1\xBB\x8B\x20\x53\x68\x61\x64\x65\x72\x73"}, // Preparing shaders
    {"\x42\x75\x69\x6C\x64\x69\x6E\x67\x20\x73\x68\x61\x64\x65\x72\x73", "\xC4\x90\x61\x6E\x67\x20\x64\xE1\xBB\xB1\x6E\x67\x20\x53\x68\x61\x64\x65\x72\x73"}, // Building shaders
    {"\x4C\x6F\x61\x64\x69\x6E\x67\x2E\x2E\x2E", "\xC4\x90\x61\x6E\x67\x20\x74\xE1\xBA\xA3\x69\x2E\x2E\x2E"}, // Loading...
    {"\x4C\x61\x75\x6E\x63\x68\x69\x6E\x67\x2E\x2E\x2E", "\xC4\x90\x61\x6E\x67\x20\x6B\x68\xE1\xBB\x9F\x69\x20\x63\x68\xE1\xBA\xA1\x79\x2E\x2E\x2E"}, // Launching...
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
    {"\x45\x78\x70\x6F\x72\x74\x20\x4C\x6F\x67", "\x58\x75\xE1\xBA\xA5\x74\x20\x66\x69\x6C\x65\x20\x4C\x6F\x67"}, // Export Log
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

static const char* LookupVietnamese(const char* en) {
    return TranslateTextBidirectional(en, 1);
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
        const char* vn = LookupVietnamese(sourceText);
        if (vn) {
            return s_orig_translate(ret, context, vn, disambiguation, n);
        }
    }
    return s_orig_translate(ret, context, sourceText, disambiguation, n);
}

static void* Hook_tr(void* ret, const void* metaObj, const char* sourceText, const char* disambiguation, int n) {
    if (!s_is_in_game && g_lang_vn && sourceText) {
        const char* vn = LookupVietnamese(sourceText);
        if (vn) {
            return s_orig_tr(ret, metaObj, vn, disambiguation, n);
        }
    }
    return s_orig_tr(ret, metaObj, sourceText, disambiguation, n);
}

static void* Hook_fromUtf8(void* ret, const QByteArrayView_C* view) {
    QByteArrayView_C mod_view;
    if (!s_is_in_game && g_lang_vn && view && view->data && view->size > 0 && view->size < 128) {
        char buf[128];
        memcpy(buf, view->data, (size_t)view->size);
        buf[view->size] = '\0';
        const char* vn = LookupVietnamese(buf);
        if (vn) {
            mod_view.data = vn;
            mod_view.size = (int64_t)strlen(vn);
            return s_orig_fromUtf8(ret, &mod_view);
        }
    }
    return s_orig_fromUtf8(ret, view);
}

static void* Hook_fromLatin1(void* ret, const QByteArrayView_C* view) {
    QByteArrayView_C mod_view;
    if (!s_is_in_game && g_lang_vn && view && view->data && view->size > 0 && view->size < 128 && s_orig_fromUtf8) {
        char buf[128];
        memcpy(buf, view->data, (size_t)view->size);
        buf[view->size] = '\0';
        const char* vn = LookupVietnamese(buf);
        if (vn) {
            mod_view.data = vn;
            mod_view.size = (int64_t)strlen(vn);
            return s_orig_fromUtf8(ret, &mod_view);
        }
    }
    return s_orig_fromLatin1(ret, view);
}

static void InstallQt6IATHooks(void) {
    BYTE* pBase = (BYTE*)GetModuleHandleW(NULL);
    if (!pBase) return;

    DWORD rva_translate  = 0x2886208;
    DWORD rva_tr         = 0x2886200;
    DWORD rva_fromUtf8   = 0x2885798;
    DWORD rva_fromLatin1 = 0x2885758;

    DWORD oldProt;

    void** pIAT_translate = (void**)(pBase + rva_translate);
    if (VirtualProtect(pIAT_translate, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        s_orig_translate = (pfn_translate)*pIAT_translate;
        *pIAT_translate = (void*)Hook_translate;
        VirtualProtect(pIAT_translate, sizeof(void*), oldProt, &oldProt);
    }

    void** pIAT_tr = (void**)(pBase + rva_tr);
    if (VirtualProtect(pIAT_tr, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        s_orig_tr = (pfn_tr)*pIAT_tr;
        *pIAT_tr = (void*)Hook_tr;
        VirtualProtect(pIAT_tr, sizeof(void*), oldProt, &oldProt);
    }

    void** pIAT_fromUtf8 = (void**)(pBase + rva_fromUtf8);
    if (VirtualProtect(pIAT_fromUtf8, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        s_orig_fromUtf8 = (pfn_fromUtf8)*pIAT_fromUtf8;
        *pIAT_fromUtf8 = (void*)Hook_fromUtf8;
        VirtualProtect(pIAT_fromUtf8, sizeof(void*), oldProt, &oldProt);
    }

    void** pIAT_fromLatin1 = (void**)(pBase + rva_fromLatin1);
    if (VirtualProtect(pIAT_fromLatin1, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        s_orig_fromLatin1 = (pfn_fromLatin1)*pIAT_fromLatin1;
        *pIAT_fromLatin1 = (void*)Hook_fromLatin1;
        VirtualProtect(pIAT_fromLatin1, sizeof(void*), oldProt, &oldProt);
    }
}

static void TriggerQtRetranslate(void) {
    HMODULE hQtCore = GetModuleHandleW(L"Qt6Core.dll");
    if (!hQtCore) return;
    typedef void* (*pfn_instance)(void);
    typedef void (*pfn_qevent_ctor)(void* this_ptr, int type);
    typedef void (*pfn_postEvent)(void* receiver, void* event, int priority);

    pfn_instance pInstance = (pfn_instance)GetProcAddress(hQtCore, "?instance@QCoreApplication@@SAPEAV1@XZ");
    pfn_qevent_ctor pEventCtor = (pfn_qevent_ctor)GetProcAddress(hQtCore, "??0QEvent@@QEAA@W4Type@0@@Z");
    pfn_postEvent pPostEvent = (pfn_postEvent)GetProcAddress(hQtCore, "?postEvent@QCoreApplication@@SAXPEAVQObject@@PEAVQEvent@@H@Z");

    if (pInstance && pEventCtor && pPostEvent) {
        void* app = pInstance();
        if (app) {
            void* ev = malloc(32);
            if (ev) {
                memset(ev, 0, 32);
                pEventCtor(ev, 89); // QEvent::LanguageChange = 89
                pPostEvent(app, ev, 0);
            }
        }
    }
}

// ==============================================================================
// 4. VERTICAL ON/OFF TOGGLE SWITCH (EN / VN) & LIVE Qt6 WIDGET TRANSLATOR
// ==============================================================================
#define ID_LANG_BTN 9901
#define WM_YNGCHII_INIT_BTN (WM_APP + 0x55)
#define TIMER_ID_YNGCHII 0x7788
#define TIMER_ID_ANIM    0x7789

static HWND s_hwndMain = NULL;
static HWND s_hwndLangBtn = NULL;
static WNDPROC s_origMainWndProc = NULL;

// Vertical Toggle Switch state:
// s_knob_pos = 0.0f (Top = EN), 1.0f (Bottom = VN)
static float s_knob_pos = 0.0f;
static int s_is_dragging = 0;
static int s_drag_moved = 0;
static int s_drag_start_y = 0;
static float s_drag_start_pos = 0.0f;

static void TriggerLiveQtWidgetTranslation(void) {
    if (s_is_in_game) return;
    static int s_in_translation = 0;
    if (s_in_translation) return;
    s_in_translation = 1;

    HMODULE hWidgets = GetModuleHandleW(L"Qt6Widgets.dll");
    HMODULE hCore = GetModuleHandleW(L"Qt6Core.dll");
    if (!hWidgets || !hCore) {
        s_in_translation = 0;
        return;
    }

    typedef void* (*pfn_allWidgets)(void* ret_qlist);
    typedef BOOL (*pfn_inherits)(void* this_ptr, const char* className);
    typedef void* (*pfn_fromUtf8_str)(void* ret_qstr, const char* str, int64_t size);
    typedef void* (*pfn_toUtf8)(const void* qstr_this, void* ret_qba);
    typedef const char* (*pfn_constData)(const void* qba_this);
    typedef void (*pfn_dtor)(void* q_ptr);

    typedef void* (*pfn_get_text)(void* this_ptr, void* ret_qstr);
    typedef void (*pfn_set_text)(void* this_ptr, const void* qstr_ptr);

    typedef int (*pfn_tab_count)(void* this_ptr);
    typedef void* (*pfn_tab_text)(void* this_ptr, void* ret_qstr, int index);
    typedef void (*pfn_tab_setText)(void* this_ptr, int index, const void* qstr_ptr);

    static pfn_allWidgets s_pfn_allWidgets = NULL;
    static pfn_inherits s_pfn_inherits = NULL;
    static pfn_fromUtf8_str s_pfn_fromUtf8_str = NULL;
    static pfn_toUtf8 s_pfn_toUtf8 = NULL;
    static pfn_constData s_pfn_constData = NULL;
    static pfn_dtor s_pfn_dtor = NULL;

    static pfn_get_text s_pfn_btn_text = NULL;
    static pfn_set_text s_pfn_btn_setText = NULL;
    static pfn_get_text s_pfn_lbl_text = NULL;
    static pfn_set_text s_pfn_lbl_setText = NULL;
    static pfn_get_text s_pfn_gb_title = NULL;
    static pfn_set_text s_pfn_gb_setTitle = NULL;

    static pfn_tab_count s_pfn_tab_count = NULL;
    static pfn_tab_text s_pfn_tab_text = NULL;
    static pfn_tab_setText s_pfn_tab_setText = NULL;

    if (!s_pfn_allWidgets) {
        s_pfn_allWidgets = (pfn_allWidgets)GetProcAddress(hWidgets, "?allWidgets@QApplication@@SA?AV?$QList@PEAVQWidget@@@@XZ");
        s_pfn_inherits = (pfn_inherits)GetProcAddress(hCore, "?inherits@QObject@@QEBA_NPEBD@Z");
        s_pfn_fromUtf8_str = (pfn_fromUtf8_str)GetProcAddress(hCore, "?fromUtf8@QString@@SA?AV1@PEBD_J@Z");
        s_pfn_toUtf8 = (pfn_toUtf8)GetProcAddress(hCore, "?toUtf8@QString@@QEGBA?AVQByteArray@@XZ");
        if (!s_pfn_toUtf8) s_pfn_toUtf8 = (pfn_toUtf8)GetProcAddress(hCore, "?toUtf8@QString@@QEHAA?AVQByteArray@@XZ");
        s_pfn_constData = (pfn_constData)GetProcAddress(hCore, "?constData@QByteArray@@QEBAPEBDXZ");
        s_pfn_dtor = (pfn_dtor)GetProcAddress(hCore, "??1QString@@QEAA@XZ");

        s_pfn_btn_text = (pfn_get_text)GetProcAddress(hWidgets, "?text@QAbstractButton@@QEBA?AVQString@@XZ");
        s_pfn_btn_setText = (pfn_set_text)GetProcAddress(hWidgets, "?setText@QAbstractButton@@QEAAXAEBVQString@@@Z");
        s_pfn_lbl_text = (pfn_get_text)GetProcAddress(hWidgets, "?text@QLabel@@QEBA?AVQString@@XZ");
        s_pfn_lbl_setText = (pfn_set_text)GetProcAddress(hWidgets, "?setText@QLabel@@QEAAXAEBVQString@@@Z");
        s_pfn_gb_title = (pfn_get_text)GetProcAddress(hWidgets, "?title@QGroupBox@@QEBA?AVQString@@XZ");
        s_pfn_gb_setTitle = (pfn_set_text)GetProcAddress(hWidgets, "?setTitle@QGroupBox@@QEAAXAEBVQString@@@Z");

        s_pfn_tab_count = (pfn_tab_count)GetProcAddress(hWidgets, "?count@QTabBar@@QEBAHXZ");
        s_pfn_tab_text = (pfn_tab_text)GetProcAddress(hWidgets, "?tabText@QTabBar@@QEBA?AVQString@@H@Z");
        s_pfn_tab_setText = (pfn_tab_setText)GetProcAddress(hWidgets, "?setTabText@QTabBar@@QEAAXHAEBVQString@@@Z");
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

    static int64_t s_last_widget_count = 0;
    static int s_last_lang = -1;
    if (widget_list.size == s_last_widget_count && s_last_lang == g_lang_vn) {
        s_pfn_dtor(&widget_list);
        s_in_translation = 0;
        return;
    }
    s_last_widget_count = widget_list.size;
    s_last_lang = g_lang_vn;

    if (widget_list.ptr && widget_list.size > 0) {
        for (int64_t i = 0; i < widget_list.size; i++) {
            void* w = widget_list.ptr[i];
            if (!w) continue;

            // 1. Handle QTabBar tabs (Settings / Game Settings dialog headers)
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
                    s_pfn_dtor(qba_buf);
                    s_pfn_dtor(qstr_buf);
                }
                continue;
            }

            // 2. Handle Buttons, Labels, GroupBoxes
            pfn_get_text fn_get = NULL;
            pfn_set_text fn_set = NULL;

            if (s_pfn_btn_text && s_pfn_btn_setText && s_pfn_inherits(w, "QAbstractButton")) {
                fn_get = s_pfn_btn_text;
                fn_set = s_pfn_btn_setText;
            } else if (s_pfn_lbl_text && s_pfn_lbl_setText && s_pfn_inherits(w, "QLabel")) {
                fn_get = s_pfn_lbl_text;
                fn_set = s_pfn_lbl_setText;
            } else if (s_pfn_gb_title && s_pfn_gb_setTitle && s_pfn_inherits(w, "QGroupBox")) {
                fn_get = s_pfn_gb_title;
                fn_set = s_pfn_gb_setTitle;
            }

            if (fn_get && fn_set) {
                char qstr_buf[32] = {0};
                fn_get(w, qstr_buf);

                char qba_buf[32] = {0};
                s_pfn_toUtf8(qstr_buf, qba_buf);
                const char* cur_text = s_pfn_constData(qba_buf);

                if (cur_text && cur_text[0] != '\0') {
                    const char* target_text = TranslateTextBidirectional(cur_text, g_lang_vn);
                    if (target_text && strcmp(cur_text, target_text) != 0) {
                        char new_qstr_buf[32] = {0};
                        s_pfn_fromUtf8_str(new_qstr_buf, target_text, -1);
                        fn_set(w, new_qstr_buf);
                        s_pfn_dtor(new_qstr_buf);
                    }
                }

                s_pfn_dtor(qba_buf);
                s_pfn_dtor(qstr_buf);
            }
        }
    }

    s_pfn_dtor(&widget_list);
    s_in_translation = 0;
}

static void ApplyLanguageState(HWND hBtnWnd, int new_vn) {
    if (g_lang_vn != new_vn) {
        g_lang_vn = new_vn;
        SaveLanguagePreference(g_lang_vn);
        TriggerQtRetranslate();
        TriggerLiveQtWidgetTranslation();
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

            // Double-buffered painting to prevent any flicker when sliding
            HDC hdc = CreateCompatibleDC(hdcScreen);
            HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, w, h);
            HGDIOBJ oldBm = SelectObject(hdc, hbm);

            // 1. Clear outer background to match Fritchii sidebar RGB(12, 12, 14)
            HBRUSH sidebarBrush = CreateSolidBrush(RGB(12, 12, 14));
            FillRect(hdc, &rc, sidebarBrush);
            DeleteObject(sidebarBrush);

            // 2. Draw vertical capsule track (Ranh gat doc)
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

            // 3. Draw background slot labels ("EN" on top half, "VN" on bottom half)
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

            // 4. Draw sliding thumb/knob (Num gat len / xuong)
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

            // 5. Draw active label on the sliding knob
            SelectObject(hdc, hFontKnob);
            SetTextColor(hdc, RGB(255, 255, 255));
            RECT rcKnob = {pad, knobY, pad + knobW, knobY + knobH};
            DrawTextW(hdc, is_vn_visual ? L"VN" : L"EN", -1, &rcKnob, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, oldFont);
            DeleteObject(hFontBg);
            DeleteObject(hFontKnob);

            BitBlt(hdcScreen, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
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
            KillTimer(hWnd, TIMER_ID_ANIM);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (s_is_dragging) {
                RECT rc;
                GetClientRect(hWnd, &rc);
                int h = rc.bottom - rc.top;
                int pad = 3;
                int knobH = (h - pad * 2) / 2;
                int travel = (h - pad * 2) - knobH;
                if (travel < 1) travel = 1;

                int curY = GET_Y_LPARAM(lParam);
                int dy = curY - s_drag_start_y;
                if (dy < -2 || dy > 2) {
                    s_drag_moved = 1;
                }

                float new_pos = s_drag_start_pos + (float)dy / (float)travel;
                if (new_pos < 0.0f) new_pos = 0.0f;
                if (new_pos > 1.0f) new_pos = 1.0f;
                s_knob_pos = new_pos;

                int target_vn = (s_knob_pos >= 0.5f) ? 1 : 0;
                if (target_vn != g_lang_vn) {
                    g_lang_vn = target_vn;
                    SaveLanguagePreference(g_lang_vn);
                    TriggerQtRetranslate();
                    TriggerLiveQtWidgetTranslation();
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
                    // If clicked on top half -> EN (0), bottom half -> VN (1), or toggle if clicking active side
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
                ApplyLanguageState(hWnd, 0); // Wheel Up -> EN
            } else if (delta < 0) {
                ApplyLanguageState(hWnd, 1); // Wheel Down -> VN
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

    // Sidebar center is at X = (24 * dpi) / 96 (sidebar is 48px wide)
    int centerX = (24 * (int)dpi) / 96;
    int bw = (26 * (int)dpi) / 96;
    int bh = (52 * (int)dpi) / 96;
    int bx = centerX - (bw / 2); // 24 - 13 = 11 at 96 DPI
    int by = (200 * (int)dpi) / 96; // Y = 200 at 96 DPI

    *out_x = bx;
    *out_y = by;
    *out_w = bw;
    *out_h = bh;
}

static LRESULT CALLBACK MainWndProcHook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
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

        s_knob_pos = g_lang_vn ? 1.0f : 0.0f;

        int bx, by, bw, bh;
        GetToggleSwitchGeometry(hWnd, &bx, &by, &bw, &bh);

        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = LangBtnWndProc;
        wc.hInstance = g_hInst ? g_hInst : GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_HAND);
        wc.lpszClassName = L"YngchiiLangSwitch";
        RegisterClassExW(&wc);

        if (!s_hwndLangBtn || !IsWindow(s_hwndLangBtn)) {
            s_hwndLangBtn = CreateWindowExW(
                0,
                L"YngchiiLangSwitch",
                L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                bx, by, bw, bh,
                hWnd,
                (HMENU)ID_LANG_BTN,
                g_hInst ? g_hInst : GetModuleHandleW(NULL),
                NULL
            );
            if (s_hwndLangBtn) {
                SetWindowPos(s_hwndLangBtn, HWND_TOP, bx, by, bw, bh, SWP_SHOWWINDOW);
                InvalidateRect(s_hwndLangBtn, NULL, FALSE);
                UpdateWindow(s_hwndLangBtn);
            }
        }

        SetTimer(hWnd, TIMER_ID_YNGCHII, 400, NULL);
        TriggerLiveQtWidgetTranslation();
        return 0;
    }

    if (uMsg == WM_SIZE || uMsg == WM_ACTIVATE) {
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
            return 0;
        }

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
        TriggerLiveQtWidgetTranslation();
        return 0;
    }

    if (uMsg == WM_KEYDOWN) {
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
// 6. MONITOR THREAD: WINDOW TITLE, ICON, EN/VN SWITCH, & THREAD PRIORITIES
// ==============================================================================
static DWORD WINAPI MonitorThread(LPVOID lpParam) {
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

    HICON hChainsaw = (HICON)LoadImageW(hExe, MAKEINTRESOURCEW(568), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);

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
// 7. DLL ENTRY POINT
// ==============================================================================
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        g_hInst = hinstDLL;

        DetectSupportedRyzenCpu();
        g_lang_vn = LoadLanguagePreference();
        InstallQt6IATHooks();

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

            char exe_path[MAX_PATH] = {0};
            if (GetModuleFileNameA(NULL, exe_path, MAX_PATH) > 0) {
                char* last_slash = strrchr(exe_path, '\\');
                if (last_slash) {
                    *last_slash = '\0';
                    char switch_path[MAX_PATH];
                    snprintf(switch_path, MAX_PATH, "%s\\nx2_switches.txt", exe_path);
                    LoadNx2SwitchesFile(switch_path);
                }
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
    }
    return TRUE;
}
