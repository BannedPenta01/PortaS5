# Phase 1 — KytyPS5 Host Architecture & Performance Report

Date: 2026-10-07. Source: GitHub KytyPS5/KytyPS5 `main` (shallow import at
commit matching the bundled build `KytyPS5-2026-09-27-421684e`).
Local build reproduced in `/userdata/system/kytyps5-src/_Build` with
clang-16 + ld.lld, Release, `-DKYTY_BUILD_LAUNCHER=OFF`, `-DBUILD_TESTING=OFF`.

## 1. High-level architecture

KytyPS5 is a **compatibility-layer emulator**: the PS5 CPU is AMD Zen 2
x86-64, so guest machine code executes **natively** on the host (no full
interpreter/recompiler). Emulation effort is concentrated in the GPU command
processor, graphics rendering (Vulkan), kernel synchronization, AGC command
processing, audio, and loader/patching.

Subsystems:

- **CPU emulation** (`src/loader/*`): native execution; tiny TLS trampolines
  (`jit.h` Jit::SafeCall), exception-handler-driven instruction emulator
  (`x64InstructionEmulator.cpp`), static instruction patcher
  (`guestInstructionPatcher.cpp`, Zydis scan, patches `rdpid`/`extrq`/`clwb`/
  `reciprocal_sqrt`).
- **Memory** (`src/kernel/memory.cpp`, `memoryAddressSpace.inc`):
  13.5 GiB guest RAM floor (`PhysicalMemory::TotalSize()=13824*1024*1024`),
  memfd-backed, PRT aperture at 0xf00000000, per-title flexible-memory cut
  (`SetFlexibleMemorySize`).
- **Kernel sync** (`kernel/pthread.cpp` ~4400 LoC, semaphore/eventQueue/
  eventFlag/syncOnAddress): full pthread layer on host pthreads, with
  10 ms signal-poll slice stepping on waits.
- **Graphics command path**: `GuestGpu::ThreadRun` (Thread_Gpu) consumes
  guest PM4/AGC command buffers (`guest_gpu/pm4.cpp`, `graphicsRun.cpp`),
  then `host_gpu/renderer/*` builds Vulkan work (tiler, bufferCache,
  textureCache, descriptorHeap, renderContext, commandScheduler).
- **Shader recompilation** (`graphics/shader/recompiler/*`): PS5 shader
  binary → decode → CFG structurize → custom IR → optimization passes
  (SSA, const-prop, DCE, resource tracking) → SPIR-V emission, validated by
  SPIRV-Tools, fed directly to `vkCreateShaderModule`.
- **Pipeline cache** (`host_gpu/renderer/pipeline/pipelineCache.cpp`):
  in-process `unordered_map` for programs/pipelines; on-disk
  `_PipelineCache/<id>.bin` = raw `vk::PipelineCache` blob +
  `DriverCacheSignature` header (git date, vendor/device/driver, UUID, XXH3).
- **Presentation** (`graphics/presentation/*`): swapchain, present scheduler,
  imgui overlay, RenderDoc, window (Thread_Window).
- **Audio** (`libs/audio*.cpp`, `ajm*`, `libAtrac9`, ffmpeg): SDL3 streams;
  Atrac9/AAC/MP3/Opus decode in AJM; avPlayer uses 8 guest worker threads.
- **Authoring/tools**: profiler (Tracy optional), RenderDoc hooks, stdout/log
  channels, imgui debug windows, command-buffer dump.

## 2. Observable performance problems (prioritized)

**P0 — Shader/pipeline creation stalls on the guest-GPU thread.**
All shader translation/compilation/pipeline creation is **synchronous** on
Thread_Gpu (`ProgramCache::Get` → TranslateProgram → SPIR-V →
`vkCreateShaderModule` → inline `vkCreate*Pipelines`). No async compile,
no prewarm, no SPIR-V/IR disk persistence; only the driver-level pipeline
cache blob persists. Every new shader or permutation causes a frame-time
hitch on compatible-but-slow handheld GPUs (AMD CUs fewer, shared L3).

**P0 — Lack of bounded caches / accumulation.**
Program and pipeline maps grow unbounded per process; no LRU eviction.
On memory-constrained handhelds this inflates RSS with little benefit.

**P1 — Synchronization granularity.**
`MasterSemaphore` timeline waits from any `FlushAndWait/Finish/UnmapMemory`,
`CommandScheduler::Submit` under a queue mutex, coarse
`eAllCommands` barriers, every-dispatch `ShaderWriteHazardBarrier`.
Concurrent CPU/GPU overlap exists via command scheduler tick recycling but
pacing can still serialize (see `FlushAndWait`).

**P1 — Sleep/poll tuning.**
10 ms condvar/signal-poll slice in guest waits; audio submit hot path uses
1 ms `SleepMicro` loops with 200 ms escape; `GuestGpu::ThreadRun` idle
fallback wakes every 100 ms. These hurt frame pacing granularity and waste
power; on battery this is undesirable.

**P1 — Guest memory budget.**
Fixed 13.5 GiB mapping; on 8 GB shared-memory handhelds this forces the
OS to swap/underlive pages. The flexible-memory cut helps but baseline is
large. For an 8 GB system we should lower the practical working set via
the existing flexible-memory path or a profile-driven cap, without
breaking games (flag-gated, not default for desktops).

**P2 — Allocator/copy hot paths.**
Per-frame temporary allocations (descriptor writes, command buffer chunking,
stream buffer staging rings) should be pooled/reused. ZArchive reads on the
read path are mutex-serialized with a decompression cache — fine for one
title, but no readahead.

**P2 — Validation enabled in shipped config.**
`Kyty.ini` ships `--shader-validation true` and `--shader-optimization-type
Performance`; every new shader runs `spvtools::SpirvTools` validation on the
compile path. For a handheld-focused fork, validation should be off by
default in release performance builds (keep it opt-in) — it costs compile
latency on every shader.

**P2 — No adaptive behavior.**
No detection of AMD APU/iGPU features (e.g. whether pipeline-cache interval
hints or `VK_AMD_shader_info` are available), no worker-count/cache-size
adaptation by RAM, no thermal/power aware behavior.

## 3. Where time is spent (expected, to be confirmed with profiling)

- GPU: shader compilation latency at scene transitions/new pipelines;
  barriers; tiling/upload compute for texture cache; present-mode Mailbox
  pacing.
- CPU: IR passes + SPIR-V emit + SPIR-V validation per shader; guest sleep
  polling; audio pacing loops; command dispatch overhead per PM4 packet;
  `.zar` decompression when games stream assets.
- Memory: texture cache tiling duplicates (guest→host translation) and the
  13.5 GiB guest arena.

## 4. Build + baseline

- Build reproduces from upstream `main` with clang-16 + ld.lld after
  supplying submodules, FFmpeg prebuilts, X11/Wayland dev headers, and
  glslangValidator (needed for tiler shaders). See `scripts/` in the fork
  for the exact toolchain recipe.
- `--help` smoke test OK. Baseline frame-time capture requires a real game
  session; the bundled `PPSA02876/PPSA06399/PPSA06406` titles can be used
  via `kyty_run.sh` (requires a display). This environment is headless for
  benchmarking purposes, so the Phase 13 benchmark log will record the
  first constrained run we can execute.

## 5. Plan (gates real work)

Phase 2: handheld profile scaffolding (cache caps, thread counts, sleep
tuning) — opt-in/auto-detect AMD APU, gated by config.
Phase 3: shader/pipeline pipeline: async compile queue, persistent SPIR-V
cache keyed by content hash, dedup, validation off by default, pipeline
creation off the guest-GPU thread.
Phase 4: CPU: trap count, TLS costs, patch coverage audit (already mostly
native).
Phase 5: Vulkan: barrier narrowing, descriptor batching, command buffer
reuse, present-mode adapt.
Phase 6/7: memory (flexible-memory driver config, pool staging buffers),
adaptive workers.
Phase 8/9/12: frame pacing metrics, power/thermal awareness, profiler doc.
Phase 11/13: adaptation + benchmark log per patch.

Every optimization will be: build → benchmark on same title/scene → record
→ verify correctness → keep/revert. No shortcuts that reduce emulation
fidelity: no resolution scale-down by default, no disabling tiling, no
skip of guest sync.

## 6. Phase 2 — Handheld Performance profile (implemented)

New files:
- `src/common/performanceProfile.{h,cpp}` — system detection + resolution:
  - `SystemProfile` (mode, amd_apu_detected, handheld_detected, ram_tier,
    logical_threads), cached once per process.
  - Auto-detects AMD handheld APUs via `/proc/cpuinfo` model name tokens
    (Custom APU 0405 = Steam Deck, Z1/Z1 Extreme, 7040/8040/AI-300 series,
    Phoenix/Rembrandt/Barcelo/Lucienne/Mendocino/Strix/Krackan, 7840U-class
    part names) and handheld form factors via `/sys/class/dmi/id/*`.
  - RAM tiers from `/proc/meminfo`: 8 GB / 16 GB / 24+ GB.
  - Derived policies: `GetPersistentCacheBudgetBytes()` (256/512/1024 MB),
    `GetRecommendedParallelCompiles()` (≤ cores/2, ≤4 on handheld).
- Config plumbing: `ConfigOptions.performance_profile` (Auto/Handheld/
  Desktop/Off), CLI `--performance-profile`, startup log line listing the
  resolved mode and detected facts, `Config::IsHandheldProfile()`,
  `Config::GetRamTier()`, ... accessors for later phases.

Verified on this host: `Performance profile: mode=Handheld detected=[apu=1
handheld=1 ram=2] threads=12` (Tier16GB from 11 GiB RAM). Default remains
`Auto`, so existing scripts behave the same but now adapt on AMD APUs.

## 8. Phase 3 — Shader/pipeline performance (implemented subset)

- Rect-list fixed-function TCS/TES shaders are now compiled and cached by
  emitted SPIR-V bytecode (`GetOrCreateRectListModules`, shaders.cpp)
  instead of recompiling both modules on every rect-list pipeline build.
- Removed the per-compile O(#programs) full-map walk and unconditional
  `std::printf` shader counter (pipelineCache.cpp), which ran on every
  new shader and was quadratic + spammy.
- Shipped configuration corrected: `shader_validation_enabled=false` in
  Kyty.ini and `--shader-validation false` in kyty_run.sh (validation
  was enabled by default in the bundled runtime profile, forcing
  `spvtools` validation + two `SpirvTools` instances per new shader).
- Enabled the on-disk `vk::PipelineCache` blob for this build by
  committing the source tree (previously "Vulkan pipeline cache:
  disabled (unknown git revision)") — saves `_PipelineCache/<TITLE_ID>.bin`
  on clean exit, which bypasses most driver pipeline compiles on
  subsequent launches.

## 9. Decrypted PS5UPDATE.PUP installer scaffolding

- Integrated an in-tree PS5 PUP container unpacker (zecoxao's
  `ps5-pup-unpacker`, C++17, standalone CMake) under
  `src/loader/PUP.cpp`/`PUP.h`/`BinaryReader.h`/`Entry.h`/`BlockInfo.h`.
- `--install-pup <file> [--pup-output <dir>]` validates the container
  magic and entry table, then unpacks into a structured tree rooted at
  the output dir (`eula.xml`, `kernel.bin`, `tables/*_for_*.img`,
  `dev/ssd0.system*` for the pieces like preinst, etc.).
- Replaces exceptions with `TryLoad`/`return false` surface for the
  project's -fno-exceptions config; raw/encrypted shipping PUPs and
  ote-Exception failures produce a clean "not a valid/decrypted PS5 PUP"
  error instead of terminating.
- **It does not boot the Horizon Dashboard.** KytyPS5 remains a game
  compatibility layer—the real PS5 OS/Dashboard/kernel is not emulated.
  This is scaffolding to let us wire up a real game content+archive path
  later; users need a decrypted `PS5UPDATE.PUP.dec` from their own
  PS5 before unpacking works.

Smoke-tested: the bootable title still runs on AMD Ryzen Z1
(RADV PHOENIX2), 60 s run, no regressions from the rect-list path.

## 10. Boot-crash debug (2026-10-08, fixed)

Symptom: fork build crashed booting PPSA02876 (Smurfs) with
`Unhandled host exception: ... pc=0x... address=0x0` after ~10 shader
compiles, while the upstream prebuilt passed 345+ shaders in the same
window. kyty.log staleness initially masked which binary was running.

Diagnosis: added temporary `dladdr()` crash-module logging to
`KytyExceptionHandler` (kept — crash path only, no perf impact). It
reported `module: /usr/lib/libvulkan_radeon.so + 0x1dd66e`, i.e. a
null-deref inside the RADV driver, not emulator code — the fork was
feeding the driver a dead `VkShaderModule`.

Root cause: the Phase-3 rect-list TCS/TES module cache. Upstream
`CreatePipelineInternal` (shaders.cpp) destroys the rect-list modules
right after `vkCreateGraphicsPipelines` (legal — the pipeline retains
compiled state). The global cache then returned those destroyed handles
on the next rect-list pipeline build → driver use-after-free. Any
module cache must be scoped to device lifetime AND exempt cached
modules from the post-build destroy; a process-global cache does
neither.

Fix: reverted to upstream per-build compile+destroy for rect-list
shaders. Verified with a 60 s Smurfs boot: 0 host exceptions, 295
shader phases, clean `Event: quit` exit. The LRU program-cache bound
was confirmed innocent for this crash (176 programs < 512 cap, eviction
never fired) and stays in place.

## 11. Emulated display resolution (2026-10-08, measured win)

Finding: the emulator hard-reported a 4K display to every game
(`VideoOutGetOutputStatus` -> 2u), so UE4 Smurfs sized its backbuffer at
3840x2160 (8.3M px/frame) even in a 1280x720 window. FPS scaled linearly
with GPU clocks (10 @ 1.36GHz -> 21 @ 2.3GHz): purely pixel-bound.

Fix (no render-target hacking, no quality lies): report the display the
emulator actually has. New `--display-resolution <Auto|HD|FullHD|UltraHD>`
(default Auto = legacy 4K report; under the handheld profile with a
sub-4K window, Auto reports HD — exactly what a real PS5 on a 720p/1080p
TV reports, and games adapt natively).

Measured on Smurfs Vileaf (Ryzen Z1, 720p window):
guest frame 3840x2160 -> 1920x1080; boot/menu 16 -> 40-60 FPS
(vblank-capped); gameplay ~10 -> ~20 FPS at matched clocks (~2x).
`RADV_PERFTEST=ps_wave32,ge_wave32,cs_wave32` tested separately: no
measurable gain, not adopted.
