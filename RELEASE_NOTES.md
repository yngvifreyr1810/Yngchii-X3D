# ⚡ Yngchii-X3D v4.1 — Release Notes

> **High-Performance AMD Ryzen 3D V-Cache (7800X3D / 9800X3D / Zen 4/5) & Universal GPU Auto-Adaptive Fork of [Frizti/Fritchii](https://github.com/Frizti/Fritchii)**

---

### 🌟 What's New in Yngchii-X3D v4.1

#### 1. ⚡ Dedicated GPU Thread Core Pinning (**Exclusive — Not in Upstream v4.1**)
* Vulkan submission threads (`GPUFront` and `GPUBack`) are now pinned to dedicated physical cores on the 3D V-Cache CCD:
  * **`GPUFront`**: Pinned to **Core 1** (`LP2`, affinity mask `0x4`).
  * **`GPUBack`**: Pinned to **Core 2** (`LP4`, affinity mask `0x10`).
  * **Guest CPU Emulation Threads**: Isolated to primary SMT physical cores (affinity mask `0x5541`).
  * **Worker Threads**: Relegated to secondary SMT logical cores (affinity mask `0xa802`).
* Eliminates SMT execution port contention and eliminates 30–40ms micro-stutters during heavy draw calls.

#### 2. 🛡️ Selective NVIDIA Overlay & Game Filter Bypass
* Automatically blocks invasive NVIDIA background injection layers (`NvCamera`, `NV_nomad`, `NVIDIA_Overlay`, `GalaxyOverlay`, `EOSOverlay`) that steal 8–15% FPS and allocate phantom VRAM.
* Preserves **MSI Afterburner / RivaTuner Statistics Server (RTSS)** so framerate and frametime overlays remain 100% operational.

#### 3. ⚡ 3D V-Cache Zero-Sleep GPU Split
* Replaces Windows kernel thread sleeps with user-mode futex spinning (`win32_futex.h`, `SplitSpinCount=32768`).
* Reduces `ZwWaitForAlertByThreadId` kernel wait overhead from 21.1% down to **0.0%**, slashing producer-consumer latency to **< 40 ns** in the 96MB L3 cache.

#### 4. 💾 Auto-Adaptive GPU VRAM & Texture Cache Budgeting
* Detects physical VRAM on any GPU (6GB, 8GB, 10GB, 12GB, 16GB, 24GB+) and establishes safe WDDM thresholds:
  * **Device-Local Heap Limit**: 77% of physical VRAM (safely reserving ~23% for OS compositing/DWM).
  * **Texture Cache Expected**: 62.5% of physical VRAM.
  * **Texture Cache Critical**: 68.75% of physical VRAM.
* Prevents catastrophic PCIe paging stutters when games stream high-resolution textures.

#### 5. 🎮 Discord Rich Presence Integration
* Connects directly via Win32 Named Pipes (`\\.\pipe\discord-ipc-0`..`9`).
* Displays current game title, elapsed play time, and optimization status on your Discord profile.
* Persistence bug resolved: configuration checkbox properly saves and restores state across sessions in `qt-config.ini`.

#### 6. 🎨 High-DPI Windows Shell Branding
* Multi-resolution `.ico` icon embedded into the `.exe` resource table with uncompressed 32-bit BGRA DIB frames (16x16 to 128x128) and PNG (256x256), rendering crisply across all Windows 11 Explorer views.

---

### 📦 Asset Checksums
* **`Yngchii-X3D-v4.1-Windows-x64.zip`**
  * Size: `107,430,206 bytes` (~102.4 MB)
  * SHA-256: `72635c0954590895dc7124e5b1edf079fa15c3f232b564fdc2d60d9aaa75811e`

---

### 🙏 Acknowledgments & Credits
* **[Zortos / Frizti](https://github.com/Frizti)**: Author and creator of the upstream [Fritchii](https://github.com/Frizti/Fritchii) project.
* **Eden & Eglantine Teams**: For their pioneering work on Switch emulation.
* **yuzu Emulator Project**: Original emulator architecture.
