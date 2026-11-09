<p align="center">
  <img src="dist/branding/yngchii_logo.png" alt="Yngchii-X3D Logo" width="260" style="border-radius: 12px; box-shadow: 0 4px 20px rgba(0,0,0,0.3);">
</p>

# ⚡ Yngchii-X3D v5.1

> **High-Performance AMD Ryzen 3D V-Cache (7800X3D / 9800X3D / Zen 4/5) Tuning Fork of [Frizti/Fritchii](https://github.com/Frizti/Fritchii) (v5 / v5.1)**  
> *Bản fork hiệu năng cao tối ưu sâu cho kiến trúc AMD Ryzen 3D V-Cache và đồng bộ liên tục với các bản phát hành chính thức của Frizti/Fritchii.*

[![Upstream: Fritchii v5.1](https://img.shields.io/badge/Upstream-Fritchii%20v5.1-blue.svg)](https://github.com/Frizti/Fritchii)
[![Architecture: AMD Zen 4/5 3D V-Cache](https://img.shields.io/badge/Architecture-AMD%20X3D%20%7C%20Zen%204%2F5-orange.svg)](https://www.amd.com)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-green.svg)](LICENSE.txt)
[![Discord RPC: Enabled](https://img.shields.io/badge/Discord%20RPC-Active-5865F2.svg)](https://discord.com)

---

## 🗺️ Table of Contents / Mục lục
- [English Documentation](#-english-documentation)
  - [1. Overview](#1-overview)
  - [2. Architectural Foundation & Hardware Tuning](#2-architectural-foundation--hardware-tuning)
  - [3. In-Emulator Auto-Updater & Upstream Sync](#3-in-emulator-auto-updater--upstream-sync)
  - [4. Upstream Fritchii v5.1 Fixes Incorporated](#4-upstream-fritchii-v51-fixes-incorporated)
  - [5. Building from Source](#5-building-from-source)
  - [6. Credits & Acknowledgments](#6-credits--acknowledgments)
- [Tài liệu Tiếng Việt](#-tài-liệu-tiếng-việt)

---

# 🚀 English Documentation

## 1. Overview
**Yngchii-X3D** is a specialized, community-driven tuning fork of **[Frizti/Fritchii](https://github.com/Frizti/Fritchii)** (developed by Zortos and Frizti, built upon Eglantine, Eden, and yuzu). It preserves all upstream features while introducing surgical, hardware-specific optimizations for AMD Ryzen 3D V-Cache processors (such as the **Ryzen 7 7800X3D**, **9800X3D**, and Zen 4/5 architectures) and intelligent in-place updates.

Yngchii-X3D maintains clean architectural tracking of upstream Fritchii releases, ensuring players receive all new renderer bug fixes alongside high-performance hardware tuning.

---

## 2. Architectural Foundation & Hardware Tuning

Building upon Zortos and Frizti's multi-threaded GPU split engine in Fritchii v5, Yngchii-X3D contributes hardware-tailored affinity and memory optimizations:

### ⚡ Dedicated GPU Thread Core Pinning on 3D V-Cache CCD
- **Hardware-Tailored Core Affinity**: On AMD Ryzen X3D processors, Vulkan submission threads are mapped directly to dedicated physical execution cores on the 3D V-Cache CCD:
  - **`GPUFront`** is pinned strictly to **Core 1 (mask `0x4`)**.
  - **`GPUBack`** is pinned strictly to **Core 2 (mask `0x10`)**.
  - **Switch 2 Guest CPU Cores** run across remaining primary physical cores (mask **`0x5541`** across Cores 0, 3, 4, 5, 6, 7).
- **Benefit**: Completely eliminates SMT execution port contention and OS thread migration stalls, delivering smooth frame delivery and low frametime variance.

### ⚡ Hardware-Gated AVX-512 Dirty Page Change Detection
- **Accelerated Memory Checks**: Implements accelerated 256-byte dirty-page scanning (`ComparePageAvx512`) gated by runtime CPUID validation (`Common::HasRuntimeAvx512()`).
- **Safe Fallback**: Dynamically executes on Zen 4, Zen 5, and Intel AVX-512 systems while cleanly falling back to vector AVX2 on Zen 3 and non-AVX-512 systems without illegal instruction crashes (`SIGILL`).

### ⚡ Selective NVIDIA Overlay & Game Filter Bypass
- **Targeted Hook Filtering**: Uses selective layer filtering (`VK_LOADER_LAYERS_DISABLE=*NvCamera*,*NV_nomad*,*NVIDIA_Overlay*`, `NV_ALLOW_ANSEL=0`, `ANSEL_SDK_DISABLE=1`) to eliminate driver-level lock contention and memory overhead from NVIDIA Ansel and ShadowPlay.
- **Monitoring Compatibility**: Keeps **MSI Afterburner / RivaTuner (RTSS)** 100% operational for live performance monitoring.

---

## 3. In-Emulator Auto-Updater & Upstream Sync

### Built-In Auto-Updater
- **In-Place Seamless Updates**: The emulator includes a built-in update dialog and in-place installer querying `https://api.github.com/repos/yngvifreyr1810/Yngchii-X3D/releases`.
- **Hardware-Aware Asset Selection**: Automatically prefers optimized packages (`*Zen4-AVX512*.zip` on AVX-512 builds, and `*Universal-AVX2*.zip` on AVX2 builds).
- **Pre-Release Protection**: Automated bot builds published as Pre-releases (`--prerelease`) are excluded from auto-update prompts until officially promoted by maintainers.

### Automated Upstream Synchronization
- **GitHub Actions Bot (`.github/workflows/upstream-auto-merge.yml`)**: Runs automatically every 2 hours, fetching upstream `Frizti/Fritchii` tags. Whenever a new upstream tag is released (e.g. `fritchii-v5.2`), the bot cleanly merges upstream updates, updates `CHANGELOG.md`, and creates a corresponding GitHub Pre-release.

---

## 4. Upstream Fritchii v5.1 Fixes Incorporated

Yngchii-X3D v5.1 fully incorporates all upstream hotfix improvements from Frizti's `fritchii-v5.1`:
1. **White Fog Elimination**: Reverted experimental noperspective varyings (Lichii#820 port), fixing solid white fog at the Hyrule Field drawbridge on modern RTX cards and line artifacts in Kokiri Forest fog.
2. **Glowing Geometry Fix**: Reverted block-widened invalidation accumulator (Lichii#700 port), preventing stale memory reloads over GPU writes.
3. **Dialogue Box Text Corruption Fix**: Protected dynamic text render targets by address in texture cache, preventing garbled dialogue text in Ocarina of Time.
4. **Delete Shader Cache**: Added one-click shader cache cleaning in Help menu and Home card with user confirmation.
5. **High-DPI Display Scaling Overhaul**: Added `FitHighDpiScaleToScreen()` in `src/yuzu/main.cpp` and scrollable tab areas in Game Settings, fixing layout issues on 1440p displays at 175% scale.

---

## 5. Building from Source

### Prerequisites (Windows 64-bit)
- CMake 3.25+
- Ninja build system
- Clang 18+ or GCC 14+ (MSYS2 MinGW-w64)
- Vulkan SDK 1.3+
- Qt 6.7+

### Build Commands
```powershell
# Clone repository with submodules
git clone --recursive https://github.com/yngvifreyr1810/Yngchii-X3D.git
cd Yngchii-X3D

# Configure build directory
cmake -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DENABLE_QT=ON `
  -DYUZU_USE_BUNDLED_VCPKG=OFF `
  -DYUZU_TESTS=OFF

# Compile
ninja -C build

# The resulting binary is generated at: build/bin/Yngchii-X3D.exe
```

---

## 6. Credits & Acknowledgments
- **[Frizti / Zortos](https://github.com/Frizti/Fritchii)**: For the foundational Fritchii architecture, GPU split engine, and ongoing emulator maintenance.
- **Eden & Eglantine Teams**: For their open-source foundational contributions.
- **yuzu & Sudachi Contributors**: For pioneering Nintendo Switch emulation in C++.

---

# 🇻🇳 Tài liệu Tiếng Việt

## 1. Tổng quan
**Yngchii-X3D** là bản fork hiệu năng cao được phát triển dựa trên nền tảng **[Frizti/Fritchii](https://github.com/Frizti/Fritchii)** (tác giả Zortos và Frizti). Dự án tập trung vào việc tinh chỉnh luồng xử lý và tối ưu hóa bộ nhớ đệm L3 cực lớn (96MB 3D V-Cache) trên các vi xử lý **AMD Ryzen 7 7800X3D**, **9800X3D** và kiến trúc Zen 4/Zen 5, mang lại độ ổn định khung hình mượt mà nhất.

Bản fork này duy trì sự đồng bộ chặt chẽ với Fritchii upstream, đảm bảo kế thừa toàn bộ các bản sửa lỗi đồ họa mới nhất.

---

## 2. Các tinh chỉnh cốt lõi

### ⚡ Ghim nhân luồng đồ họa chuyên biệt (Dedicated Thread Core Pinning)
- Kế thừa kiến trúc tách luồng GPU của Fritchii v5, Yngchii-X3D ghim cố định 2 luồng nạp lệnh Vulkan vào 2 nhân vật lý riêng biệt trên CCD 3D V-Cache:
  - **`GPUFront`**: Ghim cố định vào **Core 1 (`0x4`)**.
  - **`GPUBack`**: Ghim cố định vào **Core 2 (`0x10`)**.
  - **Các luồng CPU giả lập Switch 2**: Phân bổ đều trên các nhân vật lý còn lại (`0x5541`).
- Triệt tiêu hiện tượng tranh chấp luồng SMT và giật khung hình do hệ điều hành di chuyển luồng liên tục.

### ⚡ Quét trang bộ nhớ dirty bằng lệnh AVX-512
- Sử dụng hàm quét trang bẩn `ComparePageAvx512` tốc độ cao với cổng kiểm tra CPUID thời gian thực (`Common::HasRuntimeAvx512()`).
- Tự động kích hoạt trên Zen 4, Zen 5 và Intel AVX-512, đồng thời tự động chuyển về AVX2 trên Zen 3 (Ryzen 5 5500X3D / 5700X3D) giúp tránh hoàn toàn lỗi văng game `SIGILL`.

### ⚡ Lọc lớp phủ NVIDIA chọn lọc (Selective Overlay Bypass)
- Tự động chặn các hook ngầm của NVIDIA Ansel và ShadowPlay gây nghẽn khóa và tốn bộ nhớ, đồng thời giữ cho **MSI Afterburner / RTSS** hoạt động bình thường 100%.

### ⚡ Bộ tự động cập nhật In-App & Workflow đồng bộ Upstream
- Tích hợp sẵn hộp thoại cập nhật ngay trong ứng dụng, tự động tải gói cài đặt in-place từ repository `yngvifreyr1810/Yngchii-X3D`.
- Tích hợp workflow GitHub Actions tự động kiểm tra upstream mỗi 2 giờ để đồng bộ các bản vá mới từ tác giả Frizti.

---

## 3. Lời cảm ơn (Credits)
- Dự án chân thành cảm ơn **Frizti & Zortos** vì những đóng góp đột phá trong dự án Fritchii v5.
- Cảm ơn các nhà phát triển dự án **Eden, Eglantine, yuzu và Sudachi**.
