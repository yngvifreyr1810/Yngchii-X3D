// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include "common/common_types.h"

namespace Common {

struct CpuCapabilityInfo {
    bool is_supported_x3d{false};
    u32 zen_gen{0};               // 3, 4, 5, or 0
    u32 physical_cores{0};
    u32 logical_threads{0};
    u64 vcache_ccd_core_mask{0};
    std::string brand_string;
    std::string status_summary;
    bool has_avx2{false};
    bool has_avx512{false};
};

/// Cached runtime detection of AMD Ryzen 3D V-Cache processors & CPU topology.
/// Safe to call anywhere, thread-safe, cached on first call.
const CpuCapabilityInfo& DetectRyzenX3DCpu();

/// Standalone runtime check for AVX-512 support (CPUID Leaf 7 + OSXSAVE + XGETBV).
bool HasRuntimeAvx512();

} // namespace Common
