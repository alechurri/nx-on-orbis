# Technical findings

What was learned porting a large C++20 emulator to the PS4 with OpenOrbis and the orbis-ports
SDK. Measured on a PS4 Pro, firmware 12.02, GoldHEN. Several items apply to any PS4 homebrew.

## Package layout and memory

- **Signing/layout decides how much memory the process gets.** Same ELF, three layouts:
  - orbis-ports' default (paid `0x3800000000000011`, default authinfo, no `sce_module/`): does not
    start on this console.
  - Piglet-style (paid `0x3800000000000035` + Piglet authinfo): starts, but only **768 MiB** of
    direct memory, not enough for the GPU driver.
  - **OpenOrbis samples' layout** (paid `0x3800000000000011`, default authinfo, **with**
    `sce_module/libc.prx` + `libSceFios2.prx` and `sce_sys/about/right.sprx`, SFO category `gd`):
    starts with **~4.4 GiB** of direct memory and the CPU at ~2.1 GHz. `package-eden.sh` uses it.
- Flexible memory (anonymous `mmap`) is only ~384 MiB. Large things must come from direct memory.
- **Guest RAM is committed on demand.** `src/common/orbis_lazy_memory.*` reserves address ranges;
  the first touch of each 64 KiB chunk faults, a handler installed with
  `sceKernelInstallExceptionHandler` maps a zeroed chunk of direct memory there and the access
  retries (~5 µs per fault). Used for the 4 GiB guest RAM and Eden's big sparse tables.
- 64 KiB chunks taken first-fit from the bottom scatter over all of physical memory and break up
  the large blocks other allocators need (the C heap takes 128 MiB carve-outs). v0.1.0 takes them
  from the top 2 GiB first.
- `sceKernelAvailableDirectMemorySize` returns the **largest free block**, not the total.
  `sceKernelGetDirectMemoryType` can be walked over the whole range to get a real map (experimental
  branch, `LogDirectMemoryMap`).
- **Address space is limited.** eden-probe: the largest single `sceKernelReserveVirtualRange` was
  16 GiB (64 GiB fails with `0x8002000c`). In the emulator, 512 GiB was refused; a view that did fit
  left no room for the guest page table reserved after it. 8 GiB is what fits next to everything else.

## C/C++ runtime

- **The SDK's wide-character functions are 16-bit, the compiler's `wchar_t` is 32-bit.** The SDK's
  static `wmemchr` compares 2-byte units (`cmpw`, step 2). libc++ lowers `std::find` /
  `std::ranges::find` over any 4-byte trivially comparable type (`u32`, ids...) to
  `__builtin_wmemchr`, i.e. that `wmemchr`. Result: searches that only find a value if it is the
  first element. In Eden it corrupted the texture cache (dozens of crashes over tests 7-19).
  `frontend/libc_wide_fixes.cpp` defines 4-byte `wmemchr`, `wmemcmp`, `wmemcpy`, `wmemmove`,
  `wmemset`, `wcslen`, `wcscmp`, `wcsncmp`, `wcschr` in the executable (built with `-fno-builtin`).
  **Any PS4 homebrew using a newer libc++ with this SDK is likely affected.**
- Eden's static TLS block (9 KiB) ended up laid out differently from what lld computed (reads of a
  neighbouring field); `-femulated-tls` for the whole program plus compiler-rt's `emutls.c` fixed it.
- libc++ 18 had to be built for the PS4 (see `BUILDING.md`).
- Never log (or take any lock) from a signal handler. The sampling profiler's late signals once
  logged from a thread that already held the log mutex and froze the whole emulator.
- One unexplained heap overwrite was seen (test 28): the small heap block musl keeps the environment
  in held log text, and the GPU driver crashed in `getenv`. Not found; the experimental branch moves
  the environment to static storage and watches the old block.

## The GPU driver (Mesa RADV for "Liverpool", from orbis-ports)

- Vulkan 1.3, 197 extensions; of what Eden uses only `VK_EXT_index_type_uint8` is missing. No
  float16, no ASTC/ETC2 (Eden decodes ASTC; on the CPU here, the GPU compute path and the async CPU
  path both crashed).
- `VK_EXT_conditional_rendering` and `VK_KHR_draw_indirect_count` caused GPU hangs on GFX7; v0.1.0
  removes them before creating the device and uses the macro fallback for indirect-count draws.
- **Each submission is flattened into a 524288-dword (2 MiB) staging buffer**; a bigger one hangs
  the GPU (`mesa.log`: "submission does not fit"). v0.1.0 submits every 256 draws and after 96
  uploads.
- **Memory**: the driver takes one ONION arena (`ORBIS_ARENA_MIB`, default 1024; v0.1.0 sets 1280,
  1024 ran out loading a race). But images and other device-local buffers go to **GARLIC memory
  allocated separately, outside the arena**, and mapped into the arena's address window, so they
  cost direct memory on top of it (hundreds of MiB while a race loads). `ORBIS_VRAM_GARLIC=0` keeps
  everything in the arena; the driver warns it costs GPU speed. Not tried.
- Its log: set `MESA_LOG_FILE`, `MESA_LOG_LEVEL=info`, `RADV_DEBUG=info` (the frontend does). Its
  BUDGET lines say how many cores are busy and how long the CPU waited for the GPU.
- Self-tests that all came out correct on the console (so not causes of the color bug): B8G8R8A8
  sampling and blits, all 24 component mappings (compute), R8G8, A2B10G10R10 sampling and rendering.
  The color bug affects the whole image (3D, videos, UI); the presentation path (swapchain format
  vs the format `wsi/orbis` registers for the video output, zero-copy scan-out) was never tested.

## JIT (dynarmic)

- Code buffers: direct memory mapped RW, then `sceKernelMprotect` to RWX (the only order allowed).
  32 MiB per emulated core.
- Faults are delivered through the kernel exception handler, not `sigaction`. The handler gets a
  ucontext whose FreeBSD mcontext starts at offset 64 (`rip` +160, `rsp` +184, fault address +136);
  modifying `rip`/`rsp` and returning resumes there (verified by the experimental branch's fastmem
  self-test, which performs dynarmic's "fake call").
- The same direct memory can be mapped at two addresses (aliasing works), and `MAP_FIXED` mappings
  inside a reservation work: what fastmem needs.
- Fastmem (experimental branch): 16 KiB host pages against 4 KiB guest pages means a host page can
  only alias the backing when its four guest pages map consecutive backing with the same rights;
  everything else is left unmapped and the faulting instruction is moved to the slow path.

## Threads, timing, input, audio

- `pthread` mutexes behave (startup check: 3 threads x 400000 increments, exact).
- Threads get 2 MiB stacks and 64 KiB alternate signal stacks so a stack overflow can still report.
- Eden's fibers: 512 KiB stacks (128 KiB rewind) instead of 4 MiB; 94 fibers at 4 MiB took 372 MiB.
- Audio: `sceAudioOut` sink (`src/audio_core/sink/ps4_sink.*`), main port, 256 frames, 48 kHz stereo.
- Video out: the game picker draws directly with `sceVideoOut` and keeps its 16 MiB of framebuffers
  after closing the display (freeing them rebooted the console when a game started).

## Diagnostics built in (`frontend/ps4_platform.cpp`)

- `boot.log` written unbuffered from the first static constructor; crash handler with registers and
  a frame-pointer backtrace as `eboot+0x...` offsets; hang watchdog that dumps every thread's stack
  (signals raised with `sceKernelRaiseException`); a sampling profiler (per-thread busy share, JIT
  share and hottest code buckets every 30 s); heap census of allocations of 1 MiB or more.
- Startup checks: TLS, mutexes, `std::find` over 4-byte types, boost `flat_map`.
