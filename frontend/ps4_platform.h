// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: the console-facing pieces of the frontend (boot log, crash reports, system modules,
// the DualShock 4). Kept apart from Eden's headers so the SDK's own headers stay in one file.

#pragma once

#include <cstdint>
#include <string>

namespace Ps4 {

/// Every directory the frontend uses, created on start.
inline constexpr const char* DataDir = "/data/edenps4";

/// Opens /data/edenps4/boot.log (the previous one becomes boot.old.log). Runs from a static
/// constructor before main; calling it again does nothing.
void OpenBootLog();
/// Starts the frame watchdog thread (see ArmHangWatch).
void StartWatchdog();
/// The sampling profiler (on by default): busy share and hottest code of the GPU, Vulkan and
/// CPU-core threads every 30 s in boot.log.
void SetProfiling(bool enabled);
/// One line in boot.log, flushed and synced before returning (a crash right after keeps it).
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/// Lazy-memory fault handler + crash report for every other fault, and the orbis-compat log
/// (RADV, the overlay) into boot.log.
void InstallCrashReporting();
/// Names the current phase for crash and hang reports.
void SetPhase(const char* phase);
/// The watchdog: if the frame counter stops for `seconds` while a game runs, the main thread's
/// stack is dumped to boot.log (no restart). 0 disables it.
void ArmHangWatch(unsigned seconds);
/// Records the calling thread under `name` for hang stack dumps.
void RegisterThread(const char* name);
/// Called by the presentation path for every frame shown.
void NoteFrame();

/// System modules the SDK does not load by itself (pad, user service, audio out...).
void LoadSystemModules();
void HideSplashScreen();

struct PadState {
    bool connected{};
    std::uint32_t buttons{};
    std::uint8_t lx{128}, ly{128}, rx{128}, ry{128};
    std::uint8_t l2{}, r2{};
};

/// Opens the first user's controller. False if there is none.
bool OpenPad();
PadState ReadPad();
void SetPadLight(std::uint8_t r, std::uint8_t g, std::uint8_t b);

// DualShock 4 button bits (scePad).
namespace Button {
inline constexpr std::uint32_t L3 = 0x0002;
inline constexpr std::uint32_t R3 = 0x0004;
inline constexpr std::uint32_t Options = 0x0008;
inline constexpr std::uint32_t Up = 0x0010;
inline constexpr std::uint32_t Right = 0x0020;
inline constexpr std::uint32_t Down = 0x0040;
inline constexpr std::uint32_t Left = 0x0080;
inline constexpr std::uint32_t L2 = 0x0100;
inline constexpr std::uint32_t R2 = 0x0200;
inline constexpr std::uint32_t L1 = 0x0400;
inline constexpr std::uint32_t R1 = 0x0800;
inline constexpr std::uint32_t Triangle = 0x1000;
inline constexpr std::uint32_t Circle = 0x2000;
inline constexpr std::uint32_t Cross = 0x4000;
inline constexpr std::uint32_t Square = 0x8000;
inline constexpr std::uint32_t TouchPad = 0x100000;
} // namespace Button

/// Monotonic microseconds.
std::uint64_t NowUs();

/// Direct and flexible memory still free, for the periodic status line.
std::uint64_t FreeDirectMemory();

/// Walks the whole of direct memory and logs what is allocated and free (total, not just the
/// largest block), plus every allocation of 2 MiB or more that appeared since the last walk.
void LogDirectMemoryMap();

} // namespace Ps4
