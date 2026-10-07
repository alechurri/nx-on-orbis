// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: a minimal PS4 frontend for the Eden emulator core.
//
// Personal port, not affiliated with the Eden project. The boot sequence follows Eden's own
// command-line frontend (src/yuzu_cmd/yuzu.cpp) and ProsperoEden's PS5 frontend.
//
// Files, all under /data/edenps4:
//   keys/prod.keys (+ title.keys)     the console's keys, dumped from the user's Switch
//   firmware/*.nca                    the firmware, dumped from the user's Switch (copied into
//                                     nand/system/Contents/registered the first time)
//   roms/*.nsp|*.xci                  listed in a menu at start (Up/Down, Cross), the last one
//                                     played preselected, unless game.txt names one
//                                     (with neither, the bundled Homebrew Menu starts)
//   game.txt                          optional: the full path of the game to start
//   updates/                          update and DLC NSPs, applied when the game starts
//   nomenu.txt                        optional: skip the game menu, start the last game played
//   settings.txt                      optional, one "key=value" per line, to try graphics
//                                     options without a new package (see ApplySettingsFile)
//   boot.log                         this frontend's log; log/eden_log.txt is Eden's
//
// Controls (Switch layout by position): Circle=A Cross=B Triangle=X Square=Y, L1/R1=L/R,
// L2/R2=ZL/ZR, Options=+, touch pad click=-, L3/R3 sticks. Options + touch pad for 2 s quits.

#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/orbis_lazy_memory.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "core/perf_stats.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

#include "menu/rom_menu.h"
#include "ps4_platform.h"

namespace fs = std::filesystem;

extern "C" void Ps4HeapCensus(); // heap_census.cpp

namespace Vulkan {
extern int ps4_bgra_mode;    // vk_texture_cache.cpp
extern int ps4_swizzle_mode; // vk_texture_cache.cpp
extern int ps4_rg_mode;      // vk_texture_cache.cpp
extern int ps4_a2b10_mode;   // vk_texture_cache.cpp
}

namespace {

constexpr u32 OutputWidth = 1920;
constexpr u32 OutputHeight = 1080;

class DummyContext final : public Core::Frontend::GraphicsContext {};

/// The TV: RADV's headless surface, whose presents become video-out flips.
class Ps4Window final : public Core::Frontend::EmuWindow {
public:
    Ps4Window() {
        window_info.type = Core::Frontend::WindowSystemType::Orbis;
        // Non-null so the renderer presents instead of running headless.
        window_info.render_surface = reinterpret_cast<void*>(1);
        window_info.render_surface_scale = 1.0f;
        UpdateCurrentFramebufferLayout(OutputWidth, OutputHeight);
    }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<DummyContext>();
    }
    bool IsShown() const override {
        return true;
    }
    void OnFrameDisplayed() override {
        Ps4::NoteFrame();
    }
};

bool HasExtension(const fs::path& p, std::initializer_list<const char*> exts) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return std::any_of(exts.begin(), exts.end(), [&](const char* x) { return e == x; });
}

std::string FindGame() {
    const fs::path data{Ps4::DataDir};
    std::error_code ec;
    if (std::ifstream in{data / "game.txt"}; in) {
        std::string line;
        std::getline(in, line);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty() && fs::is_regular_file(line, ec)) {
            return line;
        }
        Ps4::Log("game.txt names '%s', which is not a file", line.c_str());
    }
    std::vector<fs::path> games;
    for (fs::directory_iterator it{data / "roms", ec}, end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && HasExtension(it->path(), {".nsp", ".xci", ".nro", ".nso", ".nca"})) {
            games.push_back(it->path());
        }
    }
    std::sort(games.begin(), games.end());
    if (!games.empty()) {
        // The last game played is preselected in the menu (menu/rom_menu.cpp).
        const fs::path last_file = data / "last_game.txt";
        size_t index = 0;
        if (std::ifstream in{last_file}; in) {
            std::string last;
            std::getline(in, last);
            for (size_t i = 0; i < games.size(); ++i) {
                if (games[i].filename().string() == last) {
                    index = i;
                }
            }
        }
        std::vector<std::string> names;
        for (const auto& game : games) {
            names.push_back(game.filename().string());
        }
        Ps4::SetPadLight(255, 255, 255);
        // nomenu.txt skips the menu (in case Eden cannot open the display after it).
        if (const int chosen = fs::exists(data / "nomenu.txt", ec) ? -1 : RunRomMenu(names, int(index));
            chosen >= 0) {
            index = size_t(chosen);
        }
        if (std::ofstream out{last_file}; out) {
            out << games[index].filename().string() << '\n';
        }
        Ps4::Log("game %zu of %zu in roms/", index + 1, games.size());
        return games[index].string();
    }
    // Nothing of the user's: the Homebrew Menu (switchbrew/nx-hbmenu, ISC) that ships in the
    // package, which needs no keys or firmware - a first test of CPU, GPU and presentation.
    const fs::path bundled{"/app0/assets/misc/hbmenu.nro"};
    if (fs::is_regular_file(bundled, ec)) {
        Ps4::Log("no game of yours found; starting the bundled Homebrew Menu");
        return bundled.string();
    }
    return {};
}

/// The firmware is read from nand/system/Contents/registered. A dump in firmware/ is copied there
/// the first time (and again if a file is missing or has another size).
void InstallFirmware() {
    const fs::path source = fs::path{Ps4::DataDir} / "firmware";
    const fs::path target = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "system/Contents/registered";
    std::error_code ec;
    if (!fs::is_directory(source, ec)) {
        Ps4::Log("no firmware/ folder; games that need the firmware will not start");
        return;
    }
    fs::create_directories(target, ec);
    size_t copied = 0, present = 0, failed = 0;
    for (fs::directory_iterator it{source, ec}, end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !HasExtension(it->path(), {".nca"})) {
            continue;
        }
        const fs::path to = target / it->path().filename();
        std::error_code e2;
        if (fs::exists(to, e2) && fs::file_size(to, e2) == it->file_size(e2)) {
            ++present;
            continue;
        }
        if (fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing, e2)) {
            ++copied;
        } else {
            ++failed;
        }
    }
    Ps4::Log("firmware: %zu NCAs installed now, %zu already there, %zu failed", copied, present, failed);
}

void ApplyPs4Settings() {
    auto& v = Settings::values;
    v.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    v.use_multi_core.SetValue(true);
    // Speed (test 24): in races 70-87% of the emulated cores' busy time is the JIT's own code.
    // Unsafe adds dynarmic's inaccurate-NaN and reduced-error FP on top of Auto's unfused FMA
    // (the Jaguar has no FMA); what most phones run MK8D with. settings.txt: cpu_accuracy=auto.
    v.cpu_accuracy.SetValue(Settings::CpuAccuracy::Unsafe);
    // Fastmem (test 25): the JIT reaches guest memory with one instruction instead of a page-table
    // walk. Needs the startup self-test (FastmemSelfTest); settings.txt: fastmem=off.
    v.cpuopt_unsafe_host_mmu.SetValue(true);
    // Handheld renders at 720p: less GPU work for a GPU that is much smaller than the PS5's.
    v.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
    v.resolution_setup.SetValue(Settings::ResolutionSetup::Res1X);
    v.use_disk_shader_cache.SetValue(true);
    v.use_asynchronous_shaders.SetValue(true);
    v.use_speed_limit.SetValue(true);
    v.sink_id.SetValue(Settings::AudioEngine::Auto);
    v.memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_4Gb);
    v.vram_usage_mode.SetValue(Settings::VramUsageMode::Conservative);
    v.renderer_debug = false;
    // The GPU macro JIT takes its code buffer from an anonymous mmap (flexible memory, not known
    // to take PROT_EXEC on the PS4); the macro interpreter needs nothing special.
    v.disable_macro_jit.SetValue(true);
    // MK8D lost the GPU twice while loading a race, both times with ASTC decoded by Eden's compute
    // pass (a large compute shader; compute is the least exercised path of the PS4 driver). On the
    // CPU, synchronously: the asynchronous decode uploaded into images already deleted (crashes in
    // tests 7 and 10, radv_image_queue_family_mask from Image::UploadMemory).
    v.accelerate_astc.SetValue(Settings::AstcDecodeMode::Cpu);
    // Speed (test 22): races ran at 10-27 fps with the GPU never waited on; the processor is the
    // limit. What Eden itself defaults to on Android phones: low GPU accuracy (fewer syncs and
    // readbacks) and no reactive flushing (no GPU->CPU flushes on guest reads).
    v.gpu_accuracy.SetValue(Settings::GpuAccuracy::Low);
    v.use_reactive_flushing.SetValue(false);
    v.external_content_dirs = {(fs::path{Ps4::DataDir} / "updates").string()};

    // The PS4 driver's own log (why it declared the device lost, among others), read when the
    // Vulkan instance is created. settings.txt can override any of these with env=NAME=VALUE.
    const fs::path mesa_log = fs::path{Ps4::DataDir} / "mesa.log";
    std::error_code ec;
    fs::rename(mesa_log, fs::path{Ps4::DataDir} / "mesa.old.log", ec);
    setenv("MESA_LOG_FILE", mesa_log.string().c_str(), 1);
    setenv("MESA_LOG_LEVEL", "info", 1);
    setenv("RADV_DEBUG", "info", 1);
    // The driver's GPU arena: 1024 MiB by default ran out loading an MK8D race (test 20); there
    // was direct memory to spare. The texture and buffer caches size their budgets from this.
    // Test 28: 1152. The driver puts most images on GARLIC memory allocated OUTSIDE the arena (the
    // direct memory map showed it: ~650 MiB of extra allocations by the race), so the arena mostly
    // gives address space; at 1280 a race load ran direct memory out (test 27, with fastmem's
    // bookkeeping on top). The texture cache budgets, and with them the GARLIC images, follow it.
    setenv("ORBIS_ARENA_MIB", "1152", 1);
}

/// settings.txt: lines "key=value" ('#' starts a comment). Each recognized line is logged; any
/// other line is logged as unknown and ignored.
///   astc=gpu|cpu|async          ASTC texture decoding (default cpu)
///   bgra=auto|on|off            B8G8R8A8 red/blue workaround (default auto: startup self-test)
///   a2b10=auto|off              A2B10G10R10 workaround from the startup self-test (default auto)
///   rg=auto|on|off              R8G8 red/green workaround (default auto: startup self-test)
///   swizzle=test|off            run the component-mapping self-test at startup (default off)
///   dyna_state=0..3             Vulkan extended dynamic state level (default 2)
///   vertex_input_dynamic=on|off (default on)
///   gpu_accuracy=low|high       (default low)
///   reactive_flushing=on|off    (default off)
///   profile=on|off              sampling profiler in boot.log (default on)
///   cpu_accuracy=unsafe|auto|accurate (default unsafe)
///   fastmem=on|off              (default on; needs cpu_accuracy=unsafe)
///   async_shaders=on|off        (default on)
///   env=NAME=VALUE              environment for the PS4 driver (e.g. env=RADV_DEBUG=info,nohiz)
// The environment, moved out of the heap once every setenv is done (test 29). Test 28 crashed in
// getenv inside the GPU driver (it reads the environment on every buffer it creates): the heap
// array musl keeps the variables in held log text ("...er.Vulka...") instead of pointers, i.e.
// something overwrote that small heap block. A static copy cannot be hit that way; the old heap
// array is kept and compared at every status line, to catch whoever writes over it.
char* g_env_static[64];
char g_env_text[8192];
char** g_env_heap = nullptr;
char* g_env_heap_saved[64];
int g_env_count = 0;
bool g_env_reported = false;

void MoveEnvironmentOutOfHeap() {
    char** env = environ;
    std::size_t used = 0;
    int n = 0;
    for (; env != nullptr && env[n] != nullptr && n < 63; ++n) {
        const std::size_t len = std::strlen(env[n]) + 1;
        if (used + len > sizeof(g_env_text)) {
            break;
        }
        std::memcpy(g_env_text + used, env[n], len);
        g_env_static[n] = g_env_text + used;
        g_env_heap_saved[n] = env[n];
        used += len;
    }
    g_env_static[n] = nullptr;
    g_env_heap_saved[n] = nullptr;
    g_env_heap = env;
    g_env_count = n;
    environ = g_env_static;
    Ps4::Log("environment: %d variables moved out of the heap (%lu bytes)", n,
             static_cast<unsigned long>(used));
}

void CheckHeapEnvironment() {
    if (g_env_heap == nullptr || g_env_reported) {
        return;
    }
    for (int i = 0; i <= g_env_count; ++i) {
        if (g_env_heap[i] == g_env_heap_saved[i]) {
            continue;
        }
        g_env_reported = true;
        unsigned char bytes[96];
        std::memcpy(bytes, g_env_heap, sizeof(bytes));
        char line[400];
        int len = 0;
        for (std::size_t b = 0; b < sizeof(bytes); ++b) {
            const unsigned char c = bytes[b];
            line[len++] = c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '.';
        }
        line[len] = 0;
        Ps4::Log("!! HEAP CORRUPTION: the old environment array at %p was overwritten (slot %d: "
                 "%p, was %p). Its first 96 bytes as text: %s",
                 static_cast<void*>(g_env_heap), i, static_cast<void*>(g_env_heap[i]),
                 static_cast<void*>(g_env_heap_saved[i]), line);
        return;
    }
}

void ApplySettingsFile() {
    std::ifstream in{fs::path{Ps4::DataDir} / "settings.txt"};
    if (!in) {
        return;
    }
    auto& v = Settings::values;
    for (std::string line; std::getline(in, line);) {
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        line.erase(std::remove(line.begin(), line.end(), ' '), line.end());
        const auto eq = line.find('=');
        if (line.empty() || eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        bool known = true;
        if (key == "astc" && (value == "gpu" || value == "cpu" || value == "async")) {
            v.accelerate_astc.SetValue(value == "gpu"   ? Settings::AstcDecodeMode::Gpu
                                       : value == "cpu" ? Settings::AstcDecodeMode::Cpu
                                                        : Settings::AstcDecodeMode::CpuAsynchronous);
        } else if (key == "bgra" && (value == "auto" || value == "on" || value == "off")) {
            Vulkan::ps4_bgra_mode = value == "auto" ? 0 : value == "on" ? 1 : 2;
        } else if (key == "cpu_accuracy" &&
                   (value == "unsafe" || value == "auto" || value == "accurate")) {
            v.cpu_accuracy.SetValue(value == "unsafe" ? Settings::CpuAccuracy::Unsafe
                                    : value == "auto" ? Settings::CpuAccuracy::Auto
                                                      : Settings::CpuAccuracy::Accurate);
        } else if (key == "fastmem" && (value == "on" || value == "off")) {
            v.cpuopt_unsafe_host_mmu.SetValue(value == "on");
        } else if (key == "profile" && (value == "on" || value == "off")) {
            Ps4::SetProfiling(value == "on");
        } else if (key == "reactive_flushing" && (value == "on" || value == "off")) {
            v.use_reactive_flushing.SetValue(value == "on");
        } else if (key == "a2b10" && (value == "auto" || value == "off")) {
            Vulkan::ps4_a2b10_mode = value == "auto" ? 0 : 2;
        } else if (key == "rg" && (value == "auto" || value == "on" || value == "off")) {
            Vulkan::ps4_rg_mode = value == "auto" ? 0 : value == "on" ? 1 : 2;
        } else if (key == "swizzle" && (value == "test" || value == "off")) {
            Vulkan::ps4_swizzle_mode = value == "test" ? 0 : 1;
        } else if (key == "dyna_state" && value.size() == 1 && value[0] >= '0' && value[0] <= '3') {
            v.dyna_state.SetValue(static_cast<Settings::ExtendedDynamicState>(value[0] - '0'));
        } else if (key == "vertex_input_dynamic" && (value == "on" || value == "off")) {
            v.vertex_input_dynamic_state.SetValue(value == "on");
        } else if (key == "gpu_accuracy" && (value == "low" || value == "high")) {
            v.gpu_accuracy.SetValue(value == "low" ? Settings::GpuAccuracy::Low
                                                   : Settings::GpuAccuracy::High);
        } else if (key == "async_shaders" && (value == "on" || value == "off")) {
            v.use_asynchronous_shaders.SetValue(value == "on");
        } else if (key == "env" && value.find('=') != std::string::npos && value.find('=') > 0) {
            // A driver switch, e.g. env=ORBIS_GS_RING_SCALE=4 (read when Vulkan starts).
            const auto split = value.find('=');
            setenv(value.substr(0, split).c_str(), value.c_str() + split + 1, 1);
        } else {
            known = false;
        }
        Ps4::Log("settings.txt: %s=%s%s", key.c_str(), value.c_str(), known ? "" : " (unknown, ignored)");
    }
}

/// DualShock 4 -> the virtual gamepad Eden binds to player 1, by position like a Switch pad.
void PollPad(InputCommon::VirtualGamepad& pad, const Ps4::PadState& s) {
    using VB = InputCommon::VirtualGamepad::VirtualButton;
    struct Map {
        u32 bit;
        VB button;
    };
    static constexpr Map map[] = {
        {Ps4::Button::Circle, VB::ButtonA},     {Ps4::Button::Cross, VB::ButtonB},
        {Ps4::Button::Triangle, VB::ButtonX},   {Ps4::Button::Square, VB::ButtonY},
        {Ps4::Button::L1, VB::TriggerL},        {Ps4::Button::R1, VB::TriggerR},
        {Ps4::Button::L2, VB::TriggerZL},       {Ps4::Button::R2, VB::TriggerZR},
        {Ps4::Button::Options, VB::ButtonPlus}, {Ps4::Button::TouchPad, VB::ButtonMinus},
        {Ps4::Button::L3, VB::StickL},          {Ps4::Button::R3, VB::StickR},
        {Ps4::Button::Up, VB::ButtonUp},        {Ps4::Button::Down, VB::ButtonDown},
        {Ps4::Button::Left, VB::ButtonLeft},    {Ps4::Button::Right, VB::ButtonRight},
    };
    for (const auto& m : map) {
        pad.SetButtonState(0, m.button, (s.buttons & m.bit) != 0);
    }
    const auto axis = [](u8 value) { return std::clamp((static_cast<int>(value) - 128) / 127.0f, -1.0f, 1.0f); };
    // Switch sticks: up is positive.
    pad.SetStickPosition(0, InputCommon::VirtualGamepad::VirtualStick::Left, axis(s.lx), -axis(s.ly));
    pad.SetStickPosition(0, InputCommon::VirtualGamepad::VirtualStick::Right, axis(s.rx), -axis(s.ry));
}

/// Thread-local storage check: an initialised thread_local must read its initial value on the
/// main thread and on a new thread, and keep per-thread writes apart (test 1 crashed on exactly
/// this: Eden's KernelCore thread_local read garbage).
struct TlsProbe {
    u64 a = 0x1122334455667788ULL;
    u8 pad[3000]{};
    u64 b = 0xA5A5A5A5A5A5A5A5ULL;
};
thread_local TlsProbe tls_probe;

bool CheckTls() {
    const bool main_ok = tls_probe.a == 0x1122334455667788ULL && tls_probe.b == 0xA5A5A5A5A5A5A5A5ULL;
    tls_probe.a = 1;
    bool worker_ok = false;
    std::thread{[&] {
        worker_ok = tls_probe.a == 0x1122334455667788ULL && tls_probe.b == 0xA5A5A5A5A5A5A5A5ULL;
        tls_probe.a = 2;
    }}.join();
    const bool kept = tls_probe.a == 1;
    Ps4::Log("TLS check: main %s, new thread %s, per-thread values %s", main_ok ? "ok" : "BAD",
             worker_ok ? "ok" : "BAD", kept ? "ok" : "BAD");
    return main_ok && worker_ok && kept;
}

/// Mutual exclusion under contention, for std::mutex and std::recursive_mutex as this libc++ and
/// the console's pthreads build them. The texture cache (guarded by a recursive_mutex) goes
/// inconsistent when the nvservices thread unmaps GPU memory while the GPU thread draws (MK8D).
extern "C" int EdenPs4FlatMapCheck(char* report, int report_size); // tests/flat_map_check.cpp

void CheckMutexes() {
    constexpr int Iterations = 400000;
    const auto run = [](auto& mutex, bool nested) {
        volatile u64 counter = 0;
        const auto worker = [&] {
            for (int i = 0; i < Iterations; ++i) {
                std::lock_guard lock{mutex};
                if constexpr (requires { mutex.try_lock(); }) {
                    if (nested) {
                        // recursive_mutex only: the owner can take it again.
                        std::lock_guard inner{mutex};
                        counter = counter + 1;
                        continue;
                    }
                }
                counter = counter + 1;
            }
        };
        std::thread a{worker};
        std::thread b{worker};
        std::thread c{worker};
        a.join();
        b.join();
        c.join();
        return u64(counter);
    };
    std::mutex plain;
    std::recursive_mutex recursive;
    const u64 plain_count = run(plain, false);
    const u64 recursive_count = run(recursive, true);
    const u64 expected = u64(Iterations) * 3;
    Ps4::Log("mutex check: std::mutex %llu/%llu %s, std::recursive_mutex (nested) %llu/%llu %s",
             static_cast<unsigned long long>(plain_count), static_cast<unsigned long long>(expected),
             plain_count == expected ? "ok" : "BROKEN",
             static_cast<unsigned long long>(recursive_count),
             static_cast<unsigned long long>(expected),
             recursive_count == expected ? "ok" : "BROKEN");
}

} // Anonymous namespace

int main() {
    Ps4::OpenBootLog();
    Ps4::Log("eden-ps4 starting (Eden %s %s)", Common::g_scm_branch, Common::g_scm_desc);
    Ps4::Log("PS4 build: %s; test 29: environment moved out of the heap + heap corruption detector (test 28 crashed in getenv), GPU arena 1152 MiB, fastmem redirects by cause, fastmem view <= 64 GiB, direct memory map, unsafe CPU, lazy memory at the top; defaults CPU ASTC, swizzle test off",
             EDEN_PS4_BUILD_ID);
    Ps4::InstallCrashReporting();
    Ps4::StartWatchdog();
    Ps4::RegisterThread("main");
    Ps4::LoadSystemModules();
    Ps4::HideSplashScreen();
    CheckTls();
    CheckMutexes();
    {
        // std::find over 4-byte elements goes through wmemchr (libc_wide_fixes.cpp).
        const std::vector<u32> values{1, 3, 7, 0x10000, 42};
        const auto at = [&](u32 v) {
            return static_cast<long>(std::find(values.begin(), values.end(), v) - values.begin());
        };
        const bool find_ok = at(1) == 0 && at(3) == 1 && at(7) == 2 && at(0x10000) == 3 &&
                             at(42) == 4 && at(5) == 5;
        Ps4::Log("find check: 1->%ld 3->%ld 7->%ld 0x10000->%ld 42->%ld 5->%ld %s", at(1), at(3),
                 at(7), at(0x10000), at(42), at(5), find_ok ? "ok" : "BROKEN");
    }
    {
        char report[256];
        const int missing = EdenPs4FlatMapCheck(report, sizeof(report));
        Ps4::Log("flat_map check: %s %s", report, missing == 0 ? "ok" : "BROKEN");
    }

    std::error_code ec;
    for (const char* sub : {"keys", "firmware", "roms", "updates"}) {
        fs::create_directories(fs::path{Ps4::DataDir} / sub, ec);
    }

    Ps4::SetPhase("logging");
    Common::Log::Initialize();
    Common::Log::Start();
    LOG_INFO(Frontend, "eden-ps4 frontend");

    const bool pad_ok = Ps4::OpenPad();
    Ps4::Log("controller %s", pad_ok ? "open" : "NOT available");

    const std::string game = FindGame();
    if (game.empty()) {
        Ps4::Log("no game: put an .nsp or .xci in %s/roms (or its path in game.txt)", Ps4::DataDir);
        Ps4::SetPadLight(255, 0, 0);
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 1;
    }
    Ps4::Log("game: %s", game.c_str());
    if (!fs::exists(fs::path{Ps4::DataDir} / "keys/prod.keys", ec)) {
        Ps4::Log("missing %s/keys/prod.keys - dump it from your Switch", Ps4::DataDir);
    }
    Ps4::SetPhase("firmware");
    InstallFirmware();

    Ps4::SetPhase("core init");
    ApplyPs4Settings();
    ApplySettingsFile();
    MoveEnvironmentOutOfHeap();
    if (Settings::IsFastmemEnabled()) {
        char report[200];
        Ps4::Log("fastmem self-test: starting (if this is the last line, put fastmem=off in settings.txt)");
        if (!Common::Orbis::FastmemSelfTest(report, sizeof(report))) {
            Settings::values.cpuopt_unsafe_host_mmu.SetValue(false);
        }
        Ps4::Log("%s", report);
    } else {
        Ps4::Log("fastmem: off (settings)");
    }
    Core::System system;
    system.Initialize();
    InputCommon::InputSubsystem input;
    input.Initialize();
    system.ApplySettings();
    Ps4Window window;

    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    system.GetUserChannel().clear();

    // The game's own contents, registered as Eden's desktop frontends do, so an update's data
    // applies on top of the base game.
    FileSys::ManualContentProvider game_contents;
    if (!game_contents.AddEntriesFromContainer(system.GetFilesystem()->OpenFile(game, FileSys::OpenMode::Read))) {
        Ps4::Log("game contents not registered (an update's data will not apply)");
    }
    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, &game_contents);

    std::mutex exit_mutex;
    std::condition_variable exit_cv;
    bool exited = false;
    system.RegisterExitCallback([&] {
        std::lock_guard lock{exit_mutex};
        exited = true;
        exit_cv.notify_all();
    });

    Ps4::SetPhase("loading game");
    Service::AM::FrontendAppletParameters params{.applet_id = Service::AM::AppletId::Application};
    const Core::SystemResultStatus loaded = system.Load(window, game, params);
    if (loaded != Core::SystemResultStatus::Success) {
        const unsigned code = static_cast<unsigned>(loaded);
        Ps4::Log("load failed: status %u%s", code,
                 loaded == Core::SystemResultStatus::ErrorVideoCore ? " (video core: see the RADV lines above)" : "");
        if (code > static_cast<unsigned>(Core::SystemResultStatus::ErrorLoader)) {
            const auto loader = static_cast<Loader::ResultStatus>(code - static_cast<unsigned>(Core::SystemResultStatus::ErrorLoader));
            Ps4::Log("loader: %s", Loader::GetResultStatusString(loader).c_str());
        }
        Ps4::SetPadLight(255, 0, 0);
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 2;
    }

    Ps4::SetPhase("starting GPU");
    system.GPU().Start();
    system.GetCpuManager().OnGpuReady();
    if (Settings::values.use_disk_shader_cache.GetValue()) {
        Ps4::SetPhase("shader cache");
        system.Renderer().ReadRasterizer()->LoadDiskResources(
            system.GetApplicationProcessProgramID(), std::stop_token{},
            [](VideoCore::LoadCallbackStage, size_t, size_t) {});
    }

    Ps4::SetPhase("running");
    Ps4::SetPadLight(0, 80, 255);
    Ps4::ArmHangWatch(15);
    system.Run();

    // Controller in, status out, until the game exits or Options + touch pad is held 2 s.
    auto* gamepad = input.GetVirtualGamepad();
    std::uint64_t quit_since = 0;
    std::uint64_t last_status = Ps4::NowUs();
    unsigned status_count = 0;
    for (;;) {
        {
            std::unique_lock lock{exit_mutex};
            if (exit_cv.wait_for(lock, std::chrono::milliseconds(8), [&] { return exited; })) {
                Ps4::Log("the game exited");
                break;
            }
        }
        const Ps4::PadState pad = Ps4::ReadPad();
        if (pad.connected && gamepad != nullptr) {
            PollPad(*gamepad, pad);
        }
        const bool quit_combo = (pad.buttons & Ps4::Button::Options) && (pad.buttons & Ps4::Button::TouchPad);
        if (!quit_combo) {
            quit_since = 0;
        } else if (quit_since == 0) {
            quit_since = Ps4::NowUs();
        } else if (Ps4::NowUs() - quit_since > 2000000) {
            Ps4::Log("quit combo held");
            break;
        }
        if (Ps4::NowUs() - last_status > 10000000) {
            last_status = Ps4::NowUs();
            const auto perf = system.GetAndResetPerfStats();
            const auto mem = Common::Orbis::GetStats();
            Ps4::Log("status: game %.1f fps, speed %.0f%%, frame %.1f ms, guest+tables %lu MiB committed (%lu MiB given back), largest free direct block %lu MiB",
                     perf.average_game_fps, perf.emulation_speed * 100.0, perf.frametime * 1000.0,
                     static_cast<unsigned long>(mem.committed_bytes >> 20),
                     static_cast<unsigned long>(mem.decommitted_bytes >> 20),
                     static_cast<unsigned long>(Ps4::FreeDirectMemory() >> 20));
            if (Common::Orbis::FastmemAvailable()) {
                const auto view = Common::Orbis::GetViewStats();
                Ps4::Log("fastmem: view %lu GiB (asked %lu GiB, last refusal 0x%08x), %lu pages "
                         "mapped (%lu MiB), %lu first touches, %lu slow-path redirects, %lu unaliasable",
                         static_cast<unsigned long>(view.view_bytes >> 30),
                         static_cast<unsigned long>(view.asked_bytes >> 30),
                         static_cast<unsigned>(view.reserve_error),
                         static_cast<unsigned long>(view.mapped_pages),
                         static_cast<unsigned long>(view.mapped_pages / 64),
                         static_cast<unsigned long>(view.first_touches),
                         static_cast<unsigned long>(view.jit_redirects),
                         static_cast<unsigned long>(view.conflicts));
                Ps4::Log("   redirects by cause: outside %lu (0x%lx), unmapped %lu (0x%lx), unreadable %lu "
                         "(0x%lx), write to read-only %lu (0x%lx), other %lu (0x%lx)",
                         static_cast<unsigned long>(view.redirect_reason[0]),
                         static_cast<unsigned long>(view.redirect_sample[0]),
                         static_cast<unsigned long>(view.redirect_reason[1]),
                         static_cast<unsigned long>(view.redirect_sample[1]),
                         static_cast<unsigned long>(view.redirect_reason[2]),
                         static_cast<unsigned long>(view.redirect_sample[2]),
                         static_cast<unsigned long>(view.redirect_reason[3]),
                         static_cast<unsigned long>(view.redirect_sample[3]),
                         static_cast<unsigned long>(view.redirect_reason[4]),
                         static_cast<unsigned long>(view.redirect_sample[4]));
            }
            CheckHeapEnvironment();
            if (++status_count % 3 == 1) {
                Ps4::LogDirectMemoryMap();
                Ps4HeapCensus();
            }
        }
    }

    // No teardown: on the console an app that returns from main (or calls exit) is reported as
    // CE-34878-0 anyway, and test 2 showed Eden's shutdown crashing in a worker thread here. The
    // game is over and its saves are written; the PS button closes the app cleanly.
    Ps4::SetPhase("finished");
    Ps4::ArmHangWatch(0);
    system.Pause();
    const auto mem = Common::Orbis::GetStats();
    Ps4::Log("finished: close the app with the PS button (lazy memory peak %lu MiB)",
             static_cast<unsigned long>(mem.committed_bytes >> 20));
    Ps4::SetPadLight(0, 255, 0);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
