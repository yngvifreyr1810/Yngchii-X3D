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
#include <math.h>
#include <gdiplus.h>
#include <shlwapi.h>

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

// ==============================================================================
// 2B. THEME STATE & PREFERENCE STORAGE (F10 Cycling)
// ==============================================================================
typedef struct {
    const char* name;
    const char* accent;          // Hex accent, e.g. "#F97316"
    const char* accent_dark;     // Pressed, e.g. "#EA580C"
    const char* accent_light;    // Hover, e.g. "#FB923C"
    const char* text_on_accent;  // Text color on play button, e.g. "#1C1433" or "#000000"
    const char* glow_rgba;       // "rgba(249, 115, 22, 0.35)"
    COLORREF ref_accent;         // RGB(249, 115, 22)
    COLORREF ref_dark;           // RGB(234, 88, 12)
    COLORREF ref_light;          // RGB(251, 146, 60)
} ThemePalette;

static ThemePalette g_palettes[4] = {
    {"Chainsaw Crimson", "#b52c2f", "#962124", "#c93639", "#efead7", "rgba(181, 44, 47, 0.35)", RGB(181, 44, 47), RGB(150, 33, 36), RGB(201, 54, 57)},
    {"Emerald Teal",   "#10B981", "#059669", "#34D399", "#064E3B", "rgba(16, 185, 129, 0.35)", RGB(16, 185, 129), RGB(5, 150, 105), RGB(52, 211, 153)},
    {"Frost Cyan",     "#0EA5E9", "#0284C7", "#38BDF8", "#082F49", "rgba(14, 165, 233, 0.35)", RGB(14, 165, 233), RGB(2, 132, 199), RGB(56, 189, 248)},
    {"Crimson Rose",   "#F43F5E", "#E11D48", "#FB7185", "#4C0519", "rgba(244, 63, 94, 0.35)", RGB(244, 63, 94), RGB(225, 29, 72), RGB(251, 113, 133)}
};

static void UpdateInternalFritchiiAccent(int theme_idx) {
    if (theme_idx < 0 || theme_idx >= 4) return;
    HMODULE hExe = GetModuleHandleW(NULL);
    if (!hExe) return;
    BYTE* base = (BYTE*)hExe;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const DWORD accent_rva = 0x222d386;
    if (nt->OptionalHeader.SizeOfImage < accent_rva + 6) return;

    BYTE* accent = base + accent_rva;
    const BYTE stock_color[] = {0xa7, 0xa7, 0x8b, 0x8b, 0xfa, 0xfa};
    bool known_color = memcmp(accent, stock_color, sizeof(stock_color)) == 0;
    for (const ThemePalette& palette : g_palettes) {
        BYTE r = GetRValue(palette.ref_accent);
        BYTE g = GetGValue(palette.ref_accent);
        BYTE b = GetBValue(palette.ref_accent);
        const BYTE color[] = {r, r, g, g, b, b};
        if (memcmp(accent, color, sizeof(color)) == 0) known_color = true;
    }
    // This address belongs to the stock v5.6 executable's theme string.
    if (!known_color) return;

    DWORD oldProtect;
    if (VirtualProtect(accent, 6, PAGE_READWRITE, &oldProtect)) {
        COLORREF col = g_palettes[theme_idx].ref_accent;
        BYTE r = GetRValue(col);
        BYTE g = GetGValue(col);
        BYTE b = GetBValue(col);
        const BYTE color[] = {r, r, g, g, b, b};
        memcpy(accent, color, sizeof(color));
        VirtualProtect(accent, 6, oldProtect, &oldProtect);
    }
}

static volatile int g_theme_idx = 0; // 0=Amber, 1=Emerald, 2=Cyan, 3=Crimson

static int LoadThemePreference(void) {
    char appdata[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\eden\\config\\yngchii_theme.cfg", appdata);
        FILE* f = fopen(path, "r");
        if (f) {
            int t = 0;
            if (fscanf(f, "%d", &t) == 1 && t >= 0 && t <= 3) {
                fclose(f);
                return t;
            }
            fclose(f);
        }
    }
    return 0; // Default: Amber Gold
}

static void SaveThemePreference(int t) {
    char appdata[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s\\eden\\config", appdata);
        CreateDirectoryA(dir, NULL);
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\eden\\config\\yngchii_theme.cfg", appdata);
        FILE* f = fopen(path, "w");
        if (f) {
            fprintf(f, "%d\n", t);
            fclose(f);
        }
    }
}

// ==============================================================================
// 2C. GDI+ IMAGE PROCESSING (Fast Box Blur & Rounded Covers)
// ==============================================================================
static void BlurBoxH(BYTE* src, BYTE* dst, int w, int h, int r) {
    float iarr = 1.0f / (r + r + 1);
    for (int y = 0; y < h; y++) {
        int ti = y * w * 3;
        int li = ti;
        int ri = ti + r * 3;
        int fv_r = src[ti], fv_g = src[ti+1], fv_b = src[ti+2];
        int lv_r = src[ti + (w-1)*3], lv_g = src[ti + (w-1)*3 + 1], lv_b = src[ti + (w-1)*3 + 2];
        int val_r = (r + 1) * fv_r, val_g = (r + 1) * fv_g, val_b = (r + 1) * fv_b;
        for (int j = 0; j < r; j++) {
            val_r += src[ti + j*3];
            val_g += src[ti + j*3 + 1];
            val_b += src[ti + j*3 + 2];
        }
        for (int j = 0; j <= r; j++) {
            val_r += src[ri] - fv_r;
            val_g += src[ri+1] - fv_g;
            val_b += src[ri+2] - fv_b;
            dst[ti] = (BYTE)(val_r * iarr);
            dst[ti+1] = (BYTE)(val_g * iarr);
            dst[ti+2] = (BYTE)(val_b * iarr);
            ri += 3; ti += 3;
        }
        for (int j = r + 1; j < w - r; j++) {
            val_r += src[ri] - src[li];
            val_g += src[ri+1] - src[li+1];
            val_b += src[ri+2] - src[li+2];
            dst[ti] = (BYTE)(val_r * iarr);
            dst[ti+1] = (BYTE)(val_g * iarr);
            dst[ti+2] = (BYTE)(val_b * iarr);
            ri += 3; li += 3; ti += 3;
        }
        for (int j = w - r; j < w; j++) {
            val_r += lv_r - src[li];
            val_g += lv_g - src[li+1];
            val_b += lv_b - src[li+2];
            dst[ti] = (BYTE)(val_r * iarr);
            dst[ti+1] = (BYTE)(val_g * iarr);
            dst[ti+2] = (BYTE)(val_b * iarr);
            li += 3; ti += 3;
        }
    }
}

static void BlurBoxV(BYTE* src, BYTE* dst, int w, int h, int r) {
    float iarr = 1.0f / (r + r + 1);
    for (int x = 0; x < w; x++) {
        int ti = x * 3;
        int li = ti;
        int ri = ti + r * w * 3;
        int fv_r = src[ti], fv_g = src[ti+1], fv_b = src[ti+2];
        int lv_r = src[ti + (h-1)*w*3], lv_g = src[ti + (h-1)*w*3 + 1], lv_b = src[ti + (h-1)*w*3 + 2];
        int val_r = (r + 1) * fv_r, val_g = (r + 1) * fv_g, val_b = (r + 1) * fv_b;
        for (int j = 0; j < r; j++) {
            val_r += src[ti + j*w*3];
            val_g += src[ti + j*w*3 + 1];
            val_b += src[ti + j*w*3 + 2];
        }
        for (int j = 0; j <= r; j++) {
            val_r += src[ri] - fv_r;
            val_g += src[ri+1] - fv_g;
            val_b += src[ri+2] - fv_b;
            dst[ti] = (BYTE)(val_r * iarr);
            dst[ti+1] = (BYTE)(val_g * iarr);
            dst[ti+2] = (BYTE)(val_b * iarr);
            ri += w*3; ti += w*3;
        }
        for (int j = r + 1; j < h - r; j++) {
            val_r += src[ri] - src[li];
            val_g += src[ri+1] - src[li+1];
            val_b += src[ri+2] - src[li+2];
            dst[ti] = (BYTE)(val_r * iarr);
            dst[ti+1] = (BYTE)(val_g * iarr);
            dst[ti+2] = (BYTE)(val_b * iarr);
            ri += w*3; li += w*3; ti += w*3;
        }
        for (int j = h - r; j < h; j++) {
            val_r += lv_r - src[li];
            val_g += lv_g - src[li+1];
            val_b += lv_b - src[li+2];
            dst[ti] = (BYTE)(val_r * iarr);
            dst[ti+1] = (BYTE)(val_g * iarr);
            dst[ti+2] = (BYTE)(val_b * iarr);
            li += w*3; ti += w*3;
        }
    }
}

static bool StartGdiPlus(ULONG_PTR* token) {
    // Resolve the delay-loaded library only when a cover needs it.
    static const HMODULE module = LoadLibraryExW(L"gdiplus.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;

    Gdiplus::GdiplusStartupInput input;
    return Gdiplus::GdiplusStartup(token, &input, NULL) == Gdiplus::Ok;
}

static int GenerateBlurredHeroBackdrop(const wchar_t* src_file, const wchar_t* out_file, int bw, int bh) {
    if (bw < 200) bw = 1216;
    if (bh < 50) bh = 212;

    ULONG_PTR gdi_token = 0;
    if (!StartGdiPlus(&gdi_token)) return 0;

    Gdiplus::GpBitmap* src_bmp = NULL;
    Gdiplus::DllExports::GdipCreateBitmapFromFile(src_file, &src_bmp);
    if (!src_bmp) {
        Gdiplus::GdiplusShutdown(gdi_token);
        return 0;
    }

    UINT src_w = 0, src_h = 0;
    Gdiplus::DllExports::GdipGetImageWidth(src_bmp, &src_w);
    Gdiplus::DllExports::GdipGetImageHeight(src_bmp, &src_h);

    float target_aspect = (float)bw / (float)bh;
    float src_aspect = (float)src_w / (float)src_h;
    int crop_x = 0, crop_y = 0, crop_w = src_w, crop_h = src_h;

    if (src_aspect < target_aspect) {
        crop_w = src_w;
        crop_h = (int)(src_w / target_aspect);
        crop_x = 0;
        crop_y = (int)((src_h - crop_h) * 0.35f);
        if (crop_y < 0) crop_y = 0;
        if (crop_y + crop_h > (int)src_h) crop_h = src_h - crop_y;
    } else {
        crop_h = src_h;
        crop_w = (int)(src_h * target_aspect);
        crop_y = 0;
        crop_x = (src_w - crop_w) / 2;
        if (crop_x < 0) crop_x = 0;
        if (crop_x + crop_w > (int)src_w) crop_w = src_w - crop_x;
    }

    int sw = 360, sh = (int)(360.0f * (float)bh / (float)bw);
    if (sh < 68) sh = 68;
    Gdiplus::GpBitmap* small_bmp = NULL;
    Gdiplus::DllExports::GdipCreateBitmapFromScan0(sw, sh, 0, PixelFormat24bppRGB, NULL, &small_bmp);
    Gdiplus::GpGraphics* gfx = NULL;
    Gdiplus::DllExports::GdipGetImageGraphicsContext(small_bmp, &gfx);
    Gdiplus::DllExports::GdipSetInterpolationMode(gfx, Gdiplus::InterpolationModeHighQualityBilinear);
    Gdiplus::DllExports::GdipDrawImageRectRectI(gfx, src_bmp, 0, 0, sw, sh, crop_x, crop_y, crop_w, crop_h, Gdiplus::UnitPixel, NULL, NULL, NULL);
    Gdiplus::DllExports::GdipDeleteGraphics(gfx);
    Gdiplus::DllExports::GdipDisposeImage(src_bmp);

    Gdiplus::BitmapData bdata;
    Gdiplus::Rect lock_rc = {0, 0, sw, sh};
    Gdiplus::DllExports::GdipBitmapLockBits(small_bmp, &lock_rc, Gdiplus::ImageLockModeRead, PixelFormat24bppRGB, &bdata);
    BYTE* raw_rgb = (BYTE*)malloc(sw * sh * 3);
    BYTE* buf2 = (BYTE*)malloc(sw * sh * 3);
    if (!raw_rgb || !buf2) {
        if (raw_rgb) free(raw_rgb);
        if (buf2) free(buf2);
        Gdiplus::DllExports::GdipBitmapUnlockBits(small_bmp, &bdata);
        Gdiplus::DllExports::GdipDisposeImage(small_bmp);
        Gdiplus::GdiplusShutdown(gdi_token);
        return 0;
    }
    for (int y = 0; y < sh; y++) {
        memcpy(raw_rgb + y * sw * 3, (BYTE*)bdata.Scan0 + y * bdata.Stride, sw * 3);
    }
    Gdiplus::DllExports::GdipBitmapUnlockBits(small_bmp, &bdata);
    Gdiplus::DllExports::GdipDisposeImage(small_bmp);

    for (int pass = 0; pass < 2; pass++) {
        BlurBoxH(raw_rgb, buf2, sw, sh, 3);
        BlurBoxV(buf2, raw_rgb, sw, sh, 3);
    }
    free(buf2);

    DWORD* argb = (DWORD*)malloc(bw * bh * 4);
    if (!argb) {
        free(raw_rgb);
        Gdiplus::GdiplusShutdown(gdi_token);
        return 0;
    }

    BYTE bg_r = 0x12, bg_g = 0x12, bg_b = 0x17;
    int corner_r = 16;

    for (int y = 0; y < bh; y++) {
        float sy = (y / (float)bh) * (sh - 1);
        int y0 = (int)sy;
        int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
        float dy = sy - y0;

        for (int x = 0; x < bw; x++) {
            float sx = (x / (float)bw) * (sw - 1);
            int x0 = (int)sx;
            int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            float dx = sx - x0;

            BYTE* p00 = &raw_rgb[(y0 * sw + x0) * 3];
            BYTE* p10 = &raw_rgb[(y0 * sw + x1) * 3];
            BYTE* p01 = &raw_rgb[(y1 * sw + x0) * 3];
            BYTE* p11 = &raw_rgb[(y1 * sw + x1) * 3];

            float r_top = p00[2] * (1.0f - dx) + p10[2] * dx;
            float g_top = p00[1] * (1.0f - dx) + p10[1] * dx;
            float b_top = p00[0] * (1.0f - dx) + p10[0] * dx;

            float r_bot = p01[2] * (1.0f - dx) + p11[2] * dx;
            float g_bot = p01[1] * (1.0f - dx) + p11[1] * dx;
            float b_bot = p01[0] * (1.0f - dx) + p11[0] * dx;

            float art_r = r_top * (1.0f - dy) + r_bot * dy;
            float art_g = g_top * (1.0f - dy) + g_bot * dy;
            float art_b = b_top * (1.0f - dy) + b_bot * dy;

            int corner_dx = 0, corner_dy = 0;
            int in_corner = 0;
            if (x < corner_r && y < corner_r) { corner_dx = corner_r - x; corner_dy = corner_r - y; in_corner = 1; }
            else if (x >= bw - corner_r && y < corner_r) { corner_dx = x - (bw - corner_r - 1); corner_dy = corner_r - y; in_corner = 1; }
            else if (x < corner_r && y >= bh - corner_r) { corner_dx = corner_r - x; corner_dy = y - (bh - corner_r - 1); in_corner = 1; }
            else if (x >= bw - corner_r && y >= bh - corner_r) { corner_dx = x - (bw - corner_r - 1); corner_dy = y - (bh - corner_r - 1); in_corner = 1; }

            int alpha = 255;
            float dist = 0.0f;
            if (in_corner) {
                dist = sqrtf((float)(corner_dx * corner_dx + corner_dy * corner_dy));
                if (dist > corner_r) {
                    alpha = 0;
                } else if (dist > corner_r - 1.0f) {
                    alpha = (int)(255.0f * (corner_r - dist));
                }
            }

            if (alpha == 0) {
                argb[y * bw + x] = 0;
                continue;
            }

            float pct_x = x / (float)bw;
            float dark_ratio;
            if (pct_x < 0.50f) {
                dark_ratio = 0.62f - 0.12f * (pct_x / 0.50f);
            } else {
                dark_ratio = 0.50f - 0.08f * ((pct_x - 0.50f) / 0.50f);
            }

            float pct_y = y / (float)bh;
            float edge_y = (pct_y < 0.5f ? pct_y : 1.0f - pct_y) * 2.0f;
            if (edge_y < 0.15f) {
                dark_ratio += (0.15f - edge_y) * 0.3f;
            }
            if (dark_ratio < 0.35f) dark_ratio = 0.35f;
            if (dark_ratio > 0.75f) dark_ratio = 0.75f;
            float art_ratio = 1.0f - dark_ratio;

            int fr = (int)(bg_r * dark_ratio + art_r * art_ratio);
            int fg = (int)(bg_g * dark_ratio + art_g * art_ratio);
            int fb = (int)(bg_b * dark_ratio + art_b * art_ratio);

            // Subtle 1px border rgba(255,255,255, 0.10)
            if (x == 0 || x == bw - 1 || y == 0 || y == bh - 1 || (in_corner && fabsf(dist - (corner_r - 0.5f)) < 1.0f)) {
                fr = (fr + 20 > 255) ? 255 : fr + 20;
                fg = (fg + 20 > 255) ? 255 : fg + 20;
                fb = (fb + 25 > 255) ? 255 : fb + 25;
            }

            argb[y * bw + x] = ((DWORD)alpha << 24) | ((DWORD)fr << 16) | ((DWORD)fg << 8) | (DWORD)fb;
        }
    }
    free(raw_rgb);

    Gdiplus::GpBitmap* out_bmp = NULL;
    Gdiplus::DllExports::GdipCreateBitmapFromScan0(bw, bh, bw * 4, PixelFormat32bppARGB, (BYTE*)argb, &out_bmp);
    CLSID pngClsid;
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &pngClsid);
    Gdiplus::DllExports::GdipSaveImageToFile(out_bmp, out_file, &pngClsid, NULL);
    Gdiplus::DllExports::GdipDisposeImage(out_bmp);
    free(argb);

    Gdiplus::GdiplusShutdown(gdi_token);
    return 1;
}

static int GenerateRoundedCoverIcon(const wchar_t* src_file, const wchar_t* out_file, int sz, int cr) {
    ULONG_PTR gdi_token = 0;
    if (!StartGdiPlus(&gdi_token)) return 0;

    Gdiplus::GpBitmap* src_bmp = NULL;
    Gdiplus::DllExports::GdipCreateBitmapFromFile(src_file, &src_bmp);
    if (!src_bmp) {
        Gdiplus::GdiplusShutdown(gdi_token);
        return 0;
    }

    Gdiplus::GpBitmap* out_bmp = NULL;
    Gdiplus::DllExports::GdipCreateBitmapFromScan0(sz, sz, 0, PixelFormat32bppARGB, NULL, &out_bmp);
    Gdiplus::GpGraphics* gfx = NULL;
    Gdiplus::DllExports::GdipGetImageGraphicsContext(out_bmp, &gfx);
    Gdiplus::DllExports::GdipSetInterpolationMode(gfx, Gdiplus::InterpolationModeHighQualityBicubic);
    Gdiplus::DllExports::GdipSetSmoothingMode(gfx, Gdiplus::SmoothingModeAntiAlias);

    Gdiplus::GpPath* path = NULL;
    Gdiplus::DllExports::GdipCreatePath(Gdiplus::FillModeAlternate, &path);
    int d = cr * 2;
    Gdiplus::DllExports::GdipAddPathArcI(path, 0, 0, d, d, 180, 90);
    Gdiplus::DllExports::GdipAddPathArcI(path, sz - d, 0, d, d, 270, 90);
    Gdiplus::DllExports::GdipAddPathArcI(path, sz - d, sz - d, d, d, 0, 90);
    Gdiplus::DllExports::GdipAddPathArcI(path, 0, sz - d, d, d, 90, 90);
    Gdiplus::DllExports::GdipClosePathFigure(path);

    Gdiplus::DllExports::GdipSetClipPath(gfx, path, Gdiplus::CombineModeReplace);
    Gdiplus::DllExports::GdipDrawImageRectI(gfx, src_bmp, 0, 0, sz, sz);
    Gdiplus::DllExports::GdipResetClip(gfx);

    Gdiplus::GpPen* pen = NULL;
    Gdiplus::DllExports::GdipCreatePen1(0x33FFFFFF, 1.0f, Gdiplus::UnitPixel, &pen);
    Gdiplus::DllExports::GdipDrawPath(gfx, pen, path);
    Gdiplus::DllExports::GdipDeletePen(pen);

    Gdiplus::DllExports::GdipDeletePath(path);
    Gdiplus::DllExports::GdipDeleteGraphics(gfx);
    Gdiplus::DllExports::GdipDisposeImage(src_bmp);

    CLSID pngClsid;
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &pngClsid);
    Gdiplus::DllExports::GdipSaveImageToFile(out_bmp, out_file, &pngClsid, NULL);
    Gdiplus::DllExports::GdipDisposeImage(out_bmp);

    Gdiplus::GdiplusShutdown(gdi_token);
    return 1;
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
            if (!impDesc->OriginalFirstThunk || !impDesc->FirstThunk) return;
            IMAGE_THUNK_DATA* origThunk = (IMAGE_THUNK_DATA*)(base + impDesc->OriginalFirstThunk);
            IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base + impDesc->FirstThunk);

            for (; origThunk->u1.AddressOfData != 0; origThunk++, thunk++) {
                if (!(origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    IMAGE_IMPORT_BY_NAME* impByName = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
                    if (strcmp((char*)impByName->Name, funcName) == 0) {
                        if (!thunk->u1.Function || (void*)thunk->u1.Function == newFunc) return;
                        DWORD oldProtect;
                        if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) return;
                        *origFunc = (void*)thunk->u1.Function;
                        thunk->u1.Function = (ULONG_PTR)newFunc;
                        VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
                        return;
                    }
                }
            }
        }
    }
}

typedef void* (*pfn_QMenu_exec)(void* this_ptr, const void* pos, void* action);
static pfn_QMenu_exec s_orig_QMenu_exec = NULL;
static void* Hook_QMenu_exec(void* this_ptr, const void* pos, void* action);

static void InstallQt6IATHooks(void) {
    HMODULE hExe = GetModuleHandleW(NULL);
    if (!hExe) return;

    // Qt6Core.dll string conversion & translation hooks
    HookIAT(hExe, "Qt6Core.dll", "?translate@QCoreApplication@@SA?AVQString@@PEBD00H@Z", (void*)Hook_translate, (void**)&s_orig_translate);
    HookIAT(hExe, "Qt6Core.dll", "?tr@QObject@@SA?AVQString@@PEBD0H@Z", (void*)Hook_tr, (void**)&s_orig_tr);
    HookIAT(hExe, "Qt6Core.dll", "?fromUtf8@QString@@SA?AV1@V?$QBasicUtf8StringView@D@@@Z", (void*)Hook_fromUtf8, (void**)&s_orig_fromUtf8);
    HookIAT(hExe, "Qt6Core.dll", "?fromLatin1@QString@@SA?AV1@V?$QBasicLatin1StringView@D@@@Z", (void*)Hook_fromLatin1, (void**)&s_orig_fromLatin1);
    HookIAT(hExe, "Qt6Widgets.dll", "?exec@QMenu@@QEAAPEAVQAction@@AEBVQPoint@@PEAV2@@Z", (void*)Hook_QMenu_exec, (void**)&s_orig_QMenu_exec);
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
typedef void (*pfn_setFixedHeight)(void* this_ptr, int h);
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
typedef void (*pfn_act_trigger)(void* this_ptr);
typedef void (*pfn_act_activate)(void* this_ptr, int action_event);

typedef void* (*pfn_view_model)(void* view_this);
typedef void* (*pfn_model_root)(void* model_this);
typedef void* (*pfn_model_item)(void* model_this, int row, int column);
typedef int (*pfn_qitem_rowCount)(void* item_this);
typedef void* (*pfn_qitem_child)(void* item_this, int row, int column);
typedef void* (*pfn_qitem_data)(void* item_this, void* ret_qvariant, int role);
typedef void* (*pfn_qvariant_toString)(const void* qvariant_this, void* ret_qstring);
typedef void (*pfn_qvariant_dtor)(void* qvariant_ptr);

typedef void* (*pfn_sourceModel)(const void* proxy_this);
typedef uint64_t (*pfn_toULongLong)(const void* qvariant_this, BOOL* ok);
typedef void (*pfn_layout_item_setAlign)(void* layout_item, int alignment);
typedef BOOL (*pfn_layout_setAlign_widget)(void* layout, void* widget, int alignment);
typedef void (*pfn_setSizePolicy)(void* widget_this, int h_policy, int v_policy);

// QScrollArea
typedef void* (*pfn_scroll_ctor)(void* this_ptr, void* parent);
typedef void (*pfn_setWidget)(void* this_ptr, void* widget);
typedef void (*pfn_setWidgetResizable)(void* this_ptr, char resizable);
typedef void (*pfn_setFrameShape)(void* this_ptr, int shape);
typedef void (*pfn_setScrollBarPolicy)(void* this_ptr, int policy);

typedef const void* (*pfn_geometry)(void* this_ptr);
typedef void (*pfn_setGeometry)(void* this_ptr, int x, int y, int w, int h);
typedef void (*pfn_lower)(void* this_ptr);
typedef void (*pfn_setScaledContents)(void* label_this, char scaled);
typedef void (*pfn_pixmap_ctor_file)(void* this_ptr, const void* qstr_file, const char* format, int flags);
typedef void (*pfn_qicon_ctor_file)(void* this_ptr, const void* qstr_file);
typedef void (*pfn_qicon_ctor_pixmap)(void* this_ptr, const void* qpixmap);
typedef void (*pfn_qicon_dtor)(void* this_ptr);
typedef void (*pfn_setIcon)(void* this_ptr, const void* qicon_ptr);

// QLayout / QBoxLayout
typedef void (*pfn_removeWidget)(void* layout_ptr, void* widget);
typedef void (*pfn_insertWidget)(void* layout_ptr, int index, void* widget, int stretch, int alignment);
typedef void (*pfn_addWidget)(void* layout_ptr, void* widget, int stretch, int alignment);
typedef void* (*pfn_hbox_ctor)(void* this_ptr, void* parent);
typedef void (*pfn_setContentsMargins)(void* layout_ptr, int left, int top, int right, int bottom);
typedef void (*pfn_setSpacing)(void* layout_ptr, int spacing);

// QLabel / QWidget / QObject
typedef void* (*pfn_lbl_ctor)(void* this_ptr, void* parent, int flags);
typedef void (*pfn_lbl_setText)(void* this_ptr, const void* qstr_ptr);
typedef void (*pfn_setAttribute)(void* this_ptr, int attribute, char on);
typedef void (*pfn_setFixedWidth)(void* this_ptr, int w);
typedef void (*pfn_setAlignment)(void* this_ptr, int alignment);
typedef void (*pfn_setWordWrap)(void* this_ptr, char on);
typedef void (*pfn_hide)(void* this_ptr);
typedef void (*pfn_show)(void* this_ptr);
typedef void (*pfn_setObjectName)(void* this_ptr, const void* qstr_name);

typedef struct {
    void* d;
    void** ptr;
    int64_t size;
} Qt6List;
typedef const Qt6List* (*pfn_children)(const void* obj_this);

typedef void* (*pfn_btn_ctor)(void* this_ptr, void* parent);
typedef void* (*pfn_toolbtn_ctor)(void* this_ptr, void* parent);
typedef void (*pfn_btn_click)(void* this_ptr);
typedef bool (*pfn_is_visible)(void* this_ptr);
typedef int (*pfn_event_type)(void* event_ptr);
typedef void* (*pfn_parent_widget)(void* this_ptr);
typedef void (*pfn_cmevent_ctor)(void* this_ptr, int reason, const int* pos);
typedef void (*pfn_cmevent_dtor)(void* this_ptr);
typedef bool (*pfn_send_event)(void* receiver, void* event);

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
static pfn_act_trigger s_pfn_act_trigger = NULL;
static pfn_act_activate s_pfn_act_activate = NULL;

static pfn_view_model s_pfn_view_model = NULL;
static pfn_model_root s_pfn_model_root = NULL;
static pfn_model_item s_pfn_model_item = NULL;
static pfn_qitem_rowCount s_pfn_qitem_rowCount = NULL;
static pfn_qitem_child s_pfn_qitem_child = NULL;
static pfn_qitem_data s_pfn_qitem_data = NULL;
static pfn_qvariant_toString s_pfn_qvariant_toString = NULL;
static pfn_qvariant_dtor s_pfn_qvariant_dtor = NULL;

static pfn_sourceModel s_pfn_sourceModel = NULL;
static pfn_toULongLong s_pfn_toULongLong = NULL;
static pfn_layout_item_setAlign s_pfn_layout_item_setAlign = NULL;
static pfn_layout_setAlign_widget s_pfn_layout_setAlign_widget = NULL;
static pfn_setSizePolicy s_pfn_setSizePolicy = NULL;

static pfn_scroll_ctor s_pfn_scroll_ctor = NULL;
static pfn_setWidget s_pfn_setWidget = NULL;
static pfn_setWidgetResizable s_pfn_setWidgetResizable = NULL;
static pfn_setFrameShape s_pfn_setFrameShape = NULL;
static pfn_setScrollBarPolicy s_pfn_setHScrollBarPolicy = NULL;
static pfn_setScrollBarPolicy s_pfn_setVScrollBarPolicy = NULL;
static pfn_geometry s_pfn_geometry = NULL;
static pfn_setGeometry s_pfn_setGeometry = NULL;
static pfn_lower s_pfn_lower = NULL;
static pfn_setScaledContents s_pfn_setScaledContents = NULL;
static pfn_pixmap_ctor_file s_pfn_pixmap_ctor_file = NULL;
static pfn_qicon_ctor_file s_pfn_qicon_ctor_file = NULL;
static pfn_qicon_ctor_pixmap s_pfn_qicon_ctor_pixmap = NULL;
static pfn_qicon_dtor s_pfn_qicon_dtor = NULL;
static pfn_setIcon s_pfn_setIcon = NULL;

static void* s_hero_backdrop = NULL;
static char s_last_hero_title[256] = {0};
static int s_last_backdrop_w = 0;
static char s_hero_backdrop_pix[64] = {0};
static int s_hero_backdrop_pix_inited = 0;

static pfn_removeWidget s_pfn_removeWidget = NULL;
static pfn_insertWidget s_pfn_insertWidget = NULL;
static pfn_addWidget s_pfn_addWidget = NULL;
static pfn_hbox_ctor s_pfn_hbox_ctor = NULL;
static pfn_setContentsMargins s_pfn_setContentsMargins = NULL;
static pfn_setSpacing s_pfn_setSpacing = NULL;

static pfn_lbl_ctor s_pfn_lbl_ctor = NULL;
static pfn_setAttribute s_pfn_setAttribute = NULL;
static pfn_setFixedWidth s_pfn_setFixedWidth = NULL;
static pfn_setFixedHeight s_pfn_setFixedHeight = NULL;
static pfn_setAlignment s_pfn_setAlignment = NULL;
static pfn_setWordWrap s_pfn_setWordWrap = NULL;
static pfn_hide s_pfn_hide = NULL;
static pfn_show s_pfn_show = NULL;
static pfn_setObjectName s_pfn_setObjectName = NULL;
static pfn_children s_pfn_children = NULL;

static pfn_btn_ctor s_pfn_btn_ctor = NULL;
static pfn_toolbtn_ctor s_pfn_toolbtn_ctor = NULL;
static pfn_btn_click s_pfn_btn_click = NULL;
static pfn_is_visible s_pfn_isVisible = NULL;
static pfn_event_type s_pfn_event_type = NULL;
static pfn_cmevent_ctor s_pfn_cmevent_ctor = NULL;
static pfn_cmevent_dtor s_pfn_cmevent_dtor = NULL;
static pfn_send_event s_pfn_sendEvent = NULL;

static void* s_btn_configure = NULL;
static void* s_orig_btn_vtable = NULL;
static void* s_cfg_btn_vtable[256];
static volatile int s_direct_configure_game = 0;

static void EnsureQMenuActionFuncs(void) {
    if (!s_pfn_actions) {
        HMODULE hWidgets = GetModuleHandleW(L"Qt6Widgets.dll");
        if (hWidgets) {
            s_pfn_actions = (pfn_actions)GetProcAddress(hWidgets, "?actions@QWidget@@QEBA?AV?$QList@PEAVQAction@@@@XZ");
        }
    }
    if (!s_pfn_act_text || !s_pfn_act_trigger) {
        HMODULE hGui = GetModuleHandleW(L"Qt6Gui.dll");
        if (hGui) {
            if (!s_pfn_act_text) s_pfn_act_text = (pfn_act_text)GetProcAddress(hGui, "?iconText@QAction@@QEBA?AVQString@@XZ");
            if (!s_pfn_act_text) s_pfn_act_text = (pfn_act_text)GetProcAddress(hGui, "?text@QAction@@QEBA?AVQString@@XZ");
            if (!s_pfn_act_trigger) s_pfn_act_trigger = (pfn_act_trigger)GetProcAddress(hGui, "?trigger@QAction@@QEAAXXZ");
            if (!s_pfn_act_activate) s_pfn_act_activate = (pfn_act_activate)GetProcAddress(hGui, "?activate@QAction@@QEAAXW4ActionEvent@1@@Z");
        }
    }
    if (!s_pfn_toUtf8 || !s_pfn_constData || !s_pfn_dtor) {
        HMODULE hCore = GetModuleHandleW(L"Qt6Core.dll");
        if (hCore) {
            if (!s_pfn_toUtf8) s_pfn_toUtf8 = (pfn_toUtf8_fn)GetProcAddress(hCore, "?toUtf8@QString@@QEGBA?AVQByteArray@@XZ");
            if (!s_pfn_toUtf8) s_pfn_toUtf8 = (pfn_toUtf8_fn)GetProcAddress(hCore, "?toUtf8@QString@@QEHAA?AVQByteArray@@XZ");
            if (!s_pfn_constData) s_pfn_constData = (pfn_constData_fn)GetProcAddress(hCore, "?constData@QByteArray@@QEBAPEBDXZ");
            if (!s_pfn_dtor) s_pfn_dtor = (pfn_str_dtor_fn)GetProcAddress(hCore, "??1QString@@QEAA@XZ");
            if (!s_pfn_qba_dtor) s_pfn_qba_dtor = (pfn_qba_dtor_fn)GetProcAddress(hCore, "??1QByteArray@@QEAA@XZ");
        }
    }
}

static void* Hook_QMenu_exec(void* this_ptr, const void* pos, void* action) {
    if (s_direct_configure_game) {
        s_direct_configure_game = 0;
        EnsureQMenuActionFuncs();
        if (this_ptr && s_pfn_actions && (s_pfn_act_activate || s_pfn_act_trigger)) {
            struct {
                void* d;
                void** ptr;
                int64_t size;
            } act_list;
            memset(&act_list, 0, sizeof(act_list));
            s_pfn_actions(this_ptr, &act_list);
            if (act_list.ptr && act_list.size > 0) {
                void* target_act = NULL;
                for (int64_t a = 0; a < act_list.size; a++) {
                    void* act = act_list.ptr[a];
                    if (!act) continue;

                    int is_target = 0;
                    void* d_ptr = act ? *(void**)((char*)act + 8) : NULL;
                    if (d_ptr) {
                        const wchar_t* w1 = *(const wchar_t**)((char*)d_ptr + 0x88 + 8);
                        int64_t len1 = *(int64_t*)((char*)d_ptr + 0x88 + 16);
                        if (w1 && len1 > 0) {
                            if ((wcsstr(w1, L"Configure") || wcsstr(w1, L"Config") || wcsstr(w1, L"C\x1ea5u h\x00ecnh") || wcsstr(w1, L"C\xe0i \x0111\x1eb7t") || wcsstr(w1, L"Settings")) &&
                                !wcsstr(w1, L"without") && !wcsstr(w1, L"kh\xf4ng") && !wcsstr(w1, L"ri\xeang")) {
                                is_target = 1;
                            }
                        }
                    }
                    char act_qstr[32] = {0};
                    if (s_pfn_act_text) {
                        s_pfn_act_text(act, act_qstr);
                        const wchar_t* wtext = *(const wchar_t**)(act_qstr + 8);
                        int64_t wlen = *(int64_t*)(act_qstr + 16);
                        if (wtext && wlen > 0) {
                            if ((wcsstr(wtext, L"Configure") || wcsstr(wtext, L"Config") || wcsstr(wtext, L"C\x1ea5u h\x00ecnh") || wcsstr(wtext, L"C\xe0i \x0111\x1eb7t") || wcsstr(wtext, L"Settings")) &&
                                !wcsstr(wtext, L"without") && !wcsstr(wtext, L"kh\xf4ng") && !wcsstr(wtext, L"ri\xeang")) {
                                is_target = 1;
                            }
                        }
                    }

                    if (!is_target && s_pfn_toUtf8 && s_pfn_constData) {
                        char act_qba[32] = {0};
                        s_pfn_toUtf8(act_qstr, act_qba);
                        const char* act_text = s_pfn_constData(act_qba);
                        if (act_text && act_text[0] != '\0') {
                            if ((strstr(act_text, "Configure") || strstr(act_text, "Config") || strstr(act_text, "Cấu hình") || strstr(act_text, "C?u h") || strstr(act_text, "Settings") || strstr(act_text, "Cài đặt")) &&
                                !strstr(act_text, "without") && !strstr(act_text, "không") && !strstr(act_text, "kh?ng") && !strstr(act_text, "riêng")) {
                                is_target = 1;
                            }
                        }
                        if (s_pfn_qba_dtor) s_pfn_qba_dtor(act_qba);
                    }
                    if (s_pfn_dtor) s_pfn_dtor(act_qstr);

                    if (is_target) {
                        target_act = act;
                        break;
                    }
                }
                if (target_act) {
                    if (s_pfn_act_activate) {
                        s_pfn_act_activate(target_act, 0);
                    } else if (s_pfn_act_trigger) {
                        s_pfn_act_trigger(target_act);
                    }
                    return target_act;
                }
            }
        }
        return NULL;
    }
    return s_orig_QMenu_exec ? s_orig_QMenu_exec(this_ptr, pos, action) : NULL;
}

typedef bool (*pfn_event_orig)(void*, void*);
static bool ConfigureBtnEventHook(void* this_ptr, void* event_ptr) {
    if (!this_ptr || !event_ptr) return false;
    int ev_type = 0;
    if (s_pfn_event_type) {
        ev_type = s_pfn_event_type(event_ptr);
    }
    pfn_event_orig orig = s_orig_btn_vtable ? (pfn_event_orig)((void**)s_orig_btn_vtable)[4] : NULL;
    bool res = orig ? orig(this_ptr, event_ptr) : false;

    if (ev_type == 3) { // QEvent::MouseButtonRelease
        void* page = s_pfn_parentWidget ? s_pfn_parentWidget(this_ptr) : NULL;
        if (page && s_pfn_cmevent_ctor && s_pfn_sendEvent) {
            s_direct_configure_game = 1;
            int local_pt[2] = {0, 0};
            char cmevent_buf[128] = {0};
            s_pfn_cmevent_ctor(cmevent_buf, 0, local_pt);
            s_pfn_sendEvent(page, cmevent_buf);
            if (s_pfn_cmevent_dtor) s_pfn_cmevent_dtor(cmevent_buf);
            s_direct_configure_game = 0;
        }
    }
    return res;
}

static void* s_rail_btn_view = NULL;
static void* s_rail_btn_add_folder = NULL;
static void* s_btn_all_games = NULL;
static void* s_btn_add_folder = NULL;
static void* s_btn_library = NULL;
static void* s_fritchii_home_widget = NULL;

static void* s_orig_rail_btn_vtable = NULL;
static void* s_rail_btn_vtable[256];

static const char* const kClassicRailIcon[] = {
    "16 16 2 1", ". c None", "# c #FFF2E6",
    "................", "................",
    "..###.########..", "..###.########..",
    "................", "................",
    "..###.########..", "..###.########..",
    "................", "................",
    "..###.########..", "..###.########..",
    "................", "................",
    "................", "................",
};

static const char* const kModernRailIcon[] = {
    "16 16 2 1", ". c None", "# c #FFF2E6",
    "................", "................",
    "..#####..#####..", "..#...#..#...#..",
    "..#...#..#...#..", "..#...#..#...#..",
    "..#####..#####..", "................",
    "................", "..#####..#####..",
    "..#...#..#...#..", "..#...#..#...#..",
    "..#...#..#...#..", "..#####..#####..",
    "................", "................",
};

static const char* const kAddFolderRailIcon[] = {
    "16 16 2 1", ". c None", "# c #FFF2E6",
    "................", "................",
    "................", "..#####.........",
    "..#...########..", "..#..........#..",
    "..#....##....#..", "..#....##....#..",
    "..#..######..#..", "..#..######..#..",
    "..#....##....#..", "..#....##....#..",
    "..#..........#..", "..############..",
    "................", "................",
};

static void (*s_pfn_pixmap_ctor_xpm)(void*, const char* const*) = NULL;

static void SetRailButtonIcon(void* button, const char* const* xpm) {
    if (!s_pfn_pixmap_ctor_xpm || !s_pfn_dtor_pixmap || !s_pfn_qicon_ctor_pixmap ||
        !s_pfn_setIcon || !s_pfn_qicon_dtor) return;

    alignas(void*) char pixmap[64] = {0};
    alignas(void*) char icon[32] = {0};
    s_pfn_pixmap_ctor_xpm(pixmap, xpm);
    s_pfn_qicon_ctor_pixmap(icon, pixmap);
    s_pfn_setIcon(button, icon);
    s_pfn_qicon_dtor(icon);
    s_pfn_dtor_pixmap(pixmap);
}

static bool RailBtnEventHook(void* this_ptr, void* event_ptr) {
    if (!this_ptr || !event_ptr) return false;
    int ev_type = 0;
    if (s_pfn_event_type) {
        ev_type = s_pfn_event_type(event_ptr);
    }
    pfn_event_orig orig = s_orig_rail_btn_vtable ? (pfn_event_orig)((void**)s_orig_rail_btn_vtable)[4] : NULL;
    bool res = orig ? orig(this_ptr, event_ptr) : false;

    if (ev_type == 3) { // QEvent::MouseButtonRelease
        if (this_ptr == s_rail_btn_add_folder) {
            if (s_btn_add_folder && s_pfn_btn_click) {
                s_pfn_btn_click(s_btn_add_folder);
            }
        } else if (this_ptr == s_rail_btn_view) {
            int is_modern = 1;
            if (s_fritchii_home_widget && s_pfn_isVisible) {
                is_modern = s_pfn_isVisible(s_fritchii_home_widget);
            }
            if (is_modern) {
                if (s_btn_all_games && s_pfn_btn_click) {
                    s_pfn_btn_click(s_btn_all_games);
                }
            } else {
                if (s_btn_library && s_pfn_btn_click) {
                    s_pfn_btn_click(s_btn_library);
                }
            }
        }
    }
    return res;
}

static void* s_game_scroll_area = NULL;

typedef struct {
    char title[256];
    char program_id[32];
    char file_type[32];
    char file_size[32];
    char play_time[64];
    char add_ons[128];
} GameMetaCache;

static GameMetaCache s_game_meta[128];
static int s_game_meta_count = 0;

static void ParseGameItemTitleAndId(char* raw_text, char* out_title, size_t title_sz, char* out_id, size_t id_sz) {
    if (!raw_text || raw_text[0] == '\0') {
        if (out_title && title_sz > 0) out_title[0] = '\0';
        if (out_id && id_sz > 0) out_id[0] = '\0';
        return;
    }
    char* nl = strchr(raw_text, '\n');
    if (nl) {
        *nl = '\0';
        char* pid = nl + 1;
        while (*pid == ' ' || *pid == '\r' || *pid == '\t') pid++;
        if (pid[0] == '0' && (pid[1] == 'x' || pid[1] == 'X')) pid += 2;
        char* end = pid + strlen(pid) - 1;
        while (end >= pid && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')) {
            *end = '\0';
            end--;
        }
        if (out_id && id_sz > 0) {
            strncpy(out_id, pid, id_sz - 1);
            out_id[id_sz - 1] = '\0';
            for (char* p = out_id; *p; p++) {
                if (*p >= 'a' && *p <= 'f') *p = *p - 'a' + 'A';
            }
        }
    } else {
        if (out_id && id_sz > 0) out_id[0] = '\0';
    }
    char* tend = raw_text + strlen(raw_text) - 1;
    while (tend >= raw_text && (*tend == '\r' || *tend == '\n' || *tend == ' ' || *tend == '\t')) {
        *tend = '\0';
        tend--;
    }
    if (out_title && title_sz > 0) {
        strncpy(out_title, raw_text, title_sz - 1);
        out_title[title_sz - 1] = '\0';
    }
}

static void HelperGetItemRoleText(void* item, int role, char* out_buf, size_t buf_size) {
    if (!item || !s_pfn_qitem_data || !s_pfn_qvariant_toString || !s_pfn_toUtf8 || !s_pfn_constData) {
        if (out_buf && buf_size > 0) out_buf[0] = '\0';
        return;
    }
    char qvar[64] = {0};
    s_pfn_qitem_data(item, qvar, role);
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

static uint64_t HelperGetItemRoleULongLong(void* item, int role) {
    if (!item || !s_pfn_qitem_data || !s_pfn_toULongLong) return 0;
    char qvar[64] = {0};
    s_pfn_qitem_data(item, qvar, role);
    uint64_t val = s_pfn_toULongLong(qvar, NULL);
    if (s_pfn_qvariant_dtor) s_pfn_qvariant_dtor(qvar);
    return val;
}

static void HelperGetItemText(void* item, char* out_buf, size_t buf_size) {
    HelperGetItemRoleText(item, 0, out_buf, buf_size);
}

static void LogDebugMeta(const char* fmt, ...) { (void)fmt; }

static void PopulateGameMeta(void* tree_view) {
    if (!tree_view || !s_pfn_view_model || !s_pfn_model_root) return;
    void* model = s_pfn_view_model(tree_view);
    if (!model) return;

    // Fix 1.1: Unwrap proxy models (QSortFilterProxyModel / GamelistSearchFilterProxyModel)
    int unwrap_depth = 0;
    while (model && s_pfn_inherits && s_pfn_inherits(model, "QAbstractProxyModel") && s_pfn_sourceModel && unwrap_depth++ < 8) {
        model = s_pfn_sourceModel(model);
    }
    LogDebugMeta("PopulateGameMeta: model=%p, unwrapped=%p, inherits_std=%d", s_pfn_view_model(tree_view), model, s_pfn_inherits(model, "QStandardItemModel"));
    if (!model || !s_pfn_inherits || !s_pfn_inherits(model, "QStandardItemModel")) return;

    void* root = s_pfn_model_root(model);
    if (!root || !s_pfn_qitem_rowCount || !s_pfn_qitem_child) return;

    int top_rows = s_pfn_qitem_rowCount(root);
    if (top_rows <= 0) return;

    int new_count = 0;
    GameMetaCache temp_meta[128];
    memset(temp_meta, 0, sizeof(temp_meta));

    for (int r = 0; r < top_rows && new_count < 128; r++) {
        void* top_item = s_pfn_qitem_child(root, r, 0);
        if (!top_item) continue;
        int sub_rows = s_pfn_qitem_rowCount(top_item);
        if (sub_rows > 0) {
            for (int sub = 0; sub < sub_rows && new_count < 128; sub++) {
                void* game_item = s_pfn_qitem_child(top_item, sub, 0);
                if (!game_item) continue;

                GameMetaCache* entry = &temp_meta[new_count];
                memset(entry, 0, sizeof(GameMetaCache));

                // 1. Title from TitleRole (259)
                HelperGetItemRoleText(game_item, 259, entry->title, sizeof(entry->title));
                if (entry->title[0] == '\0') {
                    char full_p[MAX_PATH] = {0};
                    HelperGetItemRoleText(game_item, 260, full_p, sizeof(full_p));
                    if (full_p[0] != '\0') {
                        char* fname = strrchr(full_p, '\\');
                        if (!fname) fname = strrchr(full_p, '/');
                        fname = fname ? fname + 1 : full_p;
                        char* dot = strrchr(fname, '.');
                        if (dot) *dot = '\0';
                        strncpy(entry->title, fname, sizeof(entry->title) - 1);
                    }
                }
                if (entry->title[0] == '\0') {
                    char raw_t[256] = {0};
                    HelperGetItemText(game_item, raw_t, sizeof(raw_t));
                    ParseGameItemTitleAndId(raw_t, entry->title, sizeof(entry->title), entry->program_id, sizeof(entry->program_id));
                }

                // 2. Program ID from ProgramIdRole (261)
                if (entry->program_id[0] == '\0') {
                    uint64_t pid = HelperGetItemRoleULongLong(game_item, 261);
                    if (pid != 0) {
                        snprintf(entry->program_id, sizeof(entry->program_id), "%016llX", pid);
                    }
                }

                // 3. File Type from FileTypeRole (262)
                HelperGetItemRoleText(game_item, 262, entry->file_type, sizeof(entry->file_type));

                // 4. Sibling columns (1 .. 6)
                for (int col = 1; col <= 6; col++) {
                    void* sib = s_pfn_qitem_child(top_item, sub, col);
                    if (!sib) continue;
                    char sib_text[128] = {0};
                    HelperGetItemRoleText(sib, 0, sib_text, sizeof(sib_text));
                    if (sib_text[0] == '\0') continue;

                    if (col == 1 && entry->file_type[0] == '\0') {
                        strncpy(entry->file_type, sib_text, sizeof(entry->file_type) - 1);
                    } else if (col == 2 && entry->file_size[0] == '\0') {
                        strncpy(entry->file_size, sib_text, sizeof(entry->file_size) - 1);
                    } else if (col == 3 && entry->play_time[0] == '\0') {
                        strncpy(entry->play_time, sib_text, sizeof(entry->play_time) - 1);
                    } else if (col == 4 && entry->add_ons[0] == '\0') {
                        strncpy(entry->add_ons, sib_text, sizeof(entry->add_ons) - 1);
                    } else if ((strstr(sib_text, "GB") || strstr(sib_text, "MB") || strstr(sib_text, "KB")) && entry->file_size[0] == '\0') {
                        strncpy(entry->file_size, sib_text, sizeof(entry->file_size) - 1);
                    } else if ((strchr(sib_text, ':') || strstr(sib_text, "Never") || strstr(sib_text, "Played") || strstr(sib_text, "Chưa")) && entry->play_time[0] == '\0') {
                        strncpy(entry->play_time, sib_text, sizeof(entry->play_time) - 1);
                    } else if ((strstr(sib_text, "Update") || strstr(sib_text, "DLC") || strstr(sib_text, "Patch")) && entry->add_ons[0] == '\0') {
                        strncpy(entry->add_ons, sib_text, sizeof(entry->add_ons) - 1);
                    }
                }

                if (entry->title[0] != '\0') {
                    LogDebugMeta("Found Game [%d]: title='%s', pid='%s', ftype='%s', size='%s', play='%s', add_ons='%s'",
                                 new_count, entry->title, entry->program_id, entry->file_type, entry->file_size, entry->play_time, entry->add_ons);
                    new_count++;
                }
            }
        } else {
            GameMetaCache* entry = &temp_meta[new_count];
            memset(entry, 0, sizeof(GameMetaCache));

            HelperGetItemRoleText(top_item, 259, entry->title, sizeof(entry->title));
            if (entry->title[0] == '\0') {
                char full_p[MAX_PATH] = {0};
                HelperGetItemRoleText(top_item, 260, full_p, sizeof(full_p));
                if (full_p[0] != '\0') {
                    char* fname = strrchr(full_p, '\\');
                    if (!fname) fname = strrchr(full_p, '/');
                    fname = fname ? fname + 1 : full_p;
                    char* dot = strrchr(fname, '.');
                    if (dot) *dot = '\0';
                    strncpy(entry->title, fname, sizeof(entry->title) - 1);
                }
            }
            if (entry->title[0] == '\0') {
                char raw_t[256] = {0};
                HelperGetItemText(top_item, raw_t, sizeof(raw_t));
                ParseGameItemTitleAndId(raw_t, entry->title, sizeof(entry->title), entry->program_id, sizeof(entry->program_id));
            }

            if (entry->program_id[0] == '\0') {
                uint64_t pid = HelperGetItemRoleULongLong(top_item, 261);
                if (pid != 0) {
                    snprintf(entry->program_id, sizeof(entry->program_id), "%016llX", pid);
                }
            }
            HelperGetItemRoleText(top_item, 262, entry->file_type, sizeof(entry->file_type));

            if (s_pfn_model_item) {
                for (int col = 1; col <= 6; col++) {
                    void* sib = s_pfn_model_item(model, r, col);
                    if (!sib) continue;
                    char sib_text[128] = {0};
                    HelperGetItemRoleText(sib, 0, sib_text, sizeof(sib_text));
                    if (sib_text[0] == '\0') continue;

                    if ((strstr(sib_text, "GB") || strstr(sib_text, "MB") || strstr(sib_text, "KB")) && entry->file_size[0] == '\0') {
                        strncpy(entry->file_size, sib_text, sizeof(entry->file_size) - 1);
                    }
                    else if ((strchr(sib_text, ':') || strstr(sib_text, "Never") || strstr(sib_text, "Played") || strstr(sib_text, "Chưa")) && entry->play_time[0] == '\0') {
                        strncpy(entry->play_time, sib_text, sizeof(entry->play_time) - 1);
                    }
                    else if ((strstr(sib_text, "Update") || strstr(sib_text, "DLC") || strstr(sib_text, "Patch")) && entry->add_ons[0] == '\0') {
                        strncpy(entry->add_ons, sib_text, sizeof(entry->add_ons) - 1);
                    }
                    else if ((strcmp(sib_text, "NSP") == 0 || strcmp(sib_text, "XCI") == 0 || strcmp(sib_text, "NUP") == 0) && entry->file_type[0] == '\0') {
                        strncpy(entry->file_type, sib_text, sizeof(entry->file_type) - 1);
                    }
                }
            }

            if (entry->title[0] != '\0') {
                new_count++;
            }
        }
    }

    if (new_count > 0) {
        memcpy(s_game_meta, temp_meta, sizeof(temp_meta));
        s_game_meta_count = new_count;
    }
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
        s_pfn_geometry = (pfn_geometry)GetProcAddress(hWidgets, "?geometry@QWidget@@QEBAAEBVQRect@@XZ");
        s_pfn_setGeometry = (pfn_setGeometry)GetProcAddress(hWidgets, "?setGeometry@QWidget@@QEAAXHHHH@Z");
        s_pfn_lower = (pfn_lower)GetProcAddress(hWidgets, "?lower@QWidget@@QEAAXXZ");
        s_pfn_setScaledContents = (pfn_setScaledContents)GetProcAddress(hWidgets, "?setScaledContents@QLabel@@QEAAX_N@Z");
        s_pfn_setIcon = (pfn_setIcon)GetProcAddress(hWidgets, "?setIcon@QAbstractButton@@QEAAXAEBVQIcon@@@Z");
        if (hGui) {
            s_pfn_pixmap_ctor_file = (pfn_pixmap_ctor_file)GetProcAddress(hGui, "??0QPixmap@@QEAA@AEBVQString@@PEBDV?$QFlags@W4ImageConversionFlag@Qt@@@@@Z");
            s_pfn_qicon_ctor_file = (pfn_qicon_ctor_file)GetProcAddress(hGui, "??0QIcon@@QEAA@AEBVQString@@@Z");
            s_pfn_qicon_ctor_pixmap = (pfn_qicon_ctor_pixmap)GetProcAddress(hGui, "??0QIcon@@QEAA@AEBVQPixmap@@@Z");
            s_pfn_pixmap_ctor_xpm = (decltype(s_pfn_pixmap_ctor_xpm))GetProcAddress(hGui, "??0QPixmap@@QEAA@QEBQEBD@Z");
            s_pfn_qicon_dtor = (pfn_qicon_dtor)GetProcAddress(hGui, "??1QIcon@@QEAA@XZ");
        }
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

        s_pfn_sourceModel = (pfn_sourceModel)GetProcAddress(hCore, "?sourceModel@QAbstractProxyModel@@QEBAPEAVQAbstractItemModel@@XZ");
        s_pfn_toULongLong = (pfn_toULongLong)GetProcAddress(hCore, "?toULongLong@QVariant@@QEBA_KPEA_N@Z");
        s_pfn_layout_item_setAlign = (pfn_layout_item_setAlign)GetProcAddress(hWidgets, "?setAlignment@QLayoutItem@@QEAAXV?$QFlags@W4AlignmentFlag@Qt@@@@@Z");
        s_pfn_layout_setAlign_widget = (pfn_layout_setAlign_widget)GetProcAddress(hWidgets, "?setAlignment@QLayout@@QEAA_NPEAVQWidget@@V?$QFlags@W4AlignmentFlag@Qt@@@@@Z");
        s_pfn_setSizePolicy = (pfn_setSizePolicy)GetProcAddress(hWidgets, "?setSizePolicy@QWidget@@QEAAXW4Policy@QSizePolicy@@0@Z");

        s_pfn_scroll_ctor = (pfn_scroll_ctor)GetProcAddress(hWidgets, "??0QScrollArea@@QEAA@PEAVQWidget@@@Z");
        s_pfn_setWidget = (pfn_setWidget)GetProcAddress(hWidgets, "?setWidget@QScrollArea@@QEAAXPEAVQWidget@@@Z");
        s_pfn_setWidgetResizable = (pfn_setWidgetResizable)GetProcAddress(hWidgets, "?setWidgetResizable@QScrollArea@@QEAAX_N@Z");
        s_pfn_setFrameShape = (pfn_setFrameShape)GetProcAddress(hWidgets, "?setFrameShape@QFrame@@QEAAXW4Shape@1@@Z");
        s_pfn_setHScrollBarPolicy = (pfn_setScrollBarPolicy)GetProcAddress(hWidgets, "?setHorizontalScrollBarPolicy@QAbstractScrollArea@@QEAAXW4ScrollBarPolicy@Qt@@@Z");
        s_pfn_setVScrollBarPolicy = (pfn_setScrollBarPolicy)GetProcAddress(hWidgets, "?setVerticalScrollBarPolicy@QAbstractScrollArea@@QEAAXW4ScrollBarPolicy@Qt@@@Z");

        s_pfn_removeWidget = (pfn_removeWidget)GetProcAddress(hWidgets, "?removeWidget@QLayout@@QEAAXPEAVQWidget@@@Z");
        s_pfn_insertWidget = (pfn_insertWidget)GetProcAddress(hWidgets, "?insertWidget@QBoxLayout@@QEAAXHPEAVQWidget@@HV?$QFlags@W4AlignmentFlag@Qt@@@@@Z");
        s_pfn_addWidget = (pfn_addWidget)GetProcAddress(hWidgets, "?addWidget@QBoxLayout@@QEAAXPEAVQWidget@@HV?$QFlags@W4AlignmentFlag@Qt@@@@@Z");
        s_pfn_hbox_ctor = (pfn_hbox_ctor)GetProcAddress(hWidgets, "??0QHBoxLayout@@QEAA@PEAVQWidget@@@Z");
        s_pfn_setContentsMargins = (pfn_setContentsMargins)GetProcAddress(hWidgets, "?setContentsMargins@QLayout@@QEAAXHHHH@Z");
        s_pfn_setSpacing = (pfn_setSpacing)GetProcAddress(hWidgets, "?setSpacing@QBoxLayout@@UEAAXH@Z");

        s_pfn_lbl_ctor = (pfn_lbl_ctor)GetProcAddress(hWidgets, "??0QLabel@@QEAA@PEAVQWidget@@V?$QFlags@W4WindowType@Qt@@@@@Z");
        s_pfn_setAttribute = (pfn_setAttribute)GetProcAddress(hWidgets, "?setAttribute@QWidget@@QEAAXW4WidgetAttribute@Qt@@_N@Z");
        s_pfn_setFixedWidth = (pfn_setFixedWidth)GetProcAddress(hWidgets, "?setFixedWidth@QWidget@@QEAAXH@Z");
        s_pfn_setFixedHeight = (pfn_setFixedHeight)GetProcAddress(hWidgets, "?setFixedHeight@QWidget@@QEAAXH@Z");
        s_pfn_setAlignment = (pfn_setAlignment)GetProcAddress(hWidgets, "?setAlignment@QLabel@@QEAAXV?$QFlags@W4AlignmentFlag@Qt@@@@@Z");
        s_pfn_setWordWrap = (pfn_setWordWrap)GetProcAddress(hWidgets, "?setWordWrap@QLabel@@QEAAX_N@Z");
        s_pfn_hide = (pfn_hide)GetProcAddress(hWidgets, "?hide@QWidget@@QEAAXXZ");
        s_pfn_show = (pfn_show)GetProcAddress(hWidgets, "?show@QWidget@@QEAAXXZ");
        s_pfn_setObjectName = (pfn_setObjectName)GetProcAddress(hCore, "?setObjectName@QObject@@QEAAXAEBVQString@@@Z");
        s_pfn_children = (pfn_children)GetProcAddress(hCore, "?children@QObject@@QEBAAEBV?$QList@PEAVQObject@@@@XZ");
        s_pfn_btn_ctor = (pfn_btn_ctor)GetProcAddress(hWidgets, "??0QPushButton@@QEAA@PEAVQWidget@@@Z");
        s_pfn_toolbtn_ctor = (pfn_toolbtn_ctor)GetProcAddress(hWidgets, "??0QToolButton@@QEAA@PEAVQWidget@@@Z");
        s_pfn_btn_click = (pfn_btn_click)GetProcAddress(hWidgets, "?click@QAbstractButton@@QEAAXXZ");
        s_pfn_isVisible = (pfn_is_visible)GetProcAddress(hWidgets, "?isVisible@QWidget@@QEBA_NXZ");
        s_pfn_setIconSize = (pfn_setIconSize)GetProcAddress(hWidgets, "?setIconSize@QAbstractButton@@QEAAXAEBVQSize@@@Z");
        if (hGui) {
            s_pfn_act_trigger = (pfn_act_trigger)GetProcAddress(hGui, "?trigger@QAction@@QEAAXXZ");
        }
        if (hGui) {
            s_pfn_cmevent_ctor = (pfn_cmevent_ctor)GetProcAddress(hGui, "??0QContextMenuEvent@@QEAA@W4Reason@0@AEBVQPoint@@@Z");
            s_pfn_cmevent_dtor = (pfn_cmevent_dtor)GetProcAddress(hGui, "??1QContextMenuEvent@@UEAA@XZ");
        }
        s_pfn_sendEvent = (pfn_send_event)GetProcAddress(hCore, "?sendEvent@QCoreApplication@@SA_NPEAVQObject@@PEAVQEvent@@@Z");
        s_pfn_event_type = (pfn_event_type)GetProcAddress(hCore, "?type@QEvent@@QEBA?AW4Type@1@XZ");
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
        // Pass 1: Extract game metadata from QTreeView first so it is available for all tiles
        for (int64_t i = 0; i < widget_list.size; i++) {
            void* w = widget_list.ptr[i];
            if (!w) continue;
            if (s_pfn_inherits(w, "QTreeView")) {
                PopulateGameMeta(w);
                break;
            }
        }

        // Pass 2: Detect hero title and style hero header & backdrop
        char cur_hero_title[256] = {0};
        void* hero_title_widget = NULL;
        void* fritchii_home_widget = NULL;

        for (int64_t i = 0; i < widget_list.size; i++) {
            void* w = widget_list.ptr[i];
            if (!w) continue;
            char oname[128] = {0};
            if (s_pfn_objectName) {
                char qstr_n[32] = {0};
                s_pfn_objectName(w, qstr_n);
                char qba_n[32] = {0};
                s_pfn_toUtf8(qstr_n, qba_n);
                const char* s = s_pfn_constData(qba_n);
                if (s) strncpy(oname, s, sizeof(oname) - 1);
                if (s_pfn_qba_dtor) s_pfn_qba_dtor(qba_n);
                s_pfn_dtor(qstr_n);
            }
            if (strcmp(oname, "FritchiiHeroTitle") == 0) {
                hero_title_widget = w;
                if (s_pfn_lbl_text) {
                    char q_ht[32] = {0};
                    s_pfn_lbl_text(w, q_ht);
                    char q_ba[32] = {0};
                    s_pfn_toUtf8(q_ht, q_ba);
                    const char* t = s_pfn_constData(q_ba);
                    if (t) strncpy(cur_hero_title, t, sizeof(cur_hero_title) - 1);
                    if (s_pfn_qba_dtor) s_pfn_qba_dtor(q_ba);
                    s_pfn_dtor(q_ht);
                }
            } else if (strcmp(oname, "FritchiiHome") == 0) {
                fritchii_home_widget = w;
            }
        }

        GameMetaCache* hero_meta = NULL;
        for (int m = 0; m < s_game_meta_count; m++) {
            if (cur_hero_title[0] != '\0' &&
                (strcmp(s_game_meta[m].title, cur_hero_title) == 0 ||
                 strstr(s_game_meta[m].title, cur_hero_title) != NULL ||
                 strstr(cur_hero_title, s_game_meta[m].title) != NULL)) {
                hero_meta = &s_game_meta[m];
                break;
            }
        }
        const char* hero_pid = (hero_meta && hero_meta->program_id[0]) ? hero_meta->program_id : "";

        if (fritchii_home_widget) {
            void* page = fritchii_home_widget;
            void* page_layout = s_pfn_layout ? s_pfn_layout(page) : NULL;
            if (page_layout) {
                if (s_pfn_setContentsMargins) s_pfn_setContentsMargins(page_layout, 56, 36, 56, 20);
                if (s_pfn_setSpacing) s_pfn_setSpacing(page_layout, 72);
            }

            int page_w = 1280;
            if (s_pfn_geometry) {
                const int* r = (const int*)s_pfn_geometry(page);
                if (r) {
                    int w_calc = r[2] - r[0] + 1;
                    if (w_calc > 400 && w_calc < 5000) page_w = w_calc;
                }
            }

            int backdrop_x = 32;
            int backdrop_y = 14;
            int backdrop_w = page_w - 64;
            int backdrop_h = 330;

            if (!s_hero_backdrop && s_pfn_lbl_ctor) {
                s_hero_backdrop = malloc(512);
                if (s_hero_backdrop) {
                    memset(s_hero_backdrop, 0, 512);
                    s_pfn_lbl_ctor(s_hero_backdrop, page, 0);
                    if (s_pfn_setObjectName) {
                        char q_on[32] = {0};
                        s_pfn_fromUtf8_str(q_on, "FritchiiHeroBackdrop", -1);
                        s_pfn_setObjectName(s_hero_backdrop, q_on);
                        s_pfn_dtor(q_on);
                    }
                    if (s_pfn_setAttribute) s_pfn_setAttribute(s_hero_backdrop, 51, 1);
                    if (s_pfn_setScaledContents) s_pfn_setScaledContents(s_hero_backdrop, 1);
                    if (s_pfn_setStyleSheet) {
                        char q_css[32] = {0};
                        s_pfn_fromUtf8_str(q_css, "QLabel#FritchiiHeroBackdrop { background: transparent; }", -1);
                        s_pfn_setStyleSheet(s_hero_backdrop, q_css);
                        s_pfn_dtor(q_css);
                    }
                }
            }

            if (s_hero_backdrop) {
                if (s_pfn_setGeometry) s_pfn_setGeometry(s_hero_backdrop, backdrop_x, backdrop_y, backdrop_w, backdrop_h);
                if (s_pfn_lower) s_pfn_lower(s_hero_backdrop);
                if (s_pfn_show) s_pfn_show(s_hero_backdrop);

                if (force || lang_changed || mode_changed ||
                    strcmp(s_last_hero_title, cur_hero_title) != 0 ||
                    abs(s_last_backdrop_w - backdrop_w) > 4 ||
                    !s_hero_backdrop_pix_inited) {

                    wchar_t exe_dir[MAX_PATH] = {0};
                    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
                    PathRemoveFileSpecW(exe_dir);

                    wchar_t banner_src[MAX_PATH] = {0};
                    if (hero_pid[0] != '\0') {
                        swprintf(banner_src, MAX_PATH, L"%s\\covers\\%S_banner.png", exe_dir, hero_pid);
                        if (GetFileAttributesW(banner_src) == INVALID_FILE_ATTRIBUTES) {
                            swprintf(banner_src, MAX_PATH, L"%s\\covers\\%S.png", exe_dir, hero_pid);
                        }
                    }
                    if (banner_src[0] == L'\0' || GetFileAttributesW(banner_src) == INVALID_FILE_ATTRIBUTES) {
                        swprintf(banner_src, MAX_PATH, L"%s\\hero_default.png", exe_dir);
                    }

                    wchar_t backdrop_out[MAX_PATH];
                    swprintf(backdrop_out, MAX_PATH, L"%s\\covers\\current_hero_backdrop.png", exe_dir);

                    if (GenerateBlurredHeroBackdrop(banner_src, backdrop_out, backdrop_w, backdrop_h)) {
                        char utf8_path[MAX_PATH * 2] = {0};
                        WideCharToMultiByte(CP_UTF8, 0, backdrop_out, -1, utf8_path, sizeof(utf8_path), NULL, NULL);

                        if (s_hero_backdrop_pix_inited && s_pfn_dtor_pixmap) {
                            s_pfn_dtor_pixmap(s_hero_backdrop_pix);
                            s_hero_backdrop_pix_inited = 0;
                        }

                        if (s_pfn_pixmap_ctor_file && s_pfn_setPixmap) {
                            char q_pstr[32] = {0};
                            s_pfn_fromUtf8_str(q_pstr, utf8_path, -1);
                            s_pfn_pixmap_ctor_file(s_hero_backdrop_pix, q_pstr, NULL, 0);
                            s_pfn_dtor(q_pstr);
                            s_hero_backdrop_pix_inited = 1;
                            s_pfn_setPixmap(s_hero_backdrop, s_hero_backdrop_pix);
                        }
                    }
                    strncpy(s_last_hero_title, cur_hero_title, sizeof(s_last_hero_title) - 1);
                    s_last_backdrop_w = backdrop_w;
                }
            }

            const Qt6List* page_children = s_pfn_children ? s_pfn_children(page) : NULL;
            if (page_children && page_children->ptr && page_children->size > 0) {
                for (int64_t c = 0; c < page_children->size; c++) {
                    void* child = page_children->ptr[c];
                    if (!child) continue;
                    if (s_pfn_inherits(child, "QLabel")) {
                        char cname[128] = {0};
                        if (s_pfn_objectName) {
                            char qstr_n[32] = {0};
                            s_pfn_objectName(child, qstr_n);
                            char qba_n[32] = {0};
                            s_pfn_toUtf8(qstr_n, qba_n);
                            const char* s = s_pfn_constData(qba_n);
                            if (s) strncpy(cname, s, sizeof(cname) - 1);
                            if (s_pfn_qba_dtor) s_pfn_qba_dtor(qba_n);
                            s_pfn_dtor(qstr_n);
                        }
                        if (strcmp(cname, "FritchiiHeroTitle") != 0 && strcmp(cname, "FritchiiHeroBackdrop") != 0) {
                            if (cname[0] == '\0' && s_pfn_setObjectName) {
                                char q_on[32] = {0};
                                s_pfn_fromUtf8_str(q_on, "FritchiiHeroCover", -1);
                                s_pfn_setObjectName(child, q_on);
                                s_pfn_dtor(q_on);
                            }
                            if (s_pfn_setFixedWidth) s_pfn_setFixedWidth(child, 200);
                            if (s_pfn_setFixedHeight) s_pfn_setFixedHeight(child, 200);
                            if (s_pfn_setScaledContents) s_pfn_setScaledContents(child, 1);
                            if (s_pfn_setStyleSheet) {
                                char q_css[32] = {0};
                                s_pfn_fromUtf8_str(q_css, "QLabel { background: transparent; }", -1);
                                s_pfn_setStyleSheet(child, q_css);
                                s_pfn_dtor(q_css);
                            }

                            if (hero_pid[0] != '\0') {
                                wchar_t exe_dir[MAX_PATH] = {0};
                                GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
                                PathRemoveFileSpecW(exe_dir);
                                wchar_t cov_src[MAX_PATH] = {0};
                                swprintf(cov_src, MAX_PATH, L"%s\\covers\\%S_rounded.png", exe_dir, hero_pid);
                                if (GetFileAttributesW(cov_src) == INVALID_FILE_ATTRIBUTES) {
                                    swprintf(cov_src, MAX_PATH, L"%s\\covers\\%S.png", exe_dir, hero_pid);
                                }
                                if (GetFileAttributesW(cov_src) != INVALID_FILE_ATTRIBUTES) {
                                    char utf8_cov[MAX_PATH * 2] = {0};
                                    WideCharToMultiByte(CP_UTF8, 0, cov_src, -1, utf8_cov, sizeof(utf8_cov), NULL, NULL);
                                    if (s_pfn_pixmap_ctor_file && s_pfn_setPixmap && s_pfn_dtor_pixmap) {
                                        char pix_tmp[64] = {0};
                                        char q_pstr[32] = {0};
                                        s_pfn_fromUtf8_str(q_pstr, utf8_cov, -1);
                                        s_pfn_pixmap_ctor_file(pix_tmp, q_pstr, NULL, 0);
                                        s_pfn_dtor(q_pstr);
                                        s_pfn_setPixmap(child, pix_tmp);
                                        s_pfn_dtor_pixmap(pix_tmp);
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
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

                        // Status bar buttons styled to active theme
            if (strcmp(obj_name_buf, "OotFpsStatusBarButton") == 0 ||
                strcmp(obj_name_buf, "TogglableStatusBarButton") == 0 ||
                strcmp(obj_name_buf, "RendererStatusBarButton") == 0 ||
                strcmp(obj_name_buf, "GPUStatusBarButton") == 0 ||
                strcmp(obj_name_buf, "DockingStatusBarButton") == 0) {
                if (s_pfn_setStyleSheet) {
                    char sb_css[256];
                    snprintf(sb_css, sizeof(sb_css),
                        "QPushButton { background: transparent; border: none; color: %s; padding: 3px 10px; border-radius: 6px; font-weight: 600; } "
                        "QPushButton:hover { background: #1f1f24; color: #ececf1; }",
                        g_palettes[g_theme_idx].accent);
                    char q_css[32] = {0};
                    s_pfn_fromUtf8_str(q_css, sb_css, -1);
                    s_pfn_setStyleSheet(w, q_css);
                    s_pfn_dtor(q_css);
                }
            }

            // Play button with active theme accent & Settings button
            if (strcmp(obj_name_buf, "FritchiiPlay") == 0) {
                if (s_pfn_setStyleSheet) {
                    char play_css[512];
                    snprintf(play_css, sizeof(play_css),
                        "QPushButton#FritchiiPlay { background-color: %s; color: %s; border: none; border-radius: 8px; min-height: 44px; max-height: 44px; padding: 0 32px; font-size: 15px; font-weight: 700; } "
                        "QPushButton#FritchiiPlay:hover { background-color: %s; } "
                        "QPushButton#FritchiiPlay:pressed { background-color: %s; }",
                        g_palettes[g_theme_idx].accent,
                        g_palettes[g_theme_idx].text_on_accent,
                        g_palettes[g_theme_idx].accent_light,
                        g_palettes[g_theme_idx].accent_dark);
                    char q_css[32] = {0};
                    s_pfn_fromUtf8_str(q_css, play_css, -1);
                    s_pfn_setStyleSheet(w, q_css);
                    s_pfn_dtor(q_css);
                }

                void* play_parent = s_pfn_parentWidget ? s_pfn_parentWidget(w) : NULL;
                if (play_parent && s_pfn_btn_ctor) {
                    if (!s_btn_configure) {
                        s_btn_configure = malloc(512);
                        if (s_btn_configure) {
                            memset(s_btn_configure, 0, 512);
                            s_pfn_btn_ctor(s_btn_configure, play_parent);
                            if (s_pfn_setObjectName) {
                                char q_on[32] = {0};
                                s_pfn_fromUtf8_str(q_on, "YngchiiGameConfigure", -1);
                                s_pfn_setObjectName(s_btn_configure, q_on);
                                s_pfn_dtor(q_on);
                            }
                            void** vt = *(void***)s_btn_configure;
                            if (vt) {
                                s_orig_btn_vtable = vt;
                                memcpy(s_cfg_btn_vtable, vt, sizeof(s_cfg_btn_vtable));
                                s_cfg_btn_vtable[4] = (void*)ConfigureBtnEventHook;
                                *(void***)s_btn_configure = s_cfg_btn_vtable;
                            }
                        }
                    }

                    if (s_btn_configure) {
                        int is_vn = g_lang_vn;
                        const char* btn_text = is_vn ? "⚙ Cài đặt" : "⚙ Settings";
                        if (s_pfn_btn_setText) {
                            char q_txt[32] = {0};
                            s_pfn_fromUtf8_str(q_txt, btn_text, -1);
                            s_pfn_btn_setText(s_btn_configure, q_txt);
                            s_pfn_dtor(q_txt);
                        }

                        if (s_pfn_setStyleSheet) {
                            char cfg_css[512];
                            snprintf(cfg_css, sizeof(cfg_css),
                                "QPushButton#YngchiiGameConfigure { "
                                "  background-color: #343837; "
                                "  color: #efead7; "
                                "  border: 1.5px solid #87867c; "
                                "  border-radius: 8px; "
                                "  min-height: 44px; max-height: 44px; "
                                "  padding: 0 20px; "
                                "  font-size: 15px; font-weight: 600; "
                                "} "
                                "QPushButton#YngchiiGameConfigure:hover { "
                                "  background-color: #424746; "
                                "  border-color: %s; "
                                "  color: #ffffff; "
                                "} "
                                "QPushButton#YngchiiGameConfigure:pressed { "
                                "  background-color: #282b2a; "
                                "}",
                                g_palettes[g_theme_idx].accent);
                            char q_css[32] = {0};
                            s_pfn_fromUtf8_str(q_css, cfg_css, -1);
                            s_pfn_setStyleSheet(s_btn_configure, q_css);
                            s_pfn_dtor(q_css);
                        }

                        if (s_pfn_geometry && s_pfn_setGeometry) {
                            const int* r = (const int*)s_pfn_geometry(w);
                            if (r) {
                                int play_x = r[0];
                                int play_y = r[1];
                                int play_w = r[2] - r[0] + 1;
                                int play_h = r[3] - r[1] + 1;
                                if (play_w > 0 && play_h > 0) {
                                    s_pfn_setGeometry(s_btn_configure, play_x + play_w + 14, play_y, 140, play_h);
                                    if (s_pfn_show) s_pfn_show(s_btn_configure);
                                }
                            }
                        }
                    }
                }
            }

            // Rail buttons with active theme accent
            if (strcmp(obj_name_buf, "FritchiiRailButton") == 0) {
                if (s_pfn_setStyleSheet) {
                    char rail_css[768];
                    const ThemePalette* palette = &g_palettes[g_theme_idx];
                    snprintf(rail_css, sizeof(rail_css),
                        "QToolButton#FritchiiRailButton { color: #fff2e6; background: #272225; border: 1px solid #564346; border-radius: 10px; padding: 8px; } "
                        "QToolButton#FritchiiRailButton:checked { background: #343837; border-color: %s; } "
                        "QToolButton#FritchiiRailButton:hover { background: #402a25; border-color: %s; } "
                        "QToolButton#FritchiiRailButton:pressed { background: #572e26; border-color: %s; }",
                        palette->accent, palette->accent_light, palette->accent_dark);
                    char q_css[32] = {0};
                    s_pfn_fromUtf8_str(q_css, rail_css, -1);
                    s_pfn_setStyleSheet(w, q_css);
                    s_pfn_dtor(q_css);
                }

                char rail_tip[128] = {0};
                if (s_pfn_toolTip) {
                    char q_tip[32] = {0};
                    s_pfn_toolTip(w, q_tip);
                    char q_ba[32] = {0};
                    s_pfn_toUtf8(q_tip, q_ba);
                    const char* ft = s_pfn_constData(q_ba);
                    if (ft && ft[0] != '\0') strncpy(rail_tip, ft, sizeof(rail_tip) - 1);
                    if (s_pfn_qba_dtor) s_pfn_qba_dtor(q_ba);
                    s_pfn_dtor(q_tip);
                }

                if (rail_tip[0] != '\0' && (strcmp(rail_tip, "Library") == 0 || strcmp(rail_tip, "Kho game") == 0 || strstr(rail_tip, "Library") || strstr(rail_tip, "Kho game") || strstr(rail_tip, "ThÆ° viá»‡n") || strstr(rail_tip, "Thư viện"))) {
                    s_btn_library = w;
                }

                // Add View Mode Switch & Add Folder buttons to Rail once
                void* rail_w = s_pfn_parentWidget ? s_pfn_parentWidget(w) : NULL;
                void* rail_layout = (rail_w && s_pfn_layout) ? s_pfn_layout(rail_w) : NULL;
                if (rail_layout && s_pfn_toolbtn_ctor && s_pfn_insertWidget) {
                    if (!s_rail_btn_view) {
                        s_rail_btn_view = malloc(512);
                        if (s_rail_btn_view) {
                            memset(s_rail_btn_view, 0, 512);
                            s_pfn_toolbtn_ctor(s_rail_btn_view, rail_w);
                            if (s_pfn_setObjectName) {
                                char q_on[32] = {0};
                                s_pfn_fromUtf8_str(q_on, "FritchiiRailButton", -1);
                                s_pfn_setObjectName(s_rail_btn_view, q_on);
                                s_pfn_dtor(q_on);
                            }
                            if (s_pfn_setIconSize) {
                                int qs[2] = {22, 22};
                                s_pfn_setIconSize(s_rail_btn_view, qs);
                            }
                            void** vt = *(void***)s_rail_btn_view;
                            if (vt) {
                                if (!s_orig_rail_btn_vtable) {
                                    s_orig_rail_btn_vtable = vt;
                                    memcpy(s_rail_btn_vtable, vt, sizeof(s_rail_btn_vtable));
                                    s_rail_btn_vtable[4] = (void*)RailBtnEventHook;
                                }
                                *(void***)s_rail_btn_view = s_rail_btn_vtable;
                            }
                            s_pfn_insertWidget(rail_layout, 1, s_rail_btn_view, 0, 0);
                        }
                    }

                    if (!s_rail_btn_add_folder) {
                        s_rail_btn_add_folder = malloc(512);
                        if (s_rail_btn_add_folder) {
                            memset(s_rail_btn_add_folder, 0, 512);
                            s_pfn_toolbtn_ctor(s_rail_btn_add_folder, rail_w);
                            if (s_pfn_setObjectName) {
                                char q_on[32] = {0};
                                s_pfn_fromUtf8_str(q_on, "FritchiiRailButton", -1);
                                s_pfn_setObjectName(s_rail_btn_add_folder, q_on);
                                s_pfn_dtor(q_on);
                            }
                            if (s_pfn_setIconSize) {
                                int qs[2] = {22, 22};
                                s_pfn_setIconSize(s_rail_btn_add_folder, qs);
                            }
                            void** vt = *(void***)s_rail_btn_add_folder;
                            if (vt) {
                                if (!s_orig_rail_btn_vtable) {
                                    s_orig_rail_btn_vtable = vt;
                                    memcpy(s_rail_btn_vtable, vt, sizeof(s_rail_btn_vtable));
                                    s_rail_btn_vtable[4] = (void*)RailBtnEventHook;
                                }
                                *(void***)s_rail_btn_add_folder = s_rail_btn_vtable;
                            }
                            SetRailButtonIcon(s_rail_btn_add_folder, kAddFolderRailIcon);
                            s_pfn_insertWidget(rail_layout, 2, s_rail_btn_add_folder, 0, 0);
                        }
                    }
                }

                // Update View button tooltip & icon based on current view
                if (s_rail_btn_view) {
                    int is_vn = g_lang_vn;
                    int is_modern = (s_fritchii_home_widget && s_pfn_isVisible) ? s_pfn_isVisible(s_fritchii_home_widget) : 1;
                    const char* view_tip = is_modern ?
                        (is_vn ? "Chuyển giao diện Danh sách cổ điển" : "Switch to Classic Table View") :
                        (is_vn ? "Chuyển giao diện Thẻ hiện đại" : "Switch to Modern Cards View");

                    if (s_pfn_toolTip && s_pfn_fromUtf8_str && s_pfn_dtor) {
                        char q_tip[64] = {0};
                        s_pfn_fromUtf8_str(q_tip, view_tip, -1);
                        // Using setToolTip on widget:
                        typedef void (*pfn_setToolTip)(void*, void*);
                        HMODULE hWidgets = GetModuleHandleW(L"Qt6Widgets.dll");
                        pfn_setToolTip pSetTip = hWidgets ? (pfn_setToolTip)GetProcAddress(hWidgets, "?setToolTip@QWidget@@QEAAXAEBVQString@@@Z") : NULL;
                        if (pSetTip) pSetTip(s_rail_btn_view, q_tip);
                        s_pfn_dtor(q_tip);
                    }
                    static int last_view_icon = -1;
                    if (last_view_icon != is_modern) {
                        SetRailButtonIcon(s_rail_btn_view, is_modern ? kClassicRailIcon : kModernRailIcon);
                        last_view_icon = is_modern;
                    }
                    if (s_pfn_show) s_pfn_show(s_rail_btn_view);
                }

                // Update Add Folder button tooltip & icon
                if (s_rail_btn_add_folder) {
                    int is_vn = g_lang_vn;
                    const char* add_tip = is_vn ? "Thêm thư mục game" : "Add Game Directory";
                    if (s_pfn_toolTip && s_pfn_fromUtf8_str && s_pfn_dtor) {
                        char q_tip[64] = {0};
                        s_pfn_fromUtf8_str(q_tip, add_tip, -1);
                        typedef void (*pfn_setToolTip)(void*, void*);
                        HMODULE hWidgets = GetModuleHandleW(L"Qt6Widgets.dll");
                        pfn_setToolTip pSetTip = hWidgets ? (pfn_setToolTip)GetProcAddress(hWidgets, "?setToolTip@QWidget@@QEAAXAEBVQString@@@Z") : NULL;
                        if (pSetTip) pSetTip(s_rail_btn_add_folder, q_tip);
                        s_pfn_dtor(q_tip);
                    }
                    if (s_pfn_show) s_pfn_show(s_rail_btn_add_folder);
                }

                if (rail_tip[0] != '\0' && (strcmp(rail_tip, "Library") == 0 || strcmp(rail_tip, "Kho game") == 0)) {
                    wchar_t exe_dir[MAX_PATH] = {0};
                    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
                    PathRemoveFileSpecW(exe_dir);
                    wchar_t lib_icon_path[MAX_PATH];
                    swprintf(lib_icon_path, MAX_PATH, L"%s\\covers\\rail_library_%d.png", exe_dir, g_theme_idx);
                    if (GetFileAttributesW(lib_icon_path) != INVALID_FILE_ATTRIBUTES) {
                        char utf8_lib[MAX_PATH * 2] = {0};
                        WideCharToMultiByte(CP_UTF8, 0, lib_icon_path, -1, utf8_lib, sizeof(utf8_lib), NULL, NULL);
                        if (s_pfn_qicon_ctor_file && s_pfn_setIcon && s_pfn_qicon_dtor) {
                            char q_lib[32] = {0};
                            s_pfn_fromUtf8_str(q_lib, utf8_lib, -1);
                            char q_ic[32] = {0};
                            s_pfn_qicon_ctor_file(q_ic, q_lib);
                            s_pfn_setIcon(w, q_ic);
                            s_pfn_qicon_dtor(q_ic);
                            s_pfn_dtor(q_lib);
                        }
                    }
                }
            }

            // Hero Title styling
            if (strcmp(obj_name_buf, "FritchiiHome") == 0) {
                s_fritchii_home_widget = w;
            }

            if (strcmp(obj_name_buf, "FritchiiHeroTitle") == 0) {
                if (s_pfn_setStyleSheet) {
                    const char* title_css = "QLabel#FritchiiHeroTitle { font-size: 26px; font-weight: 700; color: #ffffff; background: transparent; }";
                    char q_css[32] = {0};
                    s_pfn_fromUtf8_str(q_css, title_css, -1);
                    s_pfn_setStyleSheet(w, q_css);
                    s_pfn_dtor(q_css);
                }
            }

            // Modern Home List View on FritchiiTile
            if (strcmp(obj_name_buf, "FritchiiTile") == 0) {
                void* parent = s_pfn_parentWidget ? s_pfn_parentWidget(w) : NULL; // row_box
                void* layout = (parent && s_pfn_layout) ? s_pfn_layout(parent) : NULL; // row layout

                // 1. Wrap row_box in dedicated QScrollArea (YngchiiGameFrame) once
                if (!s_game_scroll_area && parent && s_pfn_scroll_ctor && s_pfn_setWidget) {
                    void* page = s_pfn_parentWidget ? s_pfn_parentWidget(parent) : NULL;
                    void* page_layout = (page && s_pfn_layout) ? s_pfn_layout(page) : NULL;
                    if (page_layout && s_pfn_removeWidget) {
                        s_pfn_removeWidget(page_layout, parent);

                        void* scroll = malloc(1024);
                        if (scroll) {
                            memset(scroll, 0, 1024);
                            s_pfn_scroll_ctor(scroll, page);
                            s_pfn_setWidget(scroll, parent);
                            if (s_pfn_setWidgetResizable) s_pfn_setWidgetResizable(scroll, 1);
                            if (s_pfn_setSizePolicy) s_pfn_setSizePolicy(scroll, 7, 7); // Expanding, Expanding
                            if (s_pfn_setFrameShape) s_pfn_setFrameShape(scroll, 0); // NoFrame
                            if (s_pfn_setHScrollBarPolicy) s_pfn_setHScrollBarPolicy(scroll, 1); // ScrollBarAlwaysOff
                            if (s_pfn_setVScrollBarPolicy) s_pfn_setVScrollBarPolicy(scroll, 0); // ScrollBarAsNeeded

                            if (s_pfn_setObjectName) {
                                char q_on[32] = {0};
                                s_pfn_fromUtf8_str(q_on, "YngchiiGameFrame", -1);
                                s_pfn_setObjectName(scroll, q_on);
                                s_pfn_dtor(q_on);
                            }

                            if (s_pfn_setStyleSheet) {
                                const char* scroll_ss = 
                                    "QScrollArea#YngchiiGameFrame { background: transparent; border: none; } "
                                    "QScrollArea#YngchiiGameFrame > QWidget > QWidget { background: transparent; } "
                                    "QScrollBar:vertical { width: 6px; background: transparent; margin: 0px; } "
                                    "QScrollBar::handle:vertical { background: #3f3f46; border-radius: 3px; min-height: 24px; } "
                                    "QScrollBar::handle:vertical:hover { background: #71717a; } "
                                    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; } "
                                    "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }";
                                char q_sss[32] = {0};
                                s_pfn_fromUtf8_str(q_sss, scroll_ss, -1);
                                s_pfn_setStyleSheet(scroll, q_sss);
                                s_pfn_dtor(q_sss);
                            }

                            if (s_pfn_insertWidget) {
                                s_pfn_insertWidget(page_layout, 2, scroll, 100, 0); // stretch = 100 to fill vertical space!
                            } else if (s_pfn_addWidget) {
                                s_pfn_addWidget(page_layout, scroll, 100, 0);
                            }
// Margins managed by Pass 2

                            if (s_pfn_show) {
                                s_pfn_show(parent);
                                s_pfn_show(scroll);
                            }
                            s_game_scroll_area = scroll;
                            if (s_hwndMain && IsWindow(s_hwndMain)) {
                                RECT wr;
                                if (GetWindowRect(s_hwndMain, &wr) && (wr.right - wr.left > 1280 || wr.bottom - wr.top > 820)) {
                                    int screen_w = GetSystemMetrics(SM_CXSCREEN);
                                    int screen_h = GetSystemMetrics(SM_CYSCREEN);
                                    int win_w = 1280;
                                    int win_h = (screen_h >= 960) ? 920 : 850;
                                    int win_x = (screen_w > win_w) ? (screen_w - win_w) / 2 : 0;
                                    int win_y = (screen_h > win_h) ? (screen_h - win_h) / 2 : 0;
                                    SetWindowPos(s_hwndMain, NULL, win_x, win_y, win_w, win_h, SWP_NOZORDER | SWP_NOACTIVATE);
                                }
                            }
                        }
                    }
                }

                if (s_game_scroll_area) {
                    if (s_modern_home_list) {
                        if (s_pfn_setHScrollBarPolicy) s_pfn_setHScrollBarPolicy(s_game_scroll_area, 1); // ScrollBarAlwaysOff
                        if (s_pfn_setVScrollBarPolicy) s_pfn_setVScrollBarPolicy(s_game_scroll_area, 0); // ScrollBarAsNeeded
                    } else {
                        if (s_pfn_setHScrollBarPolicy) s_pfn_setHScrollBarPolicy(s_game_scroll_area, 0); // ScrollBarAsNeeded
                        if (s_pfn_setVScrollBarPolicy) s_pfn_setVScrollBarPolicy(s_game_scroll_area, 1); // ScrollBarAlwaysOff
                    }
                }

                if (layout && s_pfn_setDirection) {
                    s_pfn_setDirection(layout, s_modern_home_list ? 2 : 0);
                    if (s_modern_home_list) {
                        // Fix 2: Clear Qt::AlignLeft so cards stretch to 100% full width
                        if (s_pfn_layout_item_setAlign) {
                            s_pfn_layout_item_setAlign((char*)layout + 16, 0);
                        }
                        if (s_pfn_setContentsMargins) {
                            s_pfn_setContentsMargins(layout, 0, 0, 10, 0);
                        }
                    }
                }

                // 2. Read tile title from toolTip
                char full_title_buf[256] = {0};
                if (s_pfn_toolTip) {
                    char q_tip[32] = {0};
                    s_pfn_toolTip(w, q_tip);
                    char q_ba[32] = {0};
                    s_pfn_toUtf8(q_tip, q_ba);
                    const char* ft = s_pfn_constData(q_ba);
                    if (ft && ft[0] != '\0') {
                        strncpy(full_title_buf, ft, sizeof(full_title_buf) - 1);
                    }
                    if (s_pfn_qba_dtor) s_pfn_qba_dtor(q_ba);
                    s_pfn_dtor(q_tip);
                }

                // 3. Filter non-game tiles ("All games", "Add folder") in List View
                int is_non_game = 0;
                if (full_title_buf[0] != '\0') {
                    if (strcmp(full_title_buf, "All games") == 0 || strcmp(full_title_buf, "Kho game") == 0 ||
                        strstr(full_title_buf, "All games") != NULL || strstr(full_title_buf, "Kho game") != NULL || strstr(full_title_buf, "Tất cả") != NULL) {
                        is_non_game = 1;
                        s_btn_all_games = w;
                    } else if (strcmp(full_title_buf, "Add folder") == 0 || strcmp(full_title_buf, "Thêm game") == 0 ||
                               strstr(full_title_buf, "Add folder") != NULL || strstr(full_title_buf, "Thêm game") != NULL || strstr(full_title_buf, "Thêm thư mục") != NULL) {
                        is_non_game = 1;
                        s_btn_add_folder = w;
                    }
                }

                if (s_modern_home_list && is_non_game) {
                    if (s_pfn_hide) s_pfn_hide(w);
                    continue;
                } else if (!s_modern_home_list && is_non_game) {
                    if (s_pfn_show) s_pfn_show(w);
                }

                // 4. Find existing child labels on card via children()
                void* card_layout = s_pfn_layout ? s_pfn_layout(w) : NULL;
                void* lbl_info = NULL;
                void* lbl_div = NULL;
                void* lbl_meta = NULL;
                void* lbl_stats = NULL;

                const Qt6List* ch_list = s_pfn_children ? s_pfn_children(w) : NULL;
                if (ch_list && ch_list->ptr && ch_list->size > 0) {
                    for (int64_t c = 0; c < ch_list->size; c++) {
                        void* child = ch_list->ptr[c];
                        if (!child) continue;
                        char child_name[64] = {0};
                        if (s_pfn_objectName) {
                            char qstr_c[32] = {0};
                            s_pfn_objectName(child, qstr_c);
                            char qba_c[32] = {0};
                            s_pfn_toUtf8(qstr_c, qba_c);
                            const char* cname = s_pfn_constData(qba_c);
                            if (cname) strncpy(child_name, cname, sizeof(child_name) - 1);
                            if (s_pfn_qba_dtor) s_pfn_qba_dtor(qba_c);
                            s_pfn_dtor(qstr_c);
                        }
                        if (strcmp(child_name, "card_lbl_info") == 0) lbl_info = child;
                        else if (strcmp(child_name, "card_div") == 0) lbl_div = child;
                        else if (strcmp(child_name, "card_lbl_meta") == 0) lbl_meta = child;
                        else if (strcmp(child_name, "card_lbl_stats") == 0) lbl_stats = child;
                    }
                }

                if (s_modern_home_list) {
                    // Create QHBoxLayout if not present
                    if (!card_layout && s_pfn_hbox_ctor) {
                        card_layout = malloc(1024);
                        if (card_layout) {
                            memset(card_layout, 0, 1024);
                            s_pfn_hbox_ctor(card_layout, w);
                            if (s_pfn_setContentsMargins) s_pfn_setContentsMargins(card_layout, 104, 10, 18, 10);
                            if (s_pfn_setSpacing) s_pfn_setSpacing(card_layout, 16);
                        }
                    }

                    // Create 4 child widgets inside card_layout
                    if (card_layout && s_pfn_lbl_ctor) {
                        if (!lbl_info) {
                            lbl_info = malloc(1024);
                            if (lbl_info) {
                                memset(lbl_info, 0, 1024);
                                s_pfn_lbl_ctor(lbl_info, w, 0);
                                if (s_pfn_setAttribute) s_pfn_setAttribute(lbl_info, 51, 1);
                                if (s_pfn_setObjectName) {
                                    char q_n[32] = {0};
                                    s_pfn_fromUtf8_str(q_n, "card_lbl_info", -1);
                                    s_pfn_setObjectName(lbl_info, q_n);
                                    s_pfn_dtor(q_n);
                                }
                                if (s_pfn_addWidget) s_pfn_addWidget(card_layout, lbl_info, 1, 0);
                            }
                        }
                        if (!lbl_div) {
                            lbl_div = malloc(1024);
                            if (lbl_div) {
                                memset(lbl_div, 0, 1024);
                                s_pfn_lbl_ctor(lbl_div, w, 0);
                                if (s_pfn_setAttribute) s_pfn_setAttribute(lbl_div, 51, 1);
                                if (s_pfn_setFixedWidth) s_pfn_setFixedWidth(lbl_div, 1);
                                if (s_pfn_setObjectName) {
                                    char q_n[32] = {0};
                                    s_pfn_fromUtf8_str(q_n, "card_div", -1);
                                    s_pfn_setObjectName(lbl_div, q_n);
                                    s_pfn_dtor(q_n);
                                }
                                if (s_pfn_setStyleSheet) {
                                    char q_ss[32] = {0};
                                    s_pfn_fromUtf8_str(q_ss, "QLabel { background-color: #383844; margin: 4px 0px; }", -1);
                                    s_pfn_setStyleSheet(lbl_div, q_ss);
                                    s_pfn_dtor(q_ss);
                                }
                                if (s_pfn_addWidget) s_pfn_addWidget(card_layout, lbl_div, 0, 0);
                            }
                        }
                        if (!lbl_meta) {
                            lbl_meta = malloc(1024);
                            if (lbl_meta) {
                                memset(lbl_meta, 0, 1024);
                                s_pfn_lbl_ctor(lbl_meta, w, 0);
                                if (s_pfn_setAttribute) s_pfn_setAttribute(lbl_meta, 51, 1);
                                if (s_pfn_setFixedWidth) s_pfn_setFixedWidth(lbl_meta, 190);
                                if (s_pfn_setAlignment) s_pfn_setAlignment(lbl_meta, 0x0021); // Top-Left
                                if (s_pfn_setObjectName) {
                                    char q_n[32] = {0};
                                    s_pfn_fromUtf8_str(q_n, "card_lbl_meta", -1);
                                    s_pfn_setObjectName(lbl_meta, q_n);
                                    s_pfn_dtor(q_n);
                                }
                                if (s_pfn_addWidget) s_pfn_addWidget(card_layout, lbl_meta, 0, 0);
                            }
                        }
                        if (!lbl_stats) {
                            lbl_stats = malloc(1024);
                            if (lbl_stats) {
                                memset(lbl_stats, 0, 1024);
                                s_pfn_lbl_ctor(lbl_stats, w, 0);
                                if (s_pfn_setAttribute) s_pfn_setAttribute(lbl_stats, 51, 1);
                                if (s_pfn_setFixedWidth) s_pfn_setFixedWidth(lbl_stats, 110);
                                if (s_pfn_setAlignment) s_pfn_setAlignment(lbl_stats, 0x0022); // Top-Right
                                if (s_pfn_setObjectName) {
                                    char q_n[32] = {0};
                                    s_pfn_fromUtf8_str(q_n, "card_lbl_stats", -1);
                                    s_pfn_setObjectName(lbl_stats, q_n);
                                    s_pfn_dtor(q_n);
                                }
                                if (s_pfn_addWidget) s_pfn_addWidget(card_layout, lbl_stats, 0, 0);
                            }
                        }
                    }

                    // Card button sizing - 100% full width
                    if (s_pfn_setSizePolicy) {
                        s_pfn_setSizePolicy(w, 7, 0); // Expanding = 7, Fixed = 0
                        if (parent) s_pfn_setSizePolicy(parent, 7, 5); // Expanding, Preferred
                    }
                    if (parent && s_pfn_setMinimumWidth) s_pfn_setMinimumWidth(parent, 0);
                    if (parent && s_pfn_setMaximumWidth) s_pfn_setMaximumWidth(parent, 16777215);
                    if (layout && s_pfn_layout_setAlign_widget) {
                        s_pfn_layout_setAlign_widget(layout, w, 0);
                    }
                    if (s_pfn_setToolButtonStyle) s_pfn_setToolButtonStyle(w, 2); // ToolButtonTextBesideIcon
                    if (s_pfn_setIconSize) {
                        struct { int w, h; } sz = {80, 80};
                        s_pfn_setIconSize(w, &sz);
                    }
                    if (s_pfn_setMinimumHeight) s_pfn_setMinimumHeight(w, 104);
                    if (s_pfn_setFixedHeight) s_pfn_setFixedHeight(w, 104);
                    if (s_pfn_setMinimumWidth) s_pfn_setMinimumWidth(w, 0); // Allow full stretching
                    if (s_pfn_setMaximumWidth) s_pfn_setMaximumWidth(w, 16777215);

                    // Clear button text
                    if (s_pfn_btn_setText) {
                        char q_empty[32] = {0};
                        s_pfn_fromUtf8_str(q_empty, "", -1);
                        s_pfn_btn_setText(w, q_empty);
                        s_pfn_dtor(q_empty);
                    }

                    // Look up metadata
                    GameMetaCache* matched = NULL;
                    for (int m = 0; m < s_game_meta_count; m++) {
                        if (full_title_buf[0] != '\0' &&
                            (strcmp(s_game_meta[m].title, full_title_buf) == 0 ||
                             strstr(s_game_meta[m].title, full_title_buf) != NULL ||
                             strstr(full_title_buf, s_game_meta[m].title) != NULL)) {
                            matched = &s_game_meta[m];
                            break;
                        }
                    }

                    const char* c_pid = (matched && matched->program_id[0]) ? matched->program_id : "0100000000010000";
                    const char* c_ftype = (matched && matched->file_type[0]) ? matched->file_type : "NSP";
                    const char* c_fsize = (matched && matched->file_size[0]) ? matched->file_size : "";
                    const char* c_ptime = (matched && matched->play_time[0] && 
                                           strcmp(matched->play_time, "00:00:00") != 0 &&
                                           strcmp(matched->play_time, "--:--:--") != 0) ? matched->play_time : (g_lang_vn ? "Chưa chơi" : "Never");

                    char c_ver[32] = "v1.0.0";
                    char c_dev[64] = "Nintendo";
                    char c_mod[64] = "Custom Config";

                    if (matched && matched->add_ons[0] != '\0') {
                        const char* up = strstr(matched->add_ons, "Update (");
                        if (up) {
                            const char* vstart = up + 8;
                            const char* vend = strchr(vstart, ')');
                            if (vend && (vend - vstart) < 20) {
                                snprintf(c_ver, sizeof(c_ver), "v%.*s", (int)(vend - vstart), vstart);
                            }
                        }
                        if (strstr(matched->add_ons, "DLC")) {
                            strncpy(c_mod, "DLC • LayeredFS", sizeof(c_mod) - 1);
                        } else {
                            strncpy(c_mod, "LayeredFS", sizeof(c_mod) - 1);
                        }
                    }

                    char h_info[1024];
                    snprintf(h_info, sizeof(h_info),
                        "<div>"
                        "<div style=\"font-size: 14px; font-weight: 700; color: #ffffff;\">%s</div>"
                        "<div style=\"font-size: 12px; color: #d1d5db; margin-top: 2px;\">%s</div>"
                        "<div style=\"font-size: 11px; color: #9ca3af; margin-top: 1px;\">%s</div>"
                        "</div>",
                        full_title_buf, c_dev, c_ver);

                    char h_meta[512];
                    snprintf(h_meta, sizeof(h_meta),
                        "<div>"
                        "<div style=\"color: #e4e4e7; font-size: 12px; font-weight: 600; font-family: monospace;\">%s</div>"
                        "<div style=\"color: #d4d4d8; font-size: 12px; margin-top: 4px;\">%s</div>"
                        "<div style=\"color: #38bdf8; font-size: 11px; margin-top: 4px;\">%s</div>"
                        "</div>",
                        c_pid, c_ftype, c_mod);

                    char h_stats[512];
                    snprintf(h_stats, sizeof(h_stats),
                        "<div style=\"text-align: right;\">"
                        "<div style=\"color: #e4e4e7; font-size: 12px; font-weight: 600;\">%s</div>"
                        "<div style=\"color: #d4d4d8; font-size: 12px; margin-top: 4px;\">%s</div>"
                        "</div>",
                        c_ptime, c_fsize);

                    if (lbl_info && s_pfn_lbl_setText) {
                        char q_h[32] = {0};
                        s_pfn_fromUtf8_str(q_h, h_info, -1);
                        s_pfn_lbl_setText(lbl_info, q_h);
                        s_pfn_dtor(q_h);
                    }
                    if (lbl_meta && s_pfn_lbl_setText) {
                        char q_h[32] = {0};
                        s_pfn_fromUtf8_str(q_h, h_meta, -1);
                        s_pfn_lbl_setText(lbl_meta, q_h);
                        s_pfn_dtor(q_h);
                    }
                    if (lbl_stats && s_pfn_lbl_setText) {
                        char q_h[32] = {0};
                        s_pfn_fromUtf8_str(q_h, h_stats, -1);
                        s_pfn_lbl_setText(lbl_stats, q_h);
                        s_pfn_dtor(q_h);
                    }

                    if (s_pfn_show) {
                        if (lbl_info) s_pfn_show(lbl_info);
                        if (lbl_div) s_pfn_show(lbl_div);
                        if (lbl_meta) s_pfn_show(lbl_meta);
                        if (lbl_stats) s_pfn_show(lbl_stats);
                    }

                    int is_hero_card = (cur_hero_title[0] != '\0' &&
                        (strcmp(full_title_buf, cur_hero_title) == 0 ||
                         strstr(full_title_buf, cur_hero_title) != NULL ||
                         strstr(cur_hero_title, full_title_buf) != NULL));

                    if (s_pfn_setStyleSheet) {
                        char card_css[512];
                        if (is_hero_card) {
                            snprintf(card_css, sizeof(card_css),
                                "QToolButton#FritchiiTile { background: #22222b; border: 1.5px solid %s; border-radius: 8px; margin-bottom: 6px; padding: 0px; text-align: left; } "
                                "QToolButton#FritchiiTile:hover { background: #282833; border-color: %s; }",
                                g_palettes[g_theme_idx].accent,
                                g_palettes[g_theme_idx].accent_light);
                        } else {
                            snprintf(card_css, sizeof(card_css),
                                "QToolButton#FritchiiTile { background: #1e1e24; border: 1px solid #2e2e38; border-radius: 8px; margin-bottom: 6px; padding: 0px; text-align: left; } "
                                "QToolButton#FritchiiTile:hover { background: #262630; border-color: %s; }",
                                g_palettes[g_theme_idx].accent);
                        }
                        char q_css[32] = {0};
                        s_pfn_fromUtf8_str(q_css, card_css, -1);
                        s_pfn_setStyleSheet(w, q_css);
                        s_pfn_dtor(q_css);
                    }

                    // Set custom tile cover if in covers/<c_pid>.png
                    if (c_pid[0] != '\0') {
                        wchar_t exe_dir[MAX_PATH] = {0};
                        GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
                        PathRemoveFileSpecW(exe_dir);
                        wchar_t cov_tile[MAX_PATH] = {0};
                        swprintf(cov_tile, MAX_PATH, L"%s\\covers\\%S_rounded.png", exe_dir, c_pid);
                        if (GetFileAttributesW(cov_tile) == INVALID_FILE_ATTRIBUTES) {
                            swprintf(cov_tile, MAX_PATH, L"%s\\covers\\%S.png", exe_dir, c_pid);
                        }
                        if (GetFileAttributesW(cov_tile) != INVALID_FILE_ATTRIBUTES) {
                            char utf8_cov[MAX_PATH * 2] = {0};
                            WideCharToMultiByte(CP_UTF8, 0, cov_tile, -1, utf8_cov, sizeof(utf8_cov), NULL, NULL);
                            if (s_pfn_pixmap_ctor_file && s_pfn_qicon_ctor_pixmap && s_pfn_setIcon && s_pfn_qicon_dtor && s_pfn_dtor_pixmap) {
                                char q_cov[32] = {0};
                                s_pfn_fromUtf8_str(q_cov, utf8_cov, -1);
                                char pix_tmp[64] = {0};
                                s_pfn_pixmap_ctor_file(pix_tmp, q_cov, NULL, 0);
                                s_pfn_dtor(q_cov);
                                char q_ic[32] = {0};
                                s_pfn_qicon_ctor_pixmap(q_ic, pix_tmp);
                                s_pfn_setIcon(w, q_ic);
                                s_pfn_qicon_dtor(q_ic);
                                s_pfn_dtor_pixmap(pix_tmp);
                            }
                        }
                    }
                } else {
                    // Grid View mode: restore default styling
                    if (s_pfn_hide) {
                        if (lbl_info) s_pfn_hide(lbl_info);
                        if (lbl_div) s_pfn_hide(lbl_div);
                        if (lbl_meta) s_pfn_hide(lbl_meta);
                        if (lbl_stats) s_pfn_hide(lbl_stats);
                    }
                    if (s_pfn_setToolButtonStyle) s_pfn_setToolButtonStyle(w, 3); // ToolButtonTextUnderIcon
                    if (s_pfn_setIconSize) {
                        struct { int w, h; } sz = {72, 72};
                        s_pfn_setIconSize(w, &sz);
                    }
                    if (s_pfn_setMinimumHeight) s_pfn_setMinimumHeight(w, 0);
                    if (s_pfn_setFixedHeight) s_pfn_setFixedHeight(w, 16777215);
                    if (s_pfn_setMinimumWidth) s_pfn_setMinimumWidth(w, 0);
                    if (s_pfn_setMaximumWidth) s_pfn_setMaximumWidth(w, 16777215);
                    if (s_pfn_btn_setText && full_title_buf[0] != '\0') {
                        char q_t[32] = {0};
                        s_pfn_fromUtf8_str(q_t, full_title_buf, -1);
                        s_pfn_btn_setText(w, q_t);
                        s_pfn_dtor(q_t);
                    }
                    if (s_pfn_setStyleSheet) {
                        char q_css[32] = {0};
                        s_pfn_fromUtf8_str(q_css, "", -1);
                        s_pfn_setStyleSheet(w, q_css);
                        s_pfn_dtor(q_css);
                    }
                }
                continue;
            }

            // Chainsaw Man Dialog Logo Replacement (always check on every timer tick)
            if (strcmp(obj_name_buf, "labelLogo") == 0) {
                static int s_chainsaw_dlg_inited = 0;
                static char s_chainsaw_dlg_pix[64] = {0};
                if (!s_chainsaw_dlg_inited && s_pfn_pixmap_ctor_file) {
                    wchar_t exe_dir[MAX_PATH] = {0};
                    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
                    PathRemoveFileSpecW(exe_dir);
                    wchar_t logo_path[MAX_PATH];
                    swprintf(logo_path, MAX_PATH, L"%s\\covers\\chainsaw_badge.png", exe_dir);
                    if (GetFileAttributesW(logo_path) != INVALID_FILE_ATTRIBUTES) {
                        char utf8_path[MAX_PATH * 2] = {0};
                        WideCharToMultiByte(CP_UTF8, 0, logo_path, -1, utf8_path, sizeof(utf8_path), NULL, NULL);
                        char q_pstr[32] = {0};
                        s_pfn_fromUtf8_str(q_pstr, utf8_path, -1);
                        s_pfn_pixmap_ctor_file(s_chainsaw_dlg_pix, q_pstr, NULL, 0);
                        s_pfn_dtor(q_pstr);
                        s_chainsaw_dlg_inited = 1;
                    }
                }
                if (s_chainsaw_dlg_inited && s_pfn_setPixmap) {
                    s_pfn_setPixmap(w, s_chainsaw_dlg_pix);
                } else {
                    if (!g_hChainsaw) {
                        HMODULE hExe = GetModuleHandleW(NULL);
                        g_hChainsaw = (HICON)LoadImageW(hExe, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
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
            if (s_pfn_inherits(w, "QTreeView")) {
                PopulateGameMeta(w);
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
            COLORREF trackBg = is_vn_visual ? g_palettes[g_theme_idx].ref_dark : RGB(24, 24, 28);
            COLORREF trackBorder = is_vn_visual ? g_palettes[g_theme_idx].ref_accent : RGB(56, 56, 64);

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

            COLORREF knobBg = is_vn_visual ? g_palettes[g_theme_idx].ref_accent : RGB(63, 63, 72);
            COLORREF knobBorder = is_vn_visual ? g_palettes[g_theme_idx].ref_light : RGB(113, 113, 126);

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

    // Mathematically center inside 64px rail column across any DPI
    int rail_w = (64 * (int)dpi) / 96;
    int bw = (28 * (int)dpi) / 96;
    int bh = (54 * (int)dpi) / 96;
    int bx = (rail_w - bw) / 2;
    int by = (270 * (int)dpi) / 96;

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

        RECT wr_init;
        if (GetWindowRect(hWnd, &wr_init) && (wr_init.right - wr_init.left > 1280 || wr_init.bottom - wr_init.top > 820)) {
            int screen_w = GetSystemMetrics(SM_CXSCREEN);
            int screen_h = GetSystemMetrics(SM_CYSCREEN);
            int win_w = 1280;
            int win_h = (screen_h >= 960) ? 920 : 850;
            int win_x = (screen_w > win_w) ? (screen_w - win_w) / 2 : 0;
            int win_y = (screen_h > win_h) ? (screen_h - win_h) / 2 : 0;
            SetWindowPos(hWnd, NULL, win_x, win_y, win_w, win_h, SWP_NOZORDER | SWP_NOACTIVATE);
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
        if (wParam == VK_F10 || (wParam == 'T' && (GetKeyState(VK_CONTROL) & 0x8000))) {
            g_theme_idx = (g_theme_idx + 1) % 4;
            SaveThemePreference(g_theme_idx);
            UpdateInternalFritchiiAccent(g_theme_idx);
            if (s_hwndLangBtn && IsWindow(s_hwndLangBtn)) {
                InvalidateRect(s_hwndLangBtn, NULL, FALSE);
            }
            TriggerLiveQtWidgetTranslation(1);
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
    DWORD appdata_len = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    if (!appdata_len || appdata_len >= MAX_PATH) return;
    char log_path[MAX_PATH] = {0};
    int path_len = snprintf(log_path, sizeof(log_path), "%s\\eden\\log\\yngchii_step_audit.log", appdata);
    if (path_len < 0 || path_len >= MAX_PATH) return;

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
                            const wchar_t* p_pipe3 = wcsstr(suffix, L" | ");
                            if (p_pipe3) {
                                const wchar_t* p_pipe4 = wcsstr(p_pipe3 + 3, L" | ");
                                if (p_pipe4) {
                                    const wchar_t* p_pipe5 = wcsstr(p_pipe4 + 3, L" | ");
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
static void InitializeProxy(void) {
    DetectSupportedRyzenCpu();
    g_lang_vn = LoadLanguagePreference();
    g_theme_idx = LoadThemePreference();
    UpdateInternalFritchiiAccent(g_theme_idx);
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
    HANDLE monitor = CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
    if (monitor) CloseHandle(monitor);
    HANDLE discord = CreateThread(NULL, 0, DiscordRpcThread, NULL, 0, NULL);
    if (discord) CloseHandle(discord);
}

static int (__cdecl *s_orig_initialize_narrow_environment)(void) = NULL;

static int __cdecl InitializeProxyFromStartup(void) {
    int result = s_orig_initialize_narrow_environment();
    static LONG initialized = 0;
    if (result == 0 && InterlockedCompareExchange(&initialized, 1, 0) == 0) {
        InitializeProxy();
    }
    return result;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hInst = hinstDLL;
        if (lpReserved) {
            // The bundled EXE calls this from CRT startup, after the loader lock
            // is released and before constructors/main read the preset or use Qt.
            HookIAT(GetModuleHandleW(NULL), "api-ms-win-crt-runtime-l1-1-0.dll",
                    "_initialize_narrow_environment", (void*)InitializeProxyFromStartup,
                    (void**)&s_orig_initialize_narrow_environment);
        }
    }
    return TRUE;
}
