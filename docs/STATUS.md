# Status, expectations and projected performance

Written when the project was closed (2026-10-07). Everything measured comes from `boot.log` /
`mesa.log` captured on one PS4 Pro (firmware 12.02, GoldHEN), with Mario Kart 8 Deluxe (base game,
NSP v0) as the main test title.

**Why it stopped:** the author ran out of time to keep testing (every build has to be installed and
played on the console by hand; 29 test builds went through that loop in five days). The work is
published as-is so anyone can continue it.

## What the project set out to do

1. Get Eden (Switch emulator) to build and run as a native PS4 application, reusing what the PS5
   port ProsperoEden had already solved.
2. Boot a commercial game. Mario Kart 8 Deluxe was the target.
3. Get into a race. ("I need the next build to run the race, even if the color is wrong.")
4. Then speed first, colors second.

The hope was "playable". The honest expectation, given the CPU (see below), was always that a
heavy 3D title like MK8D would run well below full speed.

## How far it was tested

- **Hardware:** one PS4 Pro, firmware 12.02, GoldHEN. Never tested on a base PS4, a PS4 Slim, or
  other firmwares.
- **Software:** Mario Kart 8 Deluxe (base game, NSP v0, no update) as the main title; Cuphead up to
  its menu and the start of a game; the bundled Homebrew Menu. No other games were tried.
- **Mode:** handheld (720p internal), the default. Docked mode, higher resolutions, local
  multiplayer, online, updates/DLC and saves across many sessions were not tested.
- **Longest session:** about 15 minutes with MK8D (menus plus two-lap races) without crashing
  (test 21). v0.1.0 was played the same way (tests 24 and the test 22 re-run).
- **Not verified on the console:** v0.1.0 itself was rebuilt from the test 24 sources for the
  release; the code is the same (identical function sizes), only version strings differ.

## Expectations vs. results

| Expectation | Result |
|---|---|
| Eden builds and runs as a native PS4 app | **Yes** (test 2) |
| A commercial game boots | **Yes**: MK8D title screen and menus at 40-60 fps; Cuphead menu at ~30 fps |
| MK8D gets into a race | **Yes** (test 21): races play, sound is clean |
| Playable speed | **No**: 13-16 fps in races (60 is the game's target) |
| Correct colors | **No**: red/blue swapped on 3D models and videos; UI correct |
| Stable over a session | **Mostly**: ~15 min sessions; occasional hang/crash while a race loads (memory) |
| Faster with fastmem | **Unknown**: works, but races stopped loading on that branch (memory) |

## Every test on the console

Each "test" is a package installed and run on the console, with `boot.log` / `mesa.log` /
`eden_log.txt` sent back. Details of each one are in `dev-log-es.md` (Spanish).

| Test | What changed | Result on the console |
|---|---|---|
| probes 1-3 | eden-probe: package layouts, memory limits, JIT, double mapping, faults, GPU | Found the layout that gets 4.4 GiB and 2.1 GHz; JIT, aliasing and fault redirect work; largest address reservation 16 GiB |
| 1 | First full build (Homebrew Menu) | Starts, opens the controller, crashes loading: static TLS laid out differently than the linker computed |
| 2 | Emulated TLS, JIT in direct memory | **Eden runs**: Homebrew Menu at full CPU speed, GPU 26-29 fps. MK8D reaches the title screen; Cuphead runs but memory grows until it runs out |
| 3 | Thread names for hang dumps | Diagnosis of the stalls (GPU thread waiting, CPU in memory callbacks) |
| 4 | Memory fixes | Cuphead menu in 26 s (was 99) at 31-33 fps; out of memory when a game starts (Eden heap 1.18 GiB) |
| 5 | Heap census in the log | Found it: 372 MiB of fiber stacks, big JIT tables, descriptor queues |
| 6 | 512 KiB fiber stacks, 32 MiB JIT caches, game selector | MK8D menus at 40-60 fps; **colors red/blue swapped** first seen; races: GPU device lost |
| 7 | Remove conditional rendering and indirect-count draws (GPU hangs) | Crash in the texture cache (LRU) at 108 s |
| 8 | Game picker menu, BGRA self-test | Menu works; crash in the self-test (used the scheduler too early); console rebooted opening Cuphead |
| 9 | Self-test on its own command buffer; menu keeps its framebuffers | BGRA sampling correct (not the color cause); race: device lost |
| 10 | Async CPU ASTC, driver log (`mesa.log`) | Crash: upload into an already destroyed image (async ASTC) |
| 11 | Synchronous CPU ASTC, 24-permutation component-mapping self-test | All 24 mappings correct (not the color cause); crash creating pipelines at startup (self-test side effect) |
| 12 | Self-test off by default, VIC frame dump | `mesa.log` shows the cause of the device lost: submissions larger than the driver's 2 MiB buffer |
| 13 | Submit every 256 draws / 96 uploads | No more GPU hangs; crash in the texture cache at 237 s |
| 14 | Retirement fences, crash breadcrumbs (Codex) | Race does not load: crash beginning a render pass |
| 15 | Page ownership fixes (Codex) | Crash creating a view of a deleted image |
| 16 | R8G8 self-test, texture-cache traces | R8G8 not the color cause; same cache crash |
| 17 | Mutex check, per-image history | Mutexes correct; cache entries vanish without being removed |
| 18 | A2B10G10R10 self-test | Correct in every path (not the color cause); same cache crash |
| 19 | `flat_map` check, re-read every page-table entry after insert | Found it: a value pushed into a vector is "not found" right after unless it is the first element |
| 20 | 4-byte `wmemchr` & co. in the executable (the SDK's are 16-bit) | **Texture cache fixed** (zero inconsistencies); race load: GPU out of memory |
| 21 | GPU arena 1280 MiB, cache budgets, mid-frame collection | **Races play**: two laps, ~15 min, 10-27 fps |
| 22 | `-march=btver2`, Android GPU defaults, sampling profiler | First run hung after the menu (profiler deadlock); a re-run played races. Profile: CPU-bound in JIT code |
| 23 | Profiler never logs from its signal handler | Not run on the console (superseded by 24) |
| 24 | `cpu_accuracy=unsafe`, guest memory from the top of physical memory | **Races at 13-16 fps, stable**: this is v0.1.0 |
| 25 | Fastmem, direct-memory map | Self-test passed, but the 512 GiB view was refused: fastmem not active; same speed |
| 26 | Smaller views | Crash loading the game: the view starved the address space |
| 27 | View capped (8 GiB fits) | **Fastmem active**; ran out of direct memory loading a race (hang) |
| 28 | GPU arena 1152 MiB, redirect causes | Crash at startup: heap block holding the environment overwritten |
| 29 | Environment out of the heap | Out of direct memory loading a race (crash in the driver). Last test |

## Where the time goes (race, v0.1.0)

From the built-in sampling profiler (every 4 ms per thread, reported every 30 s) and the driver's
own budget lines:

- Frame time 60-70 ms (13-16 fps). The game itself reports 100% speed (it drops frames, it does not
  slow down).
- Emulated CPU core 0: **76-84% busy**, and **70-87% of that is JIT-generated code** (the game's
  own ARM code, translated). Core 1: 50-60%, core 2: 40-50%, core 3: idle.
- GPU thread ~30% busy; Vulkan worker ~10-14%.
- The driver reports **0 ms waited for the GPU** in every 5-second window: the GPU is not the limit.
- 3.2-3.9 of the ~7 usable cores are busy on average.
- Note: MK8D is a 32-bit (AArch32) game; the A32 translator is what runs.

In short: the game's main thread, translated from ARM to x86 and running on a 2.1 GHz Jaguar core,
cannot produce frames faster than this.

## Projected maximum performance

These are estimates, not measurements.

- **Why the ceiling is low.** The Switch runs this code natively on four Cortex-A57 cores at
  1.02 GHz. Here every guest instruction goes through a dynamic recompiler, whose output typically
  costs several host instructions per guest instruction, on a Jaguar core (2013, low-power, two-wide
  decode) at 2.13 GHz. The PS4 Pro has roughly a third of the per-core speed of the PS5's Zen 2,
  which is what ProsperoEden targets.
- **What is left to gain**, roughly:
  - Fastmem (guest memory access in one instruction instead of a page-table walk): +10-30% on the
    JIT-bound threads, if the redirect storm is fixed (see below).
  - Fewer stutters (shader compilation is already asynchronous and cached on disk; first runs
    stutter more).
  - Smaller items (JIT cache sizing, thread placement): a few percent each.
- **Projection for MK8D races on a PS4 Pro: about 20-25 fps at best**, with stutters. A stable
  30 fps is unlikely; 60 fps is not reachable on this CPU. A base PS4 (1.6 GHz) would be roughly a
  quarter slower again.
- **Lighter games** (2D, indie, games that are not CPU-heavy on the Switch) are where this port has
  a realistic chance of running at full speed. That was never tested systematically.

## Memory budget (race)

The process gets 4608 MiB of direct memory. In a race (v0.1.0): guest RAM committed on demand
~1.5 GiB, GPU driver arena 1280 MiB, C heap 768-896 MiB (128 MiB carve-outs), JIT code caches
4 x 32 MiB, and the driver's GARLIC allocations for images, which live **outside** the arena and
grow by several hundred MiB while a race loads. About 100 MiB stay free. See `TECHNICAL.md`.

## The experimental branch (tests 25-29)

- **Fastmem works**: an 8 GiB view (512 GiB was refused, 0x8002000c; a 256 GiB one starved the
  address space so the guest page table could not be reserved) whose 16 KiB pages alias the
  lazily committed guest RAM and are mapped on first touch; JIT faults go to dynarmic's slow path
  through a PS4 exception handler. A startup self-test (aliasing, `mprotect`, rip/rsp redirect)
  gates it; `fastmem=off` in `settings.txt` disables it.
- **But**: ~370,000 slow-path redirects per session, almost all "unmapped" (a 16 KiB host page
  whose four 4 KiB guest pages are not all mapped to consecutive backing). Each one recompiles a
  block. Understanding why (guest physical memory fragmented at 4 KiB? mappings that never reach
  the view?) is the first thing to do there.
- **And** fastmem's bookkeeping costs ~128 MiB more C heap, which was enough to run direct memory
  out while loading a race (tests 27 and 29), even with the GPU arena reduced to 1152 MiB.
- Ideas not tried: Eden's stream buffer from 256 to 128 MiB on the PS4 (it is a 256 MiB GARLIC
  allocation outside the arena); lower texture-cache budgets; `env=ORBIS_VRAM_GARLIC=0` (keeps
  images inside the arena, which removes the double cost, but the driver warns it costs GPU speed).
- Test 28 crashed in `getenv` inside the driver because the heap block holding the environment had
  been overwritten with log text. The branch moves the environment to static storage and logs
  `!! HEAP CORRUPTION` if the old block changes. The writer was not found.

## Colors: what was ruled out

Red and blue are swapped on characters, karts and the game's videos (decoded YUV converted by the
game's own shader into A2B10G10R10 render targets); the UI is correct. Tested and correct on the
console, so not the cause: B8G8R8A8 sampling and blits, all 24 component-mapping permutations
(compute self-test), R8G8 sampling, A2B10G10R10 sampling and rendering (both orders), ASTC decode
on the CPU, the swapchain. The VIC (video decoder) output frame was dumped and is correct on a PC.
Still open: Eden's shader translation for this GPU's feature set (no float16, etc.) or something in
how the game's YUV->RGB shader is translated.

## If you want to continue

1. Read `TECHNICAL.md` first, then the Spanish log (`dev-log-es.md`) for the details of each test.
2. Logs: `/data/edenps4/boot.log` (startup checks, status every 10 s, profiler every 30 s, crash
   reports with registers and an offset backtrace), `/data/edenps4/mesa.log` (the driver),
   `/data/edenps4/log/eden_log.txt` (Eden). Symbolize `eboot+0x...` offsets with the ELF from the
   release: `llvm-symbolizer --obj=nx-on-orbis-v0.1.0.elf -C -f 0x<offset>`.
3. Most promising for speed: the fastmem redirect storm, then memory headroom so races load with
   fastmem on.
4. Most promising for colors: compare Eden's SPIR-V for MK8D's 3D and video shaders between a PC
   and this GPU profile.
