# Changelog

All notable changes to Yngchii-X3D (High-Performance AMD Ryzen 3D V-Cache & Dynamic VRAM Tuning Fork of Fritchii by Frizti & Zortos) are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [5.8.4.0] - 2026-11-11

### Upstream Fritchii v5.8.4 Synchronizations
- 4f5c53149b Revert EXP47 (read-back lock bits) for v5.8.4
- 58c5571e3c fritchii: Saves always backs up and says where, before and after
- e96eea7e7d Fritchii v5.8.4: version and settings revision 12
- f1cacebce2 fritchii: Saves copies into the target's real user profile, and fixes orphaned saves
- 62bf633408 settings: say that Switch 2 games always use 12 GB when the memory layout is touched
- 04c3d31d8a settings: the 12 GB layout reads "Switch 2", and doesn't count as an OoT override
- f6ce2754c1 main_window: log the folder Open Save Data / Mod Data Location opens
- da25dc32d6 settings: DLSS quality and preset per game too (v5.8.4)
- e74510aa80 video_core: EXP47, answer guest reads of clean GPU-written pages without the cache locks
- e2dad308a6 Fritchii v5.8.3: a Saves window to export, import and copy saves
- 6649abc4f1 Fritchii v5.8.2: ask before copying old data, mods only when ticked
- d042bdd731 Fritchii v5.8.1: copy from the Lichii 150+ folder too (newest saves win, Help > Copy Data)
- 6e11fee517 Fritchii v5.8: settings revision 8
- 08b95c876f Fritchii keeps its own data folder (%APPDATA%\fritchii), imported automatically (v5.8)
- 909be315f9 fritchii: Discord and Donate buttons on the home page, with their names
- 416bfed001 tests: the DLSS settings test checks our K default and preset enum
- ef961c0cef video_core: DLSS quality modes, live apply and status display (port of zortos293/Lichii#1249)
- 063dbb741f patch_manager: water ripples at any frame rate with Dark's Dynamic FPS too (built-in, v5.8)
- 1c8474ee96 maxwell_3d: the back evaluates NVN's conditional render macro (port of Eglantine d3612f82b1 part 1, EXP53)
- 289ffeb16b memory_manager: read EDEN_NX2_NO_TRANSLATION_CACHE once, not on every lookup
- d20c32704c video_core: stop write-protecting guest pages the CPU rewrites every frame (port of Eglantine f27522bfb9, EXP48)
- 85a33019b7 video_core: cache GPU page translations per thread (port of Eglantine b1e650849e, EXP63)
- 3236102f61 core: do not wait for the time services when applying settings during loading (#1168)
- 919d5b0d58 vulkan: keep NVIDIA DLSS on when Streamline warns that the VRAM budget is used up (#941)
- 93423cd43b nx2: skip dynarmic's IR verification pass (EDEN_NX2_JIT_FAST_COMPILE)
- db3fdb2927 textures: swizzle and unswizzle block linear in 16-byte units from a per-GOB offset table (#784)
- d03cd4cd59 Preparing shaders: stamp it with a hash of the shader translator, not the release version
- d9f6b81f81 Fritchii v5.8: version

### Yngchii-X3D Optimizations Maintained
- Dedicated AMD Ryzen 3D V-Cache thread core pinning (GPUFront Core 1, GPUBack Core 2).
- Hardware-gated AVX-512 dirty page scan with clean AVX2 fallback.
- In-place auto-updater targeting Yngchii-X3D releases with pre-release safety.
- Selective NVIDIA Overlay & Game Filter bypass.

---


## [5.7.0] - 2026-11-11

### Upstream Fritchii v5.7 Synchronizations
- 52317bf0e5 Fritchii v5.7: in-app notes
- f3ba26cfe4 Fix NX metadata editing and restore original icons
- d00273c78f Link nxformat 1.0.2 statically
- 330c1e4bb5 Load gamecard tickets first and log firmware fallback
- 2f4c1d100b Keep cart firmware when converting XCI to NX
- c1c1ccbc17 Update nxformat to 1.0.1 (#1078)
- 5d1eb38196 Add support for the NX archive format (#866 with review fixes) (#923)
- 5f8b265ecf Fritchii v5.7: version and settings revision 7
- 5b970411a0 Fritchii v5.7 defaults: EXP49 on, texture cache ceiling 7680 MiB, JIT warm-up on
- 6a29460f5b Zero-page pass: wait out bulk mapping, back off to 5 minutes when it trims little
- 0f0e8470c0 Zero-page pass: log its phases (working set query, page reads, VirtualUnlock runs)
- 25892ea154 vulkan: one command and one pair of barriers for a run of in-order uploads (port of zortos293/Lichii#1245, EXP62)
- 1b65dfe83e vulkan: EXP42=2's image pool holds up to 1.5 GiB
- 3c09b12762 vulkan: EXP42=2 also pools dedicated-allocation render targets
- 28a71116d8 Fritchii: EDEN_NX2_READ_THROTTLE=0 built in (no 40 MB/s game file read pacing)
- e1c7514de7 fsp: EDEN_NX2_READ_THROTTLE=0 turns the 40 MB/s game file read pacing off
- c43b29327f texture_cache: keep the collector's thresholds within the host's commit (port of zortos293/Lichii#1210)
- ad4b2efe25 Frame dump: capture_frame arms it mid-game (no boot switch)
- ad94953dbe nvdrv: retain the process while queued backing refresh runs
- 709c7ff672 nvdrv: always refresh stale device backing
- 506a5ca598 nx2: release the device range of memory that sub 102 destroys
- f84b9056cb nvdrv: map a reused Switch 2 device range to the CPU range's current pages
- 45e164c155 nx2: unmap every GPU range into memory that sub 102 destroys (#1199)
- 3d2bcbb898 Fight counters: front waits by site, cache lock waits by thread, image shapes made
- a85e7344b5 Hitch log: 60 lines a minute (20 hid a 740 ms first load)
- 7147facb77 JIT warm: at most 20 ms of every 100 ms per core
- b0d0ff43f7 EDEN_NX2_JIT_WARM=1: idle cores compile code other cores compiled (off by default)
- 3ae8122cf0 Hitch log: repeat compiles across cores, per-thread CPU, map/unmap/protect split, zero-fill
- dde155e33e Hitch log: recompiling game code, and the process's user/kernel CPU time
- 68b9815db4 EDEN_NX2_ZERO_TRIM=0: no zero-page passes (A/B switch)
- 6c4e3eed1b Hitch log: game file read throttle, file reads, host map/protect; debug write_memory
- 5885850c68 shader: shadow high registers of SHF with a uniform shift amount
- 60377e0804 shader: shadow high registers of seven more instruction forms
- dfac3c45ea shader: shadow high registers of IMAD with a uniform multiplier (#279)
- 169bb1f198 Hitch log: one line per present gap over 50 ms with what held the game up
- 4429e53ca5 OoT high refresh: own cave at main+0x3C46E00, Dark's routine stays

### Yngchii-X3D Optimizations Maintained
- Dedicated AMD Ryzen 3D V-Cache thread core pinning (GPUFront Core 1, GPUBack Core 2).
- Hardware-gated AVX-512 dirty page scan with clean AVX2 fallback.
- In-place auto-updater targeting Yngchii-X3D releases with pre-release safety.
- Selective NVIDIA Overlay & Game Filter bypass.

---


## [5.6.0] - 2026-11-10

### Upstream Fritchii v5.6 Synchronizations
- 696a456641 Fritchii v5.6: in-app notes get Ctrl+U high refresh and the Dynamic FPS mod skip
- 056be33ad6 OoT high refresh: EDEN_NX2_OOT_HIGH_REFRESH=0/1 test override at boot
- 9d216debc0 Dynamic FPS mod skip: only while the built-in is on
- 2d52cdc31e Merge helper/skip-dynfps-mod: Dark's standalone Dynamic FPS mod is skipped and hidden (Fritchii has it built in)
- 7ade2db463 OoT: skip and hide Dark's standalone Dynamic FPS mod, the built-in is used
- ef1ba9c2e8 OoT high refresh: no live switch while shutting down; debug server oot_high_refresh
- 0a3fe1ca43 Merge helper/hr-live: OoT high refresh switches live (Ctrl+U), water ripples above 60 fps (port of zortos293/Lichii#1152)
- 9f6d595912 tests: high refresh rate layout with the ripple accumulator, and the live switch
- 5731635020 OoT: Ctrl+U switches the high refresh rate mode while playing
- 906b154021 OoT: switch the high refresh rate mode while the game runs
- d389685a4c OoT: keep water ripples moving above 60 fps in the high refresh mode
- d041752bcd Fritchii v5.6: version and in-app notes
- 112ba4d8ee Merge helper/report-allocs: reports and fences without per-report allocations (part of zortos293/Lichii#1161)
- 419622329a vulkan: samples query bank host reset on the Vulkan worker
- 0add1e56ae vulkan: master semaphore ticks on their own cache lines
- 5944a098a8 query_cache: free query ids as a vector FIFO
- 3965ffbd0d query_cache: a report operation that fits std::function's own storage
- ed74cb0081 video_core: fence operation lists as vectors that keep their capacity
- 0d9dc6e362 nx2: EXP61 on by default (no empty FlushCommands submits)
- 523fb2861a Merge helper/read-extents: record proven storage read extents (not used yet), kept magic NX2KEPT4
- f28d17136e vulkan: log read extent coverage at the end of a kept preparation
- 984d0ba92a shader: record proven read extents of storage buffers (not used yet)
- e827b23f2d Merge helper/compute-cleanwords: EXP59 & 4 dirty-word walk for compute storage synchronizes, & 8 verifies it
- 7df8caa2b8 buffer_cache: EXP59 & 4 dirty-word walk for compute storage synchronizes, & 8 verifies it
- c338269b92 vulkan: EXP61 skips FlushCommands' submit when nothing was recorded since the last one
- 2dcba2baf8 Merge helper/evicted-pointers: port of zortos293/Lichii#1143 (evicted pipelines keep their storage buffer pointers)
- c0fa5c0902 vulkan: keep an evicted graphics pipeline's storage buffer pointers when it is translated again
- 39b54e91eb texture_cache: EXP58=2 also deletes superseded images the CPU read once
- 15cf552189 Merge helper/compute-counters: compute dispatch zones, read flush zone, binding counters (profile only)
- 753c422e13 nx2 profile: compute dispatch zones, read flush zone and compute binding counters
- f549632af9 nx2: EXP58 on by default (superseded guarded render targets die at 60 frames)
- 8921f9ba7d texture_cache: EXP58 deletes guarded render targets that a newer target of another shape superseded
- 7a6fcd3a36 Ocarina: "Ocarina of Time movies" setting (Automatic / Bilinear / Game's own upscaler)
- 868597700e Ocarina: EDEN_NX2_OOT_MOVIE_NETWORK=1 runs the game's own movie upscaler
- 229417e305 Ocarina: EDEN_NX2_DLSS_JITTER gives DLSS the camera jitter and texture bias FSR gets
- 1edd5dbf0c vulkan: EDEN_NX2_AMD_APP_CACHE=1 keeps pipelines built in play in our own VkPipelineCache on AMD
- 031f0741ce updater: never offer a release whose notes carry <!-- fritchii:no-update -->
- cd1582e0db texture_cache: EXP7 splits the clear rule's first refusal and logs scissor numbers; fix a NUL char literal; log slow zero-page passes
- 6129793a6b nx2: EXP56 on by default (no guest upload for a cleared depth target larger than its colour)
- 0fa0a0b189 Ocarina: DLSS keeps to the upscale passes it replaced before the FSR port
- d4bbab0702 vulkan: log the driver cache folder, preparation mode, unclean stops, VK_KHR_pipeline_binary
- f93124a6b8 texture_cache: EXP56 skips a cleared depth target's upload; EXP57 caps collector GPU waits
- 208cb2579e vulkan: EXP55 patches the key's vertex attribute types alone (=2 verifies against Refresh)
- ec3df1fb79 vulkan: split PIPELINE_CLEAN's "other state" misses by part; EXP54 compares the topology's class
- c99790b878 shader: EDEN_NX2_IR_POOL_CAP_MIB returns oversized IR pool memory after each job
- 0fd9d9e257 FSR 4: accept AMD's "Inc." signer names
- 669751e2a0 texture_cache: scale the collector's ceiling with the device budget on 10+ GiB cards
- eed56feedf Merge helper/fsr-port: AMD FSR 3.1 and FSR 4 as OoT anti-aliasing choices (zortos #1060-#1062)
- 26626344dd Merge helper/high-refresh: OoT high refresh rate opt-in (off by default)
- 363832faf7 OoT: opt-in high refresh rate (experimental, off by default)
- 413d853f0f settings, yuzu: Ocarina's FSR 3.1 and FSR 4 as choices of "Ocarina of Time anti-aliasing"
- a506cd318e vulkan: use the compute mesh fallback on AMD's own driver too
- a567ca378e vulkan: replace EDEN_NX2_NATIVE_MESH with a Mesh Shaders setting
- acd2cce6e5 vulkan: keep the RADV mesh fallback unless EDEN_NX2_NATIVE_MESH is 1, and say what it skips
- 8b85ce1bc8 vulkan: draw task/mesh through the compute fallback on RADV (GPU resets, #316)
- 824123205a Ocarina: AMD FSR 4 through AMD's official FidelityFX runtime, downloaded on request (rework of #915) (#1062)
- 24b65791bc Ocarina FSR: camera jitter and texture LOD bias without experiment switches (port of #914) (#1061)
- 4f3693cef0 Merge helper/lookup-programs: shader-only pipeline lookup (EXP52, off; =2 verifies)
- 59fed79d56 Ocarina of Time: AMD FSR 3.1 in place of the game's anti-aliasing network (port of #695) (#1060)
- 6269978850 vk_pipeline_cache: programs-only clean lookup keys only the attribute types (EXP52, off)
- 81ab48f7da common: the zero-page trim stops reading a page at its first non-zero line
- f8fed26ab6 vk_pipeline_cache: scale the graphics eviction limit only on PCs under 24 GB
- 38b8812570 Merge helper/ram-evict (helper)
- 77d7a70cda Merge helper/clean-blend (helper)
- d0fa67cd9b vk_pipeline_cache: graphics eviction threshold by RAM size, compute eviction (EXP51, off)
- f37d573b92 vk_pipeline_cache: clean lookup compares blend state by value (EDEN_NX2_EXP50, off)
- a7226727cf settings: no FXAA on top of the game's anti-aliasing; Frizti revision 6 (v5.6)
- 6aeded3531 Merge movie presentation by timestamp (EDEN_NX2_VIDEO_PTS_SYNC, on) from helper branch
- f40398a7cc nx2 video: show movies by their timestamps (EDEN_NX2_VIDEO_PTS_SYNC, on)
- cef673c58f core: the memory probe no longer walks the heaps by default
- e855babe29 Merge movie A/V drift trace, shorter frame round trip and decoder hardening from helper branch
- 2d476b6833 audio_core: SDL3 sink streams keep their name
- 61fe75830c Merge memory owners probe (port of zortos #951 + pipeline/guest/heap counts) from helper branch
- 9b767005f3 nx2 video: measure movie A/V drift (EDEN_NX2_VIDEO_TRACE) and shorten the frame round trip
- 9e5ce77ab9 Merge #1028 async read-back port (EXP49, off by default) from helper branch
- 7876c856cd fritchii: Discord and Donate buttons
- f00e2692f5 video_core: async read-back behind EXP49 (port of zortos293/Lichii#1028's core)
- 30bddb3d78 nx2 video: a bad frame or a failing decoder never takes the game down
- b3c7958523 video_core: EDEN_NX2_MEM_TRACE memory trace (port of zortos293/Lichii#951) and a "Memory owners" line every 10 s
- 25984e0e0b buffer_cache: compare a checked dispatch's tail with guest memory from before it
- 571023fcc3 vulkan: don't write render target components the fragment shader doesn't output (Fritchii #29, port of Lichii #353)
- eba45b151a shader: keep signed F32 to S32 conversion within range (port of zortos293/Lichii#1051)
- 8100a67d2b fritchii: Reset to Defaults saves a backup, Restore Settings brings one back
- 1521079e9f common: preserve empty ParamPackage values when deserializing
- f253d02aa5 vulkan: keep dispatching while a native compute pipeline is rebuilt (port of zortos293/Lichii#969)
- a26b263398 export_log: list Ocarina's game-specific settings
- 8627ff4233 fritchii: show VRAM in the performance overlay
- cb0c4252c5 settings: Ocarina of Time stays at its speed when the speed limit is unlocked
- e6e0a8083c audio_core: fix the sign of the ADPCM nibble steps
- 58f55cb7d9 fritchii: show RAM in the performance overlay
- 9f6bdb46f7 common: trim all-zero guest pages out of the working set (port of zortos293/Lichii#961)
- a2cf026b2c buffer_cache: check write extents of bindings over several buffers too
- 115e2dbbe1 buffer_cache: check each compute program's write extent before trusting it
- 2bf4398364 Revert "buffer_cache: keep CPU writes from uploading over unmarked GPU writes"
- b4b6240de2 buffer_cache: keep CPU writes from uploading over unmarked GPU writes
- 99910c0621 frontend: drop the built-in 64 KiB cap on written storage buffer marks
- bc6f39158c log: keep logging when another Fritchii still holds the log file
- 37deb507bf linux: make Fritchii v5 build on Linux (GCC and clang, -Werror)
- 672d8b36e8 loader: show the en-us NUP title in the library when there is one
- a645ac3e15 loader: prefer the en-us NUP title over the default title
- 253761862d loader: show the NUP library title from the base package metadata
- f34a4dff5d loader: take each NUP metadata field from the base package separately
- b14d20aa4e loader: read NUP metadata from base package even if ExeFS is empty
- 31e81b4cc1 fix: preserve user overrides on prepatched built-in records
- 5ee1a7a011 audio: Switch 2 microphone capture, and silence when the host mic can't open (OoT ocarina panic)
- 87a50046b8 updater: versions with a patch number and two-digit minors (5.6.1, 5.10)
- 4562af17f2 common: reserve the last page table level from the range's last byte (zortos293/Lichii#957)
- 40872ee016 core: give each DeviceMemoryManager its own translation generations (#984)

### Yngchii-X3D Optimizations Maintained
- Dedicated AMD Ryzen 3D V-Cache thread core pinning (GPUFront Core 1, GPUBack Core 2).
- Hardware-gated AVX-512 dirty page scan with clean AVX2 fallback.
- In-place auto-updater targeting Yngchii-X3D releases with pre-release safety.
- Selective NVIDIA Overlay & Game Filter bypass.

---


## [5.5.0] - 2026-11-09

### Upstream Fritchii v5.5 Synchronizations
- 4012e5271e Fritchii v5.5: NVIDIA shader cache size for Fritchii's process, version and in-app notes
- d64f34764a Fritchii: frame generation never runs (a value left on by an older build made the camera flicker)
- bfa630ca7b Fritchii: the Prepare Shaders warm-up window says it is the optional quick part, and no longer asks AMD players to turn shader libraries on
- bd59d18c6a Fritchii: EDS2 and dynamic vertex input in the tuned settings (Revision 5); a GPU thread pinning note
- 35cb046326 settings: DLSS global only (a per-game value hid the checkbox), and a DLSS preset setting
- 8ce6c1e0d4 Fritchii v5.4: version and in-app notes (AMD shader preparation)
- 9ea66c8273 vulkan: precompiled pipelines recorded with a dynamic logic op build where it is static (AMD)

### Yngchii-X3D Optimizations Maintained
- Dedicated AMD Ryzen 3D V-Cache thread core pinning (GPUFront Core 1, GPUBack Core 2).
- Hardware-gated AVX-512 dirty page scan with clean AVX2 fallback.
- In-place auto-updater targeting Yngchii-X3D releases with pre-release safety.
- Selective NVIDIA Overlay & Game Filter bypass.

---


## [5.3.0] - 2026-11-09

### Upstream Fritchii v5.3 Synchronizations
- Upstream synchronizations from fritchii-v5.3

### Yngchii-X3D Optimizations Maintained
- Dedicated AMD Ryzen 3D V-Cache thread core pinning (GPUFront Core 1, GPUBack Core 2).
- Hardware-gated AVX-512 dirty page scan with clean AVX2 fallback.
- In-place auto-updater targeting Yngchii-X3D releases with pre-release safety.
- Selective NVIDIA Overlay & Game Filter bypass.

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
