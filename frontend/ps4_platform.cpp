// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: the console-facing pieces of the frontend. See ps4_platform.h.

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <orbis/Pad.h>
#include <orbis_boot.h>
#include <orbis_log.h>

#include "common/orbis_lazy_memory.h"
#include "ps4_platform.h"

extern "C" {
int sceKernelInstallExceptionHandler(int signal, void (*handler)(int, void*));
int sceKernelRemoveExceptionHandler(int signal);
int sceKernelRaiseException(pthread_t thread, int signal);
int32_t sceKernelLoadStartModule(const char* path, size_t argc, const void* argv, uint32_t flags,
                                 void* option, int32_t* result);
const char* sceKernelGetFsSandboxRandomWord(void);
uint64_t sceKernelGetProcessTime(void);
int32_t sceKernelDebugOutText(int32_t channel, const char* text);
int32_t sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t align, off_t* phys,
                                          size_t* size);
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelGetDirectMemoryType(off_t start, int32_t* type, off_t* out_start, off_t* out_end);
int32_t sceUserServiceInitialize(void* params);
int32_t sceUserServiceGetInitialUser(int32_t* user);
int32_t sceSystemServiceHideSplashScreen(void);
}

namespace Ps4 {

namespace {

FILE* g_log = nullptr;
std::mutex g_log_mutex;
std::atomic<const char*> g_phase{"start"};
std::atomic<std::uint64_t> g_frames{0};
std::atomic<unsigned> g_hang_seconds{0};
// Written only by VulkanWorker; the crash report reads scalars without dereferencing handles.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
std::atomic<std::uint64_t> g_last_renderpass{0}, g_last_framebuffer{0}, g_last_pass_image{0};
std::atomic<std::uint64_t> g_last_pass_tick{0};
std::atomic<std::uint32_t> g_last_pass_images{0};
std::atomic<std::uint64_t> g_last_pass_view{0};
std::atomic<std::uint32_t> g_last_pass_guest_format{0}, g_last_pass_width{0},
    g_last_pass_height{0}, g_last_pass_samples{0};
int32_t g_pad = -1;

// PS4 ucontext: 16-byte signal mask + 48 bytes of padding, then a FreeBSD amd64 mcontext.
constexpr std::size_t Mc = 64;
constexpr std::size_t McRbp = 72, McAddr = 136, McRip = 160, McRsp = 184;
// The SDK's bits/signal.h describes musl's Linux layout, not the kernel's context.
// Register order verified against FreeBSD releng/9.0 sys/amd64/include/ucontext.h;
// the context base and selector anchors are also documented by orbis-compat/src/orbis_boot.cpp.
constexpr std::size_t McRdi = 8, McRsi = 16, McRdx = 24, McRcx = 32, McR8 = 40, McR9 = 48;
constexpr std::size_t McRax = 56, McRbx = 64, McR10 = 80, McR11 = 88, McR12 = 96;
constexpr std::size_t McR13 = 104, McR14 = 112, McR15 = 120, McTrapno = 128, McErr = 152;
constexpr std::size_t McCs = 168, McRflags = 176, McSs = 192;
constexpr std::uint64_t ImageBase = 0x400000;

std::uint64_t Read64(const void* base, std::size_t offset) {
    std::uint64_t value;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(value));
    return value;
}

void LogV(const char* fmt, va_list ap) {
    char line[1024];
    const double t = static_cast<double>(NowUs()) / 1e6;
    int n = std::snprintf(line, sizeof(line), "[%9.3f] ", t);
    n += std::vsnprintf(line + n, sizeof(line) - static_cast<size_t>(n) - 2, fmt, ap);
    if (n > static_cast<int>(sizeof(line)) - 2) {
        n = static_cast<int>(sizeof(line)) - 2;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        --n;
    }
    line[n++] = '\n';
    line[n] = 0;
    sceKernelDebugOutText(0, line);
    std::lock_guard lock{g_log_mutex};
    if (g_log != nullptr) {
        std::fwrite(line, 1, static_cast<size_t>(n), g_log);
        std::fflush(g_log);
        fsync(fileno(g_log));
    }
}

void OrbisLog(const char* fmt, va_list ap) {
    char buffer[900];
    std::vsnprintf(buffer, sizeof(buffer), fmt, ap);
    Log("  | %s", buffer);
}

void OrbisFatal(const char* what) {
    Log("!! FATAL from the driver/overlay during %s: %s", g_phase.load(), what ? what : "(null)");
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// Faults that are not a first touch of lazily committed memory.
void CrashHandler(int signal, void* ucontext) {
    const void* mc = static_cast<const unsigned char*>(ucontext) + Mc;
    const std::uint64_t rip = Read64(mc, McRip);
    const std::uint64_t rsp = Read64(mc, McRsp);
    std::uint64_t rbp = Read64(mc, McRbp);
    Log("!! CRASH signal %d during %s: rip=0x%lx (eboot+0x%lx) fault address=0x%lx rsp=0x%lx", signal,
        g_phase.load(), static_cast<unsigned long>(rip), static_cast<unsigned long>(rip - ImageBase),
        static_cast<unsigned long>(Read64(mc, McAddr)), static_cast<unsigned long>(rsp));
    // Only read scalars from the kernel-provided context; never follow register/handle pointers.
    if (Read64(mc, McCs) == 0x43 && Read64(mc, McSs) == 0x3b) {
        Log("!! registers: rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx",
            static_cast<unsigned long>(Read64(mc, McRax)),
            static_cast<unsigned long>(Read64(mc, McRbx)),
            static_cast<unsigned long>(Read64(mc, McRcx)),
            static_cast<unsigned long>(Read64(mc, McRdx)));
        Log("!! registers: rdi=0x%lx rsi=0x%lx rbp=0x%lx rsp=0x%lx",
            static_cast<unsigned long>(Read64(mc, McRdi)),
            static_cast<unsigned long>(Read64(mc, McRsi)), static_cast<unsigned long>(rbp),
            static_cast<unsigned long>(rsp));
        Log("!! registers: r8=0x%lx r9=0x%lx r10=0x%lx r11=0x%lx",
            static_cast<unsigned long>(Read64(mc, McR8)),
            static_cast<unsigned long>(Read64(mc, McR9)),
            static_cast<unsigned long>(Read64(mc, McR10)),
            static_cast<unsigned long>(Read64(mc, McR11)));
        Log("!! registers: r12=0x%lx r13=0x%lx r14=0x%lx r15=0x%lx",
            static_cast<unsigned long>(Read64(mc, McR12)),
            static_cast<unsigned long>(Read64(mc, McR13)),
            static_cast<unsigned long>(Read64(mc, McR14)),
            static_cast<unsigned long>(Read64(mc, McR15)));
        Log("!! context: trapno=%u err=0x%lx rflags=0x%lx cs=0x43 ss=0x3b mc_offset=%zu",
            static_cast<unsigned>(Read64(mc, McTrapno) & 0xffffffffu),
            static_cast<unsigned long>(Read64(mc, McErr)),
            static_cast<unsigned long>(Read64(mc, McRflags)), Mc);
    } else {
        Log("!! register names omitted: unexpected context selectors cs=0x%lx ss=0x%lx mc_offset=%zu",
            static_cast<unsigned long>(Read64(mc, McCs)),
            static_cast<unsigned long>(Read64(mc, McSs)), Mc);
        for (std::size_t word = 0; word < 16; word += 4) {
            Log("!! context raw mc[%zu..%zu]: %016lx %016lx %016lx %016lx", word, word + 3,
                static_cast<unsigned long>(Read64(mc, (word + 0) * 8)),
                static_cast<unsigned long>(Read64(mc, (word + 1) * 8)),
                static_cast<unsigned long>(Read64(mc, (word + 2) * 8)),
                static_cast<unsigned long>(Read64(mc, (word + 3) * 8)));
        }
    }
    Log("!! last Vulkan BeginRenderPass: rp=0x%lx fb=0x%lx first_image=0x%lx images=%u tick=%lu",
        static_cast<unsigned long>(g_last_renderpass.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_last_framebuffer.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_last_pass_image.load(std::memory_order_relaxed)),
        g_last_pass_images.load(std::memory_order_relaxed),
        static_cast<unsigned long>(g_last_pass_tick.load(std::memory_order_relaxed)));
    Log("!! last Vulkan attachment: first_view=0x%lx guest_format=0x%x size=%ux%u samples=%u",
        static_cast<unsigned long>(g_last_pass_view.load(std::memory_order_relaxed)),
        g_last_pass_guest_format.load(std::memory_order_relaxed),
        g_last_pass_width.load(std::memory_order_relaxed),
        g_last_pass_height.load(std::memory_order_relaxed),
        g_last_pass_samples.load(std::memory_order_relaxed));
    if (rip < 0x10000) {
        // A call through a null (or tiny) function pointer: the caller's return address is on top.
        const std::uint64_t ret = *reinterpret_cast<const std::uint64_t*>(rsp);
        Log("   called from eboot+0x%lx (null function pointer)", static_cast<unsigned long>(ret - ImageBase));
    }
    // Frame-pointer walk, bounded to the stack above rsp (the build keeps frame pointers).
    for (int depth = 0; depth < 32; ++depth) {
        if (rbp < rsp || rbp - rsp > 8 * 1024 * 1024 || (rbp & 7) != 0) {
            break;
        }
        const std::uint64_t ret = reinterpret_cast<const std::uint64_t*>(rbp)[1];
        Log("   #%02d eboot+0x%lx", depth, static_cast<unsigned long>(ret - ImageBase));
        const std::uint64_t next = reinterpret_cast<const std::uint64_t*>(rbp)[0];
        if (next <= rbp) {
            break;
        }
        rbp = next;
    }
    const auto stats = Common::Orbis::GetStats();
    Log("!! lazy memory: %lu MiB committed of %lu MiB reserved, %lu failed commits",
        static_cast<unsigned long>(stats.committed_bytes >> 20),
        static_cast<unsigned long>(stats.reserved_bytes >> 20),
        static_cast<unsigned long>(stats.commit_failures));
    // Give the fault back to the system: without a handler it ends the process.
    sceKernelRemoveExceptionHandler(11);
    sceKernelRemoveExceptionHandler(10);
    sceKernelRemoveExceptionHandler(4);
    sceKernelRemoveExceptionHandler(8);
}

// ---- named threads, for stack dumps when frames stop ----------------------------------------
struct NamedThread {
    pthread_t thread;
    char name[40];
};
constexpr int MaxNamed = 160;
NamedThread g_named[MaxNamed];
std::atomic<int> g_named_count{0};
std::mutex g_named_mutex;
constexpr int SigDump = 30; // FreeBSD SIGUSR1: what sceKernelRaiseException delivers

const char* NameOf(pthread_t thread) {
    const int n = g_named_count.load();
    for (int i = n - 1; i >= 0; --i) {
        if (pthread_equal(g_named[i].thread, thread)) {
            return g_named[i].name;
        }
    }
    return "?";
}

// ---- sampling profiler (test 22): where the busy threads spend their time --------------------
// A sampler thread raises SigDump on one profiled thread at a time; when the handler sees the
// request is for itself it counts its rip (64-byte buckets) instead of logging a stack. Every
// 30 s each thread's busy share and hottest buckets go to boot.log (symbolize the eboot offsets).
constexpr int ProfiledMax = 8;
constexpr int ProfileBuckets = 2048;
struct ProfileTable {
    char name[40];
    pthread_t thread;
    std::uint64_t keys[ProfileBuckets];
    std::uint32_t counts[ProfileBuckets];
    std::uint32_t samples;
    std::uint32_t waiting; // rip inside libkernel: blocked or sleeping
    std::uint32_t jit;     // rip in guest code generated by the JIT
};
ProfileTable g_profile[ProfiledMax];
// Set while DumpAllThreads asks every thread for its stack (only then may the handler log).
std::atomic<bool> g_dump_active{false};
std::atomic<bool> g_profiling{true};
std::atomic<int> g_profile_count{0};
std::atomic<int> g_profile_request{-1};

void ProfileRecord(ProfileTable& t, std::uint64_t rip) {
    ++t.samples;
    if (rip >= 0x7ff000000ull) {
        ++t.waiting;
        return;
    }
    if (rip < ImageBase || rip > ImageBase + 0x10000000ull) {
        ++t.jit;
        return;
    }
    const std::uint64_t key = (rip - ImageBase) & ~std::uint64_t{63};
    std::uint32_t slot = static_cast<std::uint32_t>((key >> 6) * 2654435761u) % ProfileBuckets;
    for (int probe = 0; probe < 32; ++probe, slot = (slot + 1) % ProfileBuckets) {
        if (t.keys[slot] == key || t.counts[slot] == 0) {
            t.keys[slot] = key;
            ++t.counts[slot];
            return;
        }
    }
}

void DumpHandler(int, void* ucontext) {
    const void* mc = static_cast<const unsigned char*>(ucontext) + Mc;
    const std::uint64_t rip = Read64(mc, McRip);
    {
        const int request = g_profile_request.load(std::memory_order_acquire);
        if (request >= 0 && pthread_equal(g_profile[request].thread, pthread_self())) {
            ProfileRecord(g_profile[request], rip);
            g_profile_request.store(-1, std::memory_order_release);
            return;
        }
        // Not a stack dump either: a profiler sample that arrived late. Never log from here then:
        // the interrupted thread may hold the log's mutex (test 22 deadlocked the whole emulator
        // that way while the allocator was logging a carve-out).
        if (!g_dump_active.load(std::memory_order_acquire)) {
            return;
        }
    }
    const std::uint64_t rsp = Read64(mc, McRsp);
    std::uint64_t rbp = Read64(mc, McRbp);
    char line[900];
    int n = std::snprintf(line, sizeof(line), "   [%s] eboot+0x%lx", NameOf(pthread_self()),
                          static_cast<unsigned long>(rip - ImageBase));
    for (int depth = 0; depth < 20 && n < static_cast<int>(sizeof(line)) - 24; ++depth) {
        if (rbp < rsp || rbp - rsp > 8 * 1024 * 1024 || (rbp & 7) != 0) {
            break;
        }
        const std::uint64_t ret = reinterpret_cast<const std::uint64_t*>(rbp)[1];
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " <- 0x%lx",
                           static_cast<unsigned long>(ret - ImageBase));
        const std::uint64_t next = reinterpret_cast<const std::uint64_t*>(rbp)[0];
        if (next <= rbp) {
            break;
        }
        rbp = next;
    }
    Log("%s", line);
}

void DumpAllThreads(const char* why) {
    Log("!! thread stacks (%s); offsets are eboot+0x..., newest frame first:", why);
    // No profiler samples meanwhile, so every signal now is a dump request.
    const bool was_profiling = g_profiling.exchange(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    g_dump_active.store(true, std::memory_order_release);
    const int n = g_named_count.load();
    for (int i = 0; i < n; ++i) {
        const int rc = sceKernelRaiseException(g_named[i].thread, SigDump);
        if (rc != 0) {
            Log("   [%s] (gone: 0x%08x)", g_named[i].name, static_cast<unsigned>(rc));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    g_dump_active.store(false, std::memory_order_release);
    g_profiling.store(was_profiling);
}

void Watchdog() {
    std::uint64_t last_frames = 0;
    std::uint64_t still_since = NowUs();
    bool reported = false;
    bool second = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const unsigned limit = g_hang_seconds.load();
        const std::uint64_t frames = g_frames.load();
        if (frames != last_frames) {
            last_frames = frames;
            still_since = NowUs();
            reported = false;
            second = false;
            continue;
        }
        if (limit == 0) {
            continue;
        }
        const std::uint64_t still = NowUs() - still_since;
        // Two dumps: when frames stop, and 30 s later (what moved in between is the clue).
        if (!reported && still > std::uint64_t(limit) * 1000000) {
            const auto stats = Common::Orbis::GetStats();
            Log("!! no new frame for %u s during %s (lazy memory %lu MiB, %lu failed commits)", limit,
                g_phase.load(), static_cast<unsigned long>(stats.committed_bytes >> 20),
                static_cast<unsigned long>(stats.commit_failures));
            DumpAllThreads("first");
            reported = true;
        } else if (reported && !second && still > std::uint64_t(limit + 30) * 1000000) {
            DumpAllThreads("30 s later");
            second = true;
        }
    }
}

void ProfileReport() {
    const int n = g_profile_count.load();
    for (int i = 0; i < n; ++i) {
        ProfileTable& t = g_profile[i];
        if (t.samples == 0) {
            continue;
        }
        const std::uint32_t busy = t.samples - t.waiting;
        Log("profile [%s]: %u samples, busy %u%% (jit %u%% of busy), waiting %u%%", t.name, t.samples,
            busy * 100 / t.samples, busy ? t.jit * 100 / busy : 0u, t.waiting * 100 / t.samples);
        char line[900];
        int len = std::snprintf(line, sizeof(line), "   hot:");
        for (int shown = 0; shown < 12; ++shown) {
            int best = -1;
            for (int b = 0; b < ProfileBuckets; ++b) {
                if (t.counts[b] != 0 && (best < 0 || t.counts[b] > t.counts[best])) {
                    best = b;
                }
            }
            if (best < 0 || busy == 0) {
                break;
            }
            len += std::snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " 0x%lx=%u%%",
                                 static_cast<unsigned long>(t.keys[best]),
                                 t.counts[best] * 100 / busy);
            t.counts[best] = 0;
            if (len > static_cast<int>(sizeof(line)) - 40) {
                break;
            }
        }
        Log("%s", line);
        std::memset(t.keys, 0, sizeof(t.keys));
        std::memset(t.counts, 0, sizeof(t.counts));
        t.samples = t.waiting = t.jit = 0;
    }
}

void Profiler() {
    std::uint64_t next_report = NowUs() + 30000000;
    int cursor = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        const int n = g_profile_count.load();
        if (n == 0 || !g_profiling.load(std::memory_order_relaxed)) {
            continue;
        }
        cursor = (cursor + 1) % n;
        if (g_profile_request.load() >= 0) {
            g_profile_request.store(-1); // the last one never arrived (thread gone)
        }
        g_profile_request.store(cursor, std::memory_order_release);
        if (sceKernelRaiseException(g_profile[cursor].thread, SigDump) != 0) {
            g_profile_request.store(-1);
        }
        if (NowUs() >= next_report) {
            // Let the in-flight sample land before reading the tables.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            g_profile_request.store(-1);
            ProfileReport();
            next_report = NowUs() + 30000000;
        }
    }
}

int32_t LoadModule(const std::string& path) {
    int32_t result = 0;
    const int32_t handle = sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, &result);
    if (handle < 0) {
        Log("module %s: 0x%08x", path.c_str(), static_cast<unsigned>(handle));
    }
    return handle;
}

} // Anonymous namespace

std::uint64_t NowUs() {
    return sceKernelGetProcessTime();
}

void OpenBootLog() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    mkdir(DataDir, 0777);
    const std::string path = std::string(DataDir) + "/boot.log";
    // Keep the previous session's log next to the new one.
    const std::string old = std::string(DataDir) + "/boot.old.log";
    std::remove(old.c_str());
    std::rename(path.c_str(), old.c_str());
    g_log = std::fopen(path.c_str(), "w");
    if (g_log != nullptr) {
        std::setvbuf(g_log, nullptr, _IONBF, 0);
    }
}

void SetProfiling(bool enabled) {
    g_profiling.store(enabled);
}

void StartWatchdog() {
    // From main(), not from the early constructor: no threads before static initialisation ends.
    std::thread{Watchdog}.detach();
    std::thread{Profiler}.detach();
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

void RegisterThread(const char* name) {
    std::lock_guard lock{g_named_mutex};
    const int n = g_named_count.load();
    if (n < MaxNamed) {
        g_named[n].thread = pthread_self();
        std::snprintf(g_named[n].name, sizeof(g_named[n].name), "%s", name);
        g_named_count.store(n + 1);
    }
    // The threads the frame rate depends on are also sampled by the profiler.
    const bool profiled = std::strcmp(name, "GPU") == 0 || std::strcmp(name, "VulkanWorker") == 0 ||
                          std::strncmp(name, "CPUCore_", 8) == 0;
    const int p = g_profile_count.load();
    if (profiled && p < ProfiledMax) {
        g_profile[p].thread = pthread_self();
        std::snprintf(g_profile[p].name, sizeof(g_profile[p].name), "%s", name);
        g_profile_count.store(p + 1);
    }
}

void InstallCrashReporting() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    orbis_set_log(OrbisLog);
    orbis_set_log_fatal(OrbisLog);
    orbis_set_fatal_action(OrbisFatal);
    // std::terminate (uncaught C++ exceptions) and SIGABRT, reported through orbis_log; faults
    // stay with the kernel exception handlers installed after it.
    orbis::installCrashHandlers();
    Common::Orbis::InstallFaultHandler(CrashHandler);
    sceKernelInstallExceptionHandler(SigDump, DumpHandler);
    sceKernelInstallExceptionHandler(4, CrashHandler);
    sceKernelInstallExceptionHandler(8, CrashHandler);
}

void SetPhase(const char* phase) {
    g_phase.store(phase);
    Log("== %s", phase);
}

void ArmHangWatch(unsigned seconds) {
    g_hang_seconds.store(seconds);
}

void NoteFrame() {
    g_frames.fetch_add(1, std::memory_order_relaxed);
}

void LoadSystemModules() {
    const char* word = sceKernelGetFsSandboxRandomWord();
    const std::string dir = std::string("/") + (word != nullptr ? word : "system") + "/common/lib/";
    for (const char* name : {"libSceSysCore", "libSceMbus", "libSceIpmi", "libSceSystemService",
                             "libSceUserService", "libSceAudioOut", "libScePad"}) {
        LoadModule(dir + name + ".sprx");
    }
}

void HideSplashScreen() {
    sceSystemServiceHideSplashScreen();
}

bool OpenPad() {
    struct {
        int32_t priority;
    } params{700};
    sceUserServiceInitialize(&params);
    int32_t user = -1;
    if (sceUserServiceGetInitialUser(&user) < 0) {
        Log("sceUserServiceGetInitialUser failed");
        return false;
    }
    const int32_t init = scePadInit();
    if (init < 0) {
        Log("scePadInit: 0x%08x", static_cast<unsigned>(init));
    }
    g_pad = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    if (g_pad < 0) {
        g_pad = scePadGetHandle(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0);
    }
    if (g_pad < 0) {
        Log("scePadOpen failed: 0x%08x", static_cast<unsigned>(g_pad));
        return false;
    }
    return true;
}

PadState ReadPad() {
    PadState state;
    if (g_pad < 0) {
        return state;
    }
    OrbisPadData data;
    std::memset(&data, 0, sizeof(data));
    if (scePadReadState(g_pad, &data) < 0 || !data.connected) {
        return state;
    }
    state.connected = true;
    state.buttons = data.buttons;
    state.lx = data.leftStick.x;
    state.ly = data.leftStick.y;
    state.rx = data.rightStick.x;
    state.ry = data.rightStick.y;
    state.l2 = data.analogButtons.l2;
    state.r2 = data.analogButtons.r2;
    return state;
}

void SetPadLight(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (g_pad < 0) {
        return;
    }
    OrbisPadColor color{r, g, b, 255};
    scePadSetLightBar(g_pad, &color);
}

void LogDirectMemoryMap() {
    struct Big {
        std::uint64_t start, end;
        int type;
    };
    constexpr int MaxBig = 256;
    static Big previous[MaxBig];
    static int previous_count = -1;
    Big current[MaxBig];
    int count = 0;
    const std::uint64_t total = sceKernelGetDirectMemorySize();
    constexpr std::uint64_t Step = 16 * 1024;
    std::uint64_t used = 0, free_bytes = 0, largest = 0, run = 0, regions = 0, runs = 0;
    for (std::uint64_t off = 0; off < total;) {
        int32_t type = -1;
        off_t start = 0, end = 0;
        const bool allocated =
            sceKernelGetDirectMemoryType(static_cast<off_t>(off), &type, &start, &end) == 0 &&
            static_cast<std::uint64_t>(end) > off;
        if (!allocated) {
            run += Step;
            free_bytes += Step;
            off += Step;
            continue;
        }
        // Some kernels report the next allocation at or after `off`: the gap is free.
        if (static_cast<std::uint64_t>(start) > off) {
            const std::uint64_t gap = static_cast<std::uint64_t>(start) - off;
            run += gap;
            free_bytes += gap;
            off = static_cast<std::uint64_t>(start);
        }
        if (run != 0) {
            largest = std::max(largest, run);
            ++runs;
            run = 0;
        }
        const std::uint64_t size = static_cast<std::uint64_t>(end) - off;
        used += size;
        ++regions;
        if (size >= (2u << 20) && count < MaxBig) {
            current[count++] = {off, static_cast<std::uint64_t>(end), type};
        }
        off = static_cast<std::uint64_t>(end);
    }
    if (run != 0) {
        largest = std::max(largest, run);
        ++runs;
    }
    Log("direct map: %lu MiB used in %lu allocations, %lu MiB free in %lu runs (largest %lu MiB), "
        "%d allocations >= 2 MiB",
        static_cast<unsigned long>(used >> 20), static_cast<unsigned long>(regions),
        static_cast<unsigned long>(free_bytes >> 20), static_cast<unsigned long>(runs),
        static_cast<unsigned long>(largest >> 20), count);
    char line[900];
    int len = 0;
    for (int i = 0; i < count; ++i) {
        bool seen = false;
        for (int j = 0; j < previous_count && !seen; ++j) {
            seen = previous[j].start == current[i].start && previous[j].end == current[i].end;
        }
        if (seen) {
            continue;
        }
        if (len == 0) {
            len = std::snprintf(line, sizeof(line), "   %s:", previous_count < 0 ? "allocations" : "new");
        }
        len += std::snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " 0x%lx+%luM(t%d)",
                             static_cast<unsigned long>(current[i].start),
                             static_cast<unsigned long>((current[i].end - current[i].start) >> 20),
                             current[i].type);
        if (len > static_cast<int>(sizeof(line)) - 48) {
            Log("%s", line);
            len = 0;
        }
    }
    if (len != 0) {
        Log("%s", line);
    }
    std::memcpy(previous, current, sizeof(Big) * static_cast<size_t>(count));
    previous_count = count;
}

std::uint64_t FreeDirectMemory() {
    off_t phys = 0;
    size_t size = 0;
    sceKernelAvailableDirectMemorySize(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), 0, &phys,
                                       &size);
    return size;
}

} // namespace Ps4

// Before any other static initialiser of the program (Eden has many): a crash there still
// leaves boot.log with the reason.
__attribute__((constructor(101))) static void EarlyCrashReporting() {
    Ps4::OpenBootLog();
    Ps4::Log("eden-ps4: early start");
    Ps4::InstallCrashReporting();
}

// thread_local destructors. libc++abi only builds its __cxa_thread_atexit for Linux-like
// systems, and the SDK's libc has none: run them from a pthread key destructor, newest first.
// (The main thread's are not run at exit, which is when the process ends anyway.)
namespace {
struct ThreadAtexit {
    void (*dtor)(void*);
    void* obj;
    ThreadAtexit* next;
};
pthread_key_t g_tls_dtor_key;
pthread_once_t g_tls_dtor_once = PTHREAD_ONCE_INIT;
void RunThreadAtexit(void* head) {
    for (auto* node = static_cast<ThreadAtexit*>(head); node != nullptr;) {
        ThreadAtexit* const next = node->next;
        node->dtor(node->obj);
        std::free(node);
        node = next;
    }
}
} // Anonymous namespace

extern "C" int __cxa_thread_atexit(void (*dtor)(void*), void* obj, void* /*dso*/) {
    pthread_once(&g_tls_dtor_once, [] { pthread_key_create(&g_tls_dtor_key, RunThreadAtexit); });
    auto* node = static_cast<ThreadAtexit*>(std::malloc(sizeof(ThreadAtexit)));
    if (node == nullptr) {
        return -1;
    }
    node->dtor = dtor;
    node->obj = obj;
    node->next = static_cast<ThreadAtexit*>(pthread_getspecific(g_tls_dtor_key));
    pthread_setspecific(g_tls_dtor_key, node);
    return 0;
}

// zstd declares its tracing hooks weak and calls them only if they are defined; the PS4's
// create-fself refuses an undefined symbol, weak or not, so they are defined here as "no tracing".
extern "C" {
unsigned long long ZSTD_trace_compress_begin(const void*) {
    return 0;
}
void ZSTD_trace_compress_end(unsigned long long, const void*) {}
unsigned long long ZSTD_trace_decompress_begin(const void*) {
    return 0;
}
void ZSTD_trace_decompress_end(unsigned long long, const void*) {}
}

// Eden names its threads through Common::SetCurrentThreadName (src/common/thread.cpp calls this
// weak hook on the PS4).
extern "C" void EdenPs4OnRenderPass(std::uint64_t renderpass, std::uint64_t framebuffer,
                                    std::uint64_t first_image, std::uint32_t num_images,
                                    std::uint64_t tick, std::uint64_t first_view,
                                    std::uint32_t first_guest_format, std::uint32_t width,
                                    std::uint32_t height, std::uint32_t samples) {
    Ps4::g_last_renderpass.store(renderpass, std::memory_order_relaxed);
    Ps4::g_last_framebuffer.store(framebuffer, std::memory_order_relaxed);
    Ps4::g_last_pass_image.store(first_image, std::memory_order_relaxed);
    Ps4::g_last_pass_images.store(num_images, std::memory_order_relaxed);
    Ps4::g_last_pass_tick.store(tick, std::memory_order_relaxed);
    Ps4::g_last_pass_view.store(first_view, std::memory_order_relaxed);
    Ps4::g_last_pass_guest_format.store(first_guest_format, std::memory_order_relaxed);
    Ps4::g_last_pass_width.store(width, std::memory_order_relaxed);
    Ps4::g_last_pass_height.store(height, std::memory_order_relaxed);
    Ps4::g_last_pass_samples.store(samples, std::memory_order_relaxed);
}

extern "C" void EdenPs4OnThreadNamed(pthread_t thread, const char* name) {
    (void)thread; // always the calling thread
    Ps4::RegisterThread(name);
}

/// The registered name of a thread (texture-cache history), or "?".
extern "C" const char* EdenPs4ThreadName(unsigned long long thread) {
    pthread_t handle{};
    std::memcpy(&handle, &thread, sizeof(handle) < sizeof(thread) ? sizeof(handle) : sizeof(thread));
    return Ps4::NameOf(handle);
}

/// Texture-cache inconsistency (texture_cache.h PS4_TC_TRACE): the event and its caller chain,
/// for the first 24 events of the run, so the path that breaks the cache can be symbolized.
extern "C" void EdenPs4Backtrace(const char* why, unsigned long long a, unsigned long long b,
                                 unsigned long long c, unsigned long long d) {
    static std::atomic<int> events{0};
    const int event = ++events;
    if (event > 24) {
        return;
    }
    char line[900];
    int n = std::snprintf(line, sizeof(line), "!! texture cache #%d: %s = 0x%llx 0x%llx 0x%llx 0x%llx;"
                          " callers:", event, why, a, b, c, d);
    auto rbp = reinterpret_cast<std::uint64_t>(__builtin_frame_address(0));
    for (int depth = 0; depth < 18 && n < static_cast<int>(sizeof(line)) - 24; ++depth) {
        const std::uint64_t ret = reinterpret_cast<const std::uint64_t*>(rbp)[1];
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " 0x%lx",
                           static_cast<unsigned long>(ret - Ps4::ImageBase));
        const std::uint64_t next = reinterpret_cast<const std::uint64_t*>(rbp)[0];
        if (next <= rbp || next - rbp > 8 * 1024 * 1024 || (next & 7) != 0) {
            break;
        }
        rbp = next;
    }
    Ps4::Log("%s", line);
}
