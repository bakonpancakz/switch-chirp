#include "gamevol.h"

#include <switch.h>
#include <math.h>

namespace gamevol {
namespace {

constexpr uint64_t NEW_GAME_MS = 15'000;  // (a new game: given the volume each second this long)

Mutex g_lock;
int g_pct = 100;
uint64_t g_pid = 0, g_seen_ms = 0;  // the game running (its process), and when it was first seen
uint64_t g_lowered = 0;             // a game set below 100%, not given back yet
bool g_pending = false;             // a change aud:a didn't take (busy: Melody's, maybe): tried again

uint64_t nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

// As Melody's: each 10% is 4 dB, 0 is silence
float gainOf(int pct) { return pct <= 0 ? 0.0f : powf(10.0f, (pct / 100.0f - 1.0f) * 40.0f / 20.0f); }

// aud:a (IAudioSystemManagerForApplet) takes one session at a time: opened for the call, closed after.
// sys-tune found the output volume only sticks when the input one is set too, so both are
bool apply(uint64_t pid, float gain) {
    Service s;
    if (R_FAILED(smGetService(&s, "aud:a"))) return false;
    const struct {
        float volume;
        u64 pid;
        u64 timespan;
    } in = {gain, pid, 0};
    Result rc = serviceDispatchIn(&s, 5, in);                // SetAudioOutputProcessMasterVolume
    if (R_SUCCEEDED(rc)) rc = serviceDispatchIn(&s, 7, in);  // SetAudioInputProcessMasterVolume
    serviceClose(&s);
    return R_SUCCEEDED(rc);
}

// (g_lock held) what the volume says, to the game running
void applyNow() {
    g_pending = false;
    if (!g_pid) return;
    if (g_pct < 100) {
        if (apply(g_pid, gainOf(g_pct))) g_lowered = g_pid;
        else g_pending = true;
    } else if (g_lowered == g_pid) {  // (back to 100%: given back once)
        if (apply(g_pid, 1.0f)) g_lowered = 0;
        else g_pending = true;
    }
}

}  // namespace

void init(int percent) {
    mutexInit(&g_lock);
    g_pct = percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

int percent() { return g_pct; }

void set(int percent) {
    mutexLock(&g_lock);
    g_pct = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    applyNow();
    mutexUnlock(&g_lock);
}

void tick(uint64_t pid) {
    mutexLock(&g_lock);
    uint64_t now = nowMs();
    if (pid != g_pid) {
        g_pid = pid, g_seen_ms = now;
        g_lowered = 0;  // (a new process starts at full)
    }
    if (pid && (g_pending || now - g_seen_ms < NEW_GAME_MS)) applyNow();
    mutexUnlock(&g_lock);
}

}  // namespace gamevol
