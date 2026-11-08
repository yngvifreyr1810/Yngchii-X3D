// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <version>
#if __cpp_lib_chrono >= 201907L
#include <chrono>
#include <exception>
#include <stdexcept>
#endif
#include <compare>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string_view>
#include <type_traits>
#include <deque>
#include <fmt/core.h>

#include "common/settings_enums.h"
#include "common/assert.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/time_zone.h"
#include "common/x3d_detect.h"

#if defined(__linux__ ) && defined(ARCHITECTURE_arm64)
#include <unistd.h>
#endif

namespace Settings {

// Clang 14 and earlier have errors when explicitly instantiating these classes
#ifndef CANNOT_EXPLICITLY_INSTANTIATE
#define SETTING(TYPE, RANGED) template class Setting<TYPE, RANGED>
#define SWITCHABLE(TYPE, RANGED) template class SwitchableSetting<TYPE, RANGED>

SETTING(AppletMode, false);
SETTING(AudioEngine, false);
SETTING(bool, false);
SETTING(int, false);
SETTING(std::string, false);
SETTING(u16, false);
SWITCHABLE(AnisotropyMode, true);
SWITCHABLE(AntiAliasing, false);
SWITCHABLE(OotAntiAliasing, false);
SWITCHABLE(AspectRatio, true);
SWITCHABLE(AstcDecodeMode, true);
SWITCHABLE(AstcRecompression, true);
SWITCHABLE(AudioMode, true);
SWITCHABLE(CpuBackend, true);
SWITCHABLE(CpuAccuracy, true);
SWITCHABLE(FullscreenMode, true);
SWITCHABLE(GpuAccuracy, true);
SWITCHABLE(Language, true);
SWITCHABLE(MemoryLayout, true);
SWITCHABLE(NvdecEmulation, false);
SWITCHABLE(Region, true);
SWITCHABLE(RendererBackend, true);
SWITCHABLE(ScalingFilter, false);
SWITCHABLE(FrameGenType, false);
SWITCHABLE(SpirvOptimizeMode, true);
SWITCHABLE(TimeZone, true);
SETTING(VSyncMode, true);
SWITCHABLE(bool, false);
SWITCHABLE(int, false);
SWITCHABLE(int, true);
SWITCHABLE(s64, false);
SWITCHABLE(u16, true);
SWITCHABLE(u32, false);
SWITCHABLE(u8, false);
SWITCHABLE(u8, true);

// Used in UISettings
// TODO see if we can move this to uisettings.cpp
SWITCHABLE(ConfirmStop, true);

#undef SETTING
#undef SWITCHABLE
#endif

Values values;

std::string GetTimeZoneString(TimeZone time_zone) {
    const auto time_zone_index = static_cast<std::size_t>(time_zone);
    ASSERT(time_zone_index < Common::TimeZone::GetTimeZoneStrings().size());

    std::string location_name;
    if (time_zone_index == 0) { // Auto
#if __cpp_lib_chrono >= 201907L && !defined(MINGW)
        // Disabled for MinGW -- tzdb always returns Etc/UTC
        try {
            const struct std::chrono::tzdb& time_zone_data = std::chrono::get_tzdb();
            const std::chrono::time_zone* current_zone = time_zone_data.current_zone();
            std::string_view current_zone_name = current_zone->name();
            location_name = current_zone_name;
        } catch (std::runtime_error& runtime_error) {
            // VCRUNTIME will throw a runtime_error if the operating system's selected time zone
            // cannot be found
            location_name = Common::TimeZone::FindSystemTimeZone();
            LOG_WARNING(Common,
                        "Error occurred when trying to determine system time zone:\n{}\nFalling "
                        "back to hour offset \"{}\"",
                        runtime_error.what(), location_name);
        }
#else
        location_name = Common::TimeZone::FindSystemTimeZone();
#endif
    } else {
        location_name = Common::TimeZone::GetTimeZoneStrings()[time_zone_index];
    }
    return location_name;
}

void LogSettings() {
    std::deque<std::string> settings_list;
    for (auto& [category, settings] : values.linkage.by_category) {
        for (const auto& setting : settings) {
            // Hide the token secret, for security reasons.
            if (setting->Id() != values.eden_token.Id()) {
                auto const is_default = setting->ToString() == setting->DefaultToString();
                auto const name = fmt::format(
                    "{:c}{:c} {}.{}",
                    is_default ? '-' : 'M',
                    setting->UsingGlobal() ? '-' : 'C', TranslateCategory(category),
                    setting->GetLabel());
                if (is_default)
                    settings_list.push_back(fmt::format("{}: {}", name, setting->Canonicalize()));
                else
                    settings_list.push_front(fmt::format("{}: {}", name, setting->Canonicalize()));
            }
        }
    }
    LOG_INFO(Config, "Eden Configuration:");
    for (auto const& e : settings_list)
        LOG_INFO(Config, "{}", e);
#define LOG_PATH(NAME) \
    LOG_INFO(Config, #NAME ": {}", Common::FS::PathToUTF8String(Common::FS::GetEdenPath(Common::FS::EdenPath::NAME)))
    LOG_PATH(CacheDir);
    LOG_PATH(ConfigDir);
    LOG_PATH(LoadDir);
    LOG_PATH(NANDDir);
    LOG_PATH(SaveDir);
    LOG_PATH(SDMCDir);
#undef LOG_PATH
}

bool GetDebugKnobAt(u8 i) {
    return (values.debug_knobs.GetValue() & (1 << (i & 0xF))) != 0;
}

void UpdateGPUAccuracy() {
    values.current_gpu_accuracy = values.gpu_accuracy.GetValue();
}

bool IsGPULevelHigh() {
    return values.current_gpu_accuracy == GpuAccuracy::High;
}

bool IsDMALevelDefault() {
    return values.dma_accuracy.GetValue() == DmaAccuracy::Default;
}

bool IsDMALevelSafe() {
    return values.dma_accuracy.GetValue() == DmaAccuracy::Safe;
}

bool IsGPUFenceBehaviorDefault() {
    return values.gpu_fence_behavior.GetValue() == GpuFenceBehavior::Default;
}

bool IsGPUFenceBehaviorBalanced() {
    return values.gpu_fence_behavior.GetValue() == GpuFenceBehavior::Balanced;
}

bool IsGPUFenceBehaviorAccurate() {
    return values.gpu_fence_behavior.GetValue() == GpuFenceBehavior::Accurate;
}

bool IsFastmemEnabled() {
    if (values.cpu_accuracy.GetValue() == Settings::CpuAccuracy::Debugging)
        return bool(values.cpuopt_fastmem);
    else if (values.cpu_accuracy.GetValue() == CpuAccuracy::Unsafe)
        return bool(values.cpuopt_unsafe_host_mmu);
#if defined(__linux__) && defined(ARCHITECTURE_arm64)
    // Only 4kb systems support host MMU right now
    // TODO: Support this
    return getpagesize() == 4096;
#elif !defined(__APPLE__) && !defined(__ANDROID__) && !defined(_WIN32) && !defined(__linux__) && !defined(__FreeBSD__)
    return false;
#else
    return true;
#endif
}

static bool is_nce_enabled = false;

void SetNceEnabled(bool is_39bit) {
    const bool is_nce_selected = values.cpu_backend.GetValue() == CpuBackend::Nce;
    if (is_nce_selected && !IsFastmemEnabled()) {
        LOG_WARNING(Common, "Fastmem is required to natively execute code in a performant manner, "
                            "falling back to Dynarmic");
    }
    if (is_nce_selected && !is_39bit) {
        LOG_WARNING(
            Common,
            "Program does not utilize 39-bit address space, unable to natively execute code");
    }
    is_nce_enabled = IsFastmemEnabled() && is_nce_selected && is_39bit;
}

bool IsNceEnabled() {
    return is_nce_enabled;
}

static u64 current_program_id = 0;

void SetCurrentProgramID(u64 program_id) {
    current_program_id = program_id;
}

u64 GetCurrentProgramID() {
    return current_program_id;
}

static bool current_program_is_switch2 = false;

void SetCurrentProgramIsSwitch2(bool is_switch2) {
    current_program_is_switch2 = is_switch2;
}

bool CurrentProgramIsSwitch2() {
    return current_program_is_switch2;
}

static std::optional<AstcRecompression> astc_recompression_override;

AstcRecompression GetAstcRecompression() {
    return astc_recompression_override.value_or(values.astc_recompression.GetValue());
}

void SetAstcRecompressionOverride(std::optional<AstcRecompression> recompression) {
    astc_recompression_override = recompression;
}

bool IsDockedMode() {
    return values.use_docked_mode.GetValue() == Settings::ConsoleMode::Docked;
}

float Volume() {
    if (values.audio_muted) {
        return 0.0f;
    }
    return values.volume.GetValue() / static_cast<f32>(values.volume.GetDefault());
}

const char* TranslateCategory(Category category) {
    switch (category) {
    case Category::Android:
        return "Android";
    case Category::Audio:
        return "Audio";
    case Category::Core:
        return "Core";
    case Category::Cpu:
    case Category::CpuDebug:
    case Category::CpuUnsafe:
        return "Cpu";
    case Category::Overlay:
        return "Overlay";
    case Category::Renderer:
    case Category::RendererAdvanced:
    case Category::RendererHacks:
    case Category::RendererDebug:
    case Category::RendererExtensions:
        return "Renderer";
    case Category::System:
    case Category::SystemAudio:
        return "System";
    case Category::DataStorage:
        return "Data Storage";
    case Category::Debugging:
    case Category::DebuggingGraphics:
        return "Debugging";
    case Category::GpuDriver:
        return "GpuDriver";
    case Category::LibraryApplet:
        return "LibraryApplet";
    case Category::Miscellaneous:
        return "Miscellaneous";
    case Category::Network:
        return "Network";
    case Category::WebService:
        return "WebService";
    case Category::AddOns:
        return "DisabledAddOns";
    case Category::Controls:
        return "Controls";
    case Category::Ui:
    case Category::UiGeneral:
        return "UI";
    case Category::UiAudio:
        return "UiAudio";
    case Category::UiLayout:
        return "UILayout";
    case Category::UiGameList:
        return "UIGameList";
    case Category::Screenshots:
        return "Screenshots";
    case Category::Shortcuts:
        return "Shortcuts";
    case Category::Multiplayer:
        return "Multiplayer";
    case Category::Services:
        return "Services";
    case Category::Paths:
        return "Paths";
    case Category::MaxEnum:
        break;
    }
    return "Miscellaneous";
}

void TranslateResolutionInfo(ResolutionSetup setup, ResolutionScalingInfo& info) {
    info.downscale = false;
    switch (setup) {
    case ResolutionSetup::Res1_4X:
        info.up_scale = 1;
        info.down_shift = 2;
        info.downscale = true;
        break;
    case ResolutionSetup::Res1_2X:
        info.up_scale = 1;
        info.down_shift = 1;
        info.downscale = true;
        break;
    case ResolutionSetup::Res3_4X:
        info.up_scale = 3;
        info.down_shift = 2;
        info.downscale = true;
        break;
    case ResolutionSetup::Res1X:
        info.up_scale = 1;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res3_2X:
        info.up_scale = 3;
        info.down_shift = 1;
        break;
    case ResolutionSetup::Res5_4X:
        info.up_scale = 5;
        info.down_shift = 2;
        break;
    case ResolutionSetup::Res2X:
        info.up_scale = 2;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res3X:
        info.up_scale = 3;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res4X:
        info.up_scale = 4;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res5X:
        info.up_scale = 5;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res6X:
        info.up_scale = 6;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res7X:
        info.up_scale = 7;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res8X:
        info.up_scale = 8;
        info.down_shift = 0;
        break;
    default:
        UNREACHABLE();
    }
    info.up_factor = static_cast<f32>(info.up_scale) / (1U << info.down_shift);
    info.down_factor = static_cast<f32>(1U << info.down_shift) / info.up_scale;
    info.active = info.up_scale != 1 || info.down_shift != 0;
}

void UpdateRescalingInfo() {
    const auto setup = values.resolution_setup.GetValue();
    auto& info = values.resolution_info;
    TranslateResolutionInfo(setup, info);
}

u32 FrameGenMultiplier() {
    return std::clamp(values.frame_gen_multiplier.GetValue(), MIN_FRAME_GEN_MULTIPLIER,
                      MAX_FRAME_GEN_MULTIPLIER);
}

size_t FrameGenGenerations() {
    if (!values.frame_gen.GetValue()) {
        return 0;
    }
    return FrameGenMultiplier() - 1;
}

size_t FrameGenMaxGenerations() {
    if (!values.frame_gen.GetValue()) {
        return 0;
    }
    if (values.frame_gen_target_rate.GetValue() != 0) {
        return MAX_FRAME_GEN_MULTIPLIER - 1;
    }
    return FrameGenMultiplier() - 1;
}

void RestoreGlobalState(bool is_powered_on) {
    // If a game is running, DO NOT restore the global settings state
    if (is_powered_on) {
        return;
    }

    for (const auto& reset : values.linkage.restore_functions) {
        reset();
    }

    // Reset per-game flags
    values.use_squashed_iterated_blend = false;
}

static bool configuring_global = true;

bool IsConfiguringGlobal() {
    return configuring_global;
}

void SetConfiguringGlobal(bool is_global) {
    configuring_global = is_global;
}

u16 SpeedLimit() {
    switch (SpeedMode(values.current_speed_mode)) {
    case SpeedMode::Standard:
        return values.speed_limit.GetValue();
    case SpeedMode::Turbo:
        return values.turbo_speed_limit.GetValue();
    case SpeedMode::Slow:
        return values.slow_speed_limit.GetValue();
    default:
        UNIMPLEMENTED();
    }

    return 100;
}

void SetSpeedMode(const SpeedMode& mode) {
    values.current_speed_mode.SetValue(mode);

    switch (mode) {
    case SpeedMode::Turbo:
    case SpeedMode::Slow:
        values.use_speed_limit.SetValue(true);
        break;
    case SpeedMode::Standard:
    default:
        break;
    }
}

void ToggleStandardMode() {
    values.use_speed_limit.SetValue(!values.use_speed_limit.GetValue());
    SetSpeedMode(SpeedMode::Standard);
}

void ToggleTurboMode() {
    if (values.current_speed_mode.GetValue() != SpeedMode::Turbo)
        SetSpeedMode(SpeedMode::Turbo);
    else
        SetSpeedMode(SpeedMode::Standard);
}

void ToggleSlowMode() {
    if (values.current_speed_mode.GetValue() != SpeedMode::Slow)
        SetSpeedMode(SpeedMode::Slow);
    else
        SetSpeedMode(SpeedMode::Standard);
}

bool IsOpenGL() {
    const auto backend = Settings::values.renderer_backend.GetValue();
    switch (backend) {
    case RendererBackend::OpenGL_GLSL:
    case RendererBackend::OpenGL_GLASM:
    case RendererBackend::OpenGL_SPIRV:
        return true;
    default:
        return false;
    }
}

void ApplyYngviX3dProfile(bool gpu_vram_enabled, bool cpu_x3d_enabled) {
    auto set_env = [](const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    };

    const auto& cpu = Common::DetectRyzenX3DCpu();

    // 1. Tier 1: Universal GPU/VRAM & Engine Optimizations (Safe on ALL CPUs & GPUs)
    if (gpu_vram_enabled) {
        LOG_INFO(Config, "[Yngvi's X3D Optimize] Enabling Tier 1 Universal GPU/VRAM & Engine Optimizations");
        set_env("EDEN_NX2_PRELOAD_ALL_PIPELINES", "1");
        set_env("EDEN_NX2_DRIVER_PIPELINE_CACHE", "1");
        set_env("EDEN_NX2_PRELOAD_RESIDENT_MAX", "22000");
        set_env("EDEN_NX2_PRELOAD_WARM_MAX", "6000");
        set_env("EDEN_NX2_BACKGROUND_PIPELINES", "0");
        set_env("EDEN_NX2_VSYNC_HZ", "240");
        set_env("EDEN_NX2_OCCLUSION_OFF", "1");
        set_env("EDEN_NX2_HOST_PACE", "0");
        set_env("EDEN_NX2_GRAPHICS_SSBO_BARE", "0");
        set_env("EDEN_NX2_WRITTEN_SSBO_MARK_KIB", "64");
        set_env("EDEN_NX2_ACQUIRE_PRECHECK", "2");
        set_env("EDEN_NX2_MACRO_SINK", "1");
        set_env("EDEN_NX2_COMPUTE_SINK", "1");
        set_env("EDEN_NX2_RING_PAIRS", "1");
        set_env("EDEN_NX2_UBO_INPLACE", "1");
        set_env("EDEN_NX2_PIPELINE_CLEAN", "1");
        set_env("EDEN_NX2_LIST_WAIT_SKIP", "1");
        set_env("EDEN_NX2_PREFLUSH", "1");
        set_env("EDEN_NX2_SEM_FAST", "2");
        set_env("EDEN_NX2_FENCE_BATCH", "1");
        set_env("EDEN_NX2_EXP2", "1");
        set_env("EDEN_NX2_EXP9", "3");
        set_env("EDEN_NX2_EXP10", "1");
        set_env("EDEN_NX2_EXP12", "1");
        set_env("EDEN_NX2_EXP14", "1");
        set_env("EDEN_NX2_EXP17", "1");
        set_env("EDEN_NX2_EXP18", "7");
        set_env("EDEN_NX2_EXP19", "2");
        set_env("EDEN_NX2_RANGED_MAX", "8192");
        set_env("EDEN_NX2_LOOP_SKIP", "1");

        set_env("VK_LOADER_LAYERS_DISABLE", "*NvCamera*,*NV_nomad*,*NVIDIA_Overlay*,*GalaxyOverlay*,*EOSOverlay*");
        set_env("NV_ALLOW_ANSEL", "0");
        set_env("ANSEL_SDK_DISABLE", "1");
    } else {
        LOG_INFO(Config, "[Yngvi's X3D Optimize] Tier 1 GPU/VRAM Optimizations Disabled (Stock Mode)");
        set_env("EDEN_NX2_PRELOAD_ALL_PIPELINES", "0");
        set_env("EDEN_NX2_DRIVER_PIPELINE_CACHE", "0");
        set_env("EDEN_NX2_PRELOAD_RESIDENT_MAX", "0");
        set_env("EDEN_NX2_PRELOAD_WARM_MAX", "0");
        set_env("EDEN_NX2_BACKGROUND_PIPELINES", "0");
        set_env("EDEN_NX2_VSYNC_HZ", "60");
        set_env("EDEN_NX2_OCCLUSION_OFF", "0");
        set_env("EDEN_NX2_HOST_PACE", "1");
        set_env("EDEN_NX2_GRAPHICS_SSBO_BARE", "0");
        set_env("EDEN_NX2_WRITTEN_SSBO_MARK_KIB", "0");
        set_env("EDEN_NX2_ACQUIRE_PRECHECK", "0");
        set_env("EDEN_NX2_MACRO_SINK", "0");
        set_env("EDEN_NX2_COMPUTE_SINK", "0");
        set_env("EDEN_NX2_RING_PAIRS", "0");
        set_env("EDEN_NX2_UBO_INPLACE", "0");
        set_env("EDEN_NX2_PIPELINE_CLEAN", "0");
        set_env("EDEN_NX2_LIST_WAIT_SKIP", "0");
        set_env("EDEN_NX2_PREFLUSH", "0");
        set_env("EDEN_NX2_SEM_FAST", "0");
        set_env("EDEN_NX2_FENCE_BATCH", "0");
        set_env("EDEN_NX2_EXP2", "0");
        set_env("EDEN_NX2_EXP9", "0");
        set_env("EDEN_NX2_EXP10", "0");
        set_env("EDEN_NX2_EXP12", "0");
        set_env("EDEN_NX2_EXP14", "0");
        set_env("EDEN_NX2_EXP17", "0");
        set_env("EDEN_NX2_EXP18", "0");
        set_env("EDEN_NX2_EXP19", "0");
        set_env("EDEN_NX2_RANGED_MAX", "0");
        set_env("EDEN_NX2_LOOP_SKIP", "0");

        set_env("VK_LOADER_LAYERS_DISABLE", "");
        set_env("NV_ALLOW_ANSEL", "1");
        set_env("ANSEL_SDK_DISABLE", "0");
    }

    // 2. Tier 2: CPU X3D Boost (Hard-Gated by is_supported_x3d)
    if (cpu_x3d_enabled) {
        if (cpu.is_supported_x3d) {
            LOG_INFO(Config, "[Yngvi's X3D Optimize] Activating Tier 2 CPU X3D Boost for: {}", cpu.status_summary);
            set_env("EDEN_NX2_SPLIT", "2");
            set_env("EDEN_NX2_PIN_THREADS", "1");
            set_env("EDEN_NX2_SPLIT_SPINS", "32768");
            if (gpu_vram_enabled) {
                set_env("EDEN_NX2_PERF_ON", "bounds,clean_words,async_pipelines,unswizzle_workers,cb_batch,index_rebind,sorted_page_batches,pipeline_retention,submit_budget,split_overlap");
                set_env("EDEN_NX2_PERF_OFF", "front_sleep,fence_flush,fragment_barrier");
            } else {
                set_env("EDEN_NX2_PERF_ON", "split_overlap");
                set_env("EDEN_NX2_PERF_OFF", "front_sleep");
            }
        } else {
            LOG_WARNING(Config, "[Yngvi's X3D Optimize] CPU '{}' is not an AMD Ryzen X3D (Zen 3/4/5) processor; CPU X3D Boost safely bypassed.", cpu.brand_string);
            set_env("EDEN_NX2_SPLIT", "0");
            set_env("EDEN_NX2_PIN_THREADS", "0");
            set_env("EDEN_NX2_SPLIT_SPINS", "2048");
            if (gpu_vram_enabled) {
                set_env("EDEN_NX2_PERF_ON", "bounds,clean_words,async_pipelines,unswizzle_workers,cb_batch,index_rebind,sorted_page_batches,pipeline_retention,submit_budget");
                set_env("EDEN_NX2_PERF_OFF", "fence_flush,fragment_barrier");
            } else {
                set_env("EDEN_NX2_PERF_ON", "");
                set_env("EDEN_NX2_PERF_OFF", "");
            }
        }
    } else {
        LOG_INFO(Config, "[Yngvi's X3D Optimize] Tier 2 CPU X3D Boost Disabled (Stock Mode)");
        set_env("EDEN_NX2_SPLIT", "0");
        set_env("EDEN_NX2_PIN_THREADS", "0");
        set_env("EDEN_NX2_SPLIT_SPINS", "2048");
        if (gpu_vram_enabled) {
            set_env("EDEN_NX2_PERF_ON", "bounds,clean_words,async_pipelines,unswizzle_workers,cb_batch,index_rebind,sorted_page_batches,pipeline_retention,submit_budget");
            set_env("EDEN_NX2_PERF_OFF", "fence_flush,fragment_barrier");
        } else {
            set_env("EDEN_NX2_PERF_ON", "");
            set_env("EDEN_NX2_PERF_OFF", "");
        }
    }
}

void ApplyYngviX3dProfile() {
    ApplyYngviX3dProfile(values.yngvi_x3d_optimize.GetValue(), values.yngvi_cpu_x3d_boost.GetValue());
}

} // namespace Settings
