#include "limits.h"

#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <switch.h>

extern "C" char* fake_heap_start;  // (Chirp's heap: main.cpp's inner_heap, set up in __libnx_initheap)
extern "C" char* fake_heap_end;

namespace limits {
namespace {

struct Sample {
    s64 cur[5], max[5];
    u64 used;
    unsigned heap_used, heap_total;  // (Chirp's heap: newlib's, inside INNER_HEAP_SIZE)
    bool ok;
};

Sample sample() {
    Sample s = {};
    u64 h = 0;
    // (a sysmodule's resource limit is the system's: shared by every sysmodule and system service)
    if (R_FAILED(svcGetInfo(&h, InfoType_ResourceLimit, INVALID_HANDLE, 0)) || !h) return s;
    Handle limit = (Handle)h;
    const LimitableResource which[5] = {LimitableResource_Memory, LimitableResource_Threads, LimitableResource_Events,
                                        LimitableResource_TransferMemories, LimitableResource_Sessions};
    for (int i = 0; i < 5; i++) {
        svcGetResourceLimitCurrentValue(&s.cur[i], limit, which[i]);
        svcGetResourceLimitLimitValue(&s.max[i], limit, which[i]);
    }
    svcCloseHandle(limit);
    svcGetInfo(&s.used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    struct mallinfo m = mallinfo();
    s.heap_used = (unsigned)m.uordblks;
    s.heap_total = (unsigned)(fake_heap_end - fake_heap_start);
    s.ok = true;
    return s;
}

void format(const Sample& s, char* out, size_t cap) {
    if (!s.ok) {
        snprintf(out, cap, "system: its resource limit can't be read");
        return;
    }
    const double MB = 1024.0 * 1024.0;
    snprintf(out, cap,
             "system: memory %.2f/%.2f MB (%.2f free), threads %lld/%lld, events %lld/%lld, transfer memories %lld/%lld, "
             "sessions %lld/%lld; Chirp uses %.2f MB, its heap %u/%u KB",
             s.cur[0] / MB, s.max[0] / MB, (s.max[0] - s.cur[0]) / MB, (long long)s.cur[1], (long long)s.max[1],
             (long long)s.cur[2], (long long)s.max[2], (long long)s.cur[3], (long long)s.max[3], (long long)s.cur[4],
             (long long)s.max[4], s.used / MB, s.heap_used / 1024, s.heap_total / 1024);
}

Sample g_last = {};
Mutex g_lock;  // (the voice thread and the network thread both watch)

}  // namespace

void describe(char* out, size_t cap) { format(sample(), out, cap); }

bool memory(unsigned* system_free_kb, unsigned* chirp_kb, unsigned* heap_used_kb, unsigned* heap_kb) {
    Sample s = sample();
    *system_free_kb = s.ok ? (unsigned)((s.max[0] - s.cur[0]) / 1024) : 0;
    *chirp_kb = (unsigned)(s.used / 1024);
    *heap_used_kb = s.heap_used / 1024, *heap_kb = s.heap_total / 1024;
    return s.ok;
}

void watch(void (*log)(const char* line)) {
    Sample s = sample();
    if (!s.ok) return;
    mutexLock(&g_lock);
    bool moved = !g_last.ok || llabs(s.cur[0] - g_last.cur[0]) >= 64 * 1024 ||
                 (s.heap_used > g_last.heap_used ? s.heap_used - g_last.heap_used : g_last.heap_used - s.heap_used) >= 16 * 1024;
    // (threads exactly; events, transfer memories and sessions are the whole system's, moving all the time
    // with other processes: only a real change, 16 or more)
    moved = moved || s.cur[1] != g_last.cur[1];
    for (int i = 2; i < 5; i++) moved = moved || llabs(s.cur[i] - g_last.cur[i]) >= 16;
    if (moved) g_last = s;
    mutexUnlock(&g_lock);
    if (!moved) return;
    char line[256];
    format(s, line, sizeof(line));
    log(line);
}

}  // namespace limits
