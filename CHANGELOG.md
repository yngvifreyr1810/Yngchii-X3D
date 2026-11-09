# Changelog

All notable changes to Yngchii-X3D (High-Performance AMD Ryzen 3D V-Cache & Dynamic VRAM Tuning Fork of Fritchii by Frizti & Zortos) are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [5.1.0] - 2026-11-09

### Added & Enhanced
- **Fritchii v5 & v5.1 Foundation Integration**: Built directly upon Frizti's official Fritchii v5 and v5.1 releases, integrating all core rendering and engine updates.
- **Dedicated AMD Ryzen 3D V-Cache Core Pinning**: Ported the proven multi-threaded affinity architecture from v4.1 onto the v5 codebase:
  - `GPUFront` pinned to physical Core 1 (`0x4`).
  - `GPUBack` pinned to physical Core 2 (`0x10`).
  - Guest Switch 2 emulation threads pinned cleanly across physical cores (`0x5541`).
- **Hardware-Gated AVX-512 Dirty Page Change Detection**: High-throughput 256-byte dirty-page scanning (`ComparePageAvx512`) gated by runtime CPUID validation (`Common::HasRuntimeAvx512()`), providing accelerated memory checks on AMD Zen 4/5 and Intel AVX-512 while falling back to AVX2 on Zen 3 and non-AVX-512 CPUs.
- **Selective NVIDIA Overlay Bypass**: Intercepts intrusive background overlays (`NvCamera`, `NV_nomad`, `NVIDIA_Overlay`) that cause lock contention, while preserving 100% full compatibility for **MSI Afterburner / RivaTuner (RTSS)** monitoring.
- **In-Emulator Auto-Updater & In-Place Installer**:
  - Re-anchored update checker to `https://api.github.com/repos/yngvifreyr1810/Yngchii-X3D/releases?per_page=100`.
  - Added dynamic executable detection supporting `Yngchii-X3D.exe` and `lichii.exe` for seamless in-place updates.
  - Multi-asset hardware awareness: prefers Zen 4/5 AVX-512 packages when running on AVX-512 builds, and Universal AVX2 packages on AVX2 builds.
  - Pre-release safety gate: ignores GitHub Pre-releases so testing builds are not distributed until promoted.
- **Automated Upstream Sync Bot**: Integrated `.github/workflows/upstream-auto-merge.yml` for automated 2-hour upstream tracking and pre-release packaging.

### Upstream Fritchii v5.1 Fixes Incorporated
- **White Fog Resolution**: Reverted experimental noperspective varyings (Lichii#820 port) to eliminate white fog at Hyrule Field drawbridge and line artifacts in Kokiri Forest.
- **Glowing Geometry Resolution**: Reverted block-widened invalidation accumulator (Lichii#700 port) to prevent stale guest memory reloads over GPU writes.
- **Dialogue Text Fix**: Added address-based protection in texture cache to resolve garbled dialogue box text in Ocarina of Time.
- **Delete Shader Cache**: Added one-click shader cache cleaning in Help menu and Home card with user confirmation.
- **High-DPI Display Scaling**: Fixed Game Settings dialog on 1440p displays at 175% scale (`FitHighDpiScaleToScreen()` and scrollable tabs).

---

## [4.1.1] - 2026-11-09

### Added
- **Universal `-march=x86-64-v3` Architecture**: High-performance portable binary targeting AVX2, FMA, BMI1/2, ensuring crash-free execution on AMD Zen 3 (Ryzen 5 5500X3D / 5700X3D / 5800X3D) and non-AVX-512 systems.
- **2-Tier Modular Optimization Architecture**:
  - *Tier 1 (Universal GPU/VRAM & Engine Optimizations)*: Zero-sleep futex spinning, dynamic WDDM heap budgeting, 240Hz pacing, selective overlay filter.
  - *Tier 2 (AMD Ryzen X3D CPU Boost)*: Hardware-gated core pinning and SMT worker isolation.
- **Diagnostic Logging**: Added early startup hardware diagnostic logging to `%APPDATA%\eden\log\yngchii_startup.log`.

### Fixed
- **7168 MiB VRAM Clamp Bug Resolved**: Restored full physical device-local VRAM detection (12GB on RTX 3080 Ti) and optimal texture cache headroom (62.5% TC Expected, 68.75% TC Critical).

---

## [4.1.0] - 2026-11-08

### Added
- Initial community release of Yngchii-X3D based on Fritchii v4.1 by Zortos.
- Dedicated GPU submission thread core pinning on AMD 3D V-Cache CCDs.
- 3D V-Cache zero-sleep user-mode futex synchronization.
- Hardware-agnostic auto-adaptive GPU VRAM budgeting.
