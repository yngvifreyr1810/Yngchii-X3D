// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/x3d_detect.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#if defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <fmt/format.h>

namespace Common {

static void GetCpuid(int leaf, int subleaf, int info[4]) {
#if defined(_MSC_VER)
    __cpuidex(info, leaf, subleaf);
#elif defined(__GNUC__) || defined(__clang__)
    __cpuid_count(leaf, subleaf, info[0], info[1], info[2], info[3]);
#else
    info[0] = info[1] = info[2] = info[3] = 0;
#endif
}

bool HasRuntimeAvx512() {
    static const bool supported = []() -> bool {
        int info[4];
        GetCpuid(0, 0, info);
        if (info[0] < 7) {
            return false;
        }

        // Check OSXSAVE in Leaf 1 ECX bit 27
        GetCpuid(1, 0, info);
        if ((info[2] & (1 << 27)) == 0) {
            return false;
        }

        // Check OS support for AVX & AVX-512 state saving via XGETBV
#if defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)
        const unsigned long long xcr0 = _xgetbv(0);
        // Bit 1: SSE, Bit 2: AVX, Bit 5: opmask, Bit 6: ZMM_Hi256, Bit 7: Hi16_ZMM
        if ((xcr0 & 0xE6ULL) != 0xE6ULL) {
            return false;
        }
#else
        return false;
#endif

        // Check Leaf 7 (subleaf 0) for AVX-512F (bit 16), AVX-512BW (bit 30), AVX-512VL (bit 31)
        GetCpuid(7, 0, info);
        const bool avx512f = (info[1] & (1 << 16)) != 0;
        const bool avx512bw = (info[1] & (1 << 30)) != 0;
        const bool avx512vl = (info[1] & (1 << 31)) != 0;
        return avx512f && avx512bw && avx512vl;
    }();
    return supported;
}

const CpuCapabilityInfo& DetectRyzenX3DCpu() {
    static const CpuCapabilityInfo info = []() -> CpuCapabilityInfo {
        CpuCapabilityInfo result{};

        // 1. Query Vendor String
        int cpu_info[4];
        GetCpuid(0, 0, cpu_info);
        char vendor[13]{};
        std::memcpy(vendor + 0, &cpu_info[1], 4); // EBX
        std::memcpy(vendor + 4, &cpu_info[3], 4); // EDX
        std::memcpy(vendor + 8, &cpu_info[2], 4); // ECX
        const bool is_amd = (std::strcmp(vendor, "AuthenticAMD") == 0);

        // 2. Query Family and Model
        GetCpuid(1, 0, cpu_info);
        const u32 base_family = (cpu_info[0] >> 8) & 0x0F;
        const u32 ext_family = (cpu_info[0] >> 20) & 0xFF;
        const u32 family = base_family + ext_family;

        const u32 base_model = (cpu_info[0] >> 4) & 0x0F;
        const u32 ext_model = (cpu_info[0] >> 16) & 0x0F;
        const u32 model = base_model | (ext_model << 4);

        // 3. Query Brand String (Leaves 0x80000002 .. 0x80000004)
        GetCpuid(static_cast<int>(0x80000000), 0, cpu_info);
        const u32 max_ext = static_cast<u32>(cpu_info[0]);
        char brand[49]{};
        if (max_ext >= 0x80000004) {
            GetCpuid(static_cast<int>(0x80000002), 0, reinterpret_cast<int*>(brand + 0));
            GetCpuid(static_cast<int>(0x80000003), 0, reinterpret_cast<int*>(brand + 16));
            GetCpuid(static_cast<int>(0x80000004), 0, reinterpret_cast<int*>(brand + 32));
        }
        std::string brand_str = brand;
        // Trim leading and trailing whitespace
        const auto first = brand_str.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            const auto last = brand_str.find_last_not_of(" \t\r\n");
            brand_str = brand_str.substr(first, (last - first + 1));
        }
        if (brand_str.empty()) {
            brand_str = "Unknown x86_64 Processor";
        }
        result.brand_string = brand_str;

        // 4. Instruction Set Capabilities
        GetCpuid(7, 0, cpu_info);
        result.has_avx2 = (cpu_info[1] & (1 << 5)) != 0;
        result.has_avx512 = HasRuntimeAvx512();

        // 5. Topology detection on Windows
#ifdef _WIN32
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationAll, nullptr, &length);
        if (length > 0) {
            std::vector<u8> buffer(length);
            if (GetLogicalProcessorInformationEx(
                    RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                    &length)) {
                u8* ptr = buffer.data();
                u64 largest_l3_size = 0;
                u64 largest_l3_mask = 0;

                while (ptr < buffer.data() + length) {
                    const auto* info_ex =
                        reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(ptr);
                    if (info_ex->Relationship == RelationProcessorCore) {
                        result.physical_cores++;
                        for (WORD g = 0; g < info_ex->Processor.GroupCount; ++g) {
                            result.logical_threads +=
                                static_cast<u32>(std::popcount(info_ex->Processor.GroupMask[g].Mask));
                        }
                    } else if (info_ex->Relationship == RelationCache) {
                        const auto& cache = info_ex->Cache;
                        if (cache.Level == 3 && cache.GroupCount > 0) {
                            const u64 sz = cache.CacheSize;
                            const u64 mask = cache.GroupMasks[0].Mask;
                            if (sz > largest_l3_size) {
                                largest_l3_size = sz;
                                largest_l3_mask = mask;
                            }
                        }
                    }
                    ptr += info_ex->Size;
                }
                result.vcache_ccd_core_mask = largest_l3_mask;
            }
        }
#endif
        if (result.physical_cores == 0) {
            result.physical_cores = 8;
            result.logical_threads = 16;
        }

        // 6. X3D Verification
        const bool has_x3d_name = (brand_str.find("X3D") != std::string::npos);
        if (is_amd && has_x3d_name) {
            result.is_supported_x3d = true;
            if (family == 0x19) {
                result.zen_gen = (model <= 0x2F) ? 3 : 4;
            } else if (family == 0x1A) {
                result.zen_gen = 5;
            } else {
                result.zen_gen = 4; // Future / fallback
            }
        } else {
            result.is_supported_x3d = false;
            result.zen_gen = 0;
        }

        // 7. Status summary
        if (result.is_supported_x3d) {
            result.status_summary = fmt::format(
                "{} — Supported (Zen {} X3D, {}C/{}T{}{})",
                result.brand_string, result.zen_gen, result.physical_cores, result.logical_threads,
                result.has_avx512 ? ", AVX-512" : (result.has_avx2 ? ", AVX2" : ""),
                result.vcache_ccd_core_mask != 0 ? " [V-Cache Pin Active]" : "");
        } else {
            result.status_summary = fmt::format(
                "{} — Non-X3D (CPU X3D Boost safely bypassed)",
                result.brand_string);
        }

        return result;
    }();
    return info;
}

} // namespace Common
