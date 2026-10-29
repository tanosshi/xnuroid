// Vibration for the guest's UIFeedbackGenerator calls.
//
// An app host installs a hook (Android Vibrator through JNI); without one, the shell's vibrator service is driven
// with `cmd vibrator_manager`, spawned asynchronously so the caller (often the game thread) never waits.
// LOADER_HAPTICS=0 turns vibration off.
#include <spawn.h>
#include <sys/wait.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>

extern char** environ;

namespace {
std::atomic<void (*)(int, int)> g_hook{nullptr};
std::atomic<int64_t> g_last_ms{0};

int64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
}  // namespace

extern "C" void host_set_vibrate_hook(void (*hook)(int ms, int amplitude)) { g_hook = hook; }

// ms: duration; amplitude: 1..255
extern "C" void host_vibrate(int ms, int amplitude) {
    static const bool off = getenv("LOADER_HAPTICS") && getenv("LOADER_HAPTICS")[0] == '0';
    if (off || ms <= 0) return;
    int64_t t = now_ms(), last = g_last_ms.load();
    if (t - last < 25 || !g_last_ms.compare_exchange_strong(last, t)) return;   // haptic taps closer than 25 ms merge
    if (auto hook = g_hook.load()) { hook(ms, amplitude); return; }
    std::thread([ms, amplitude] {
        std::string d = std::to_string(ms), a = std::to_string(amplitude < 1 ? 1 : amplitude > 255 ? 255 : amplitude);
        const char* argv[] = {"/system/bin/cmd", "vibrator_manager", "synced", "-f", "oneshot", "-a", d.c_str(), a.c_str(), nullptr};
        pid_t pid;
        if (posix_spawn(&pid, argv[0], nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0) waitpid(pid, nullptr, 0);
    }).detach();
}
