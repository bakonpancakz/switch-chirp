#include "cpushare.h"

#ifdef __SWITCH__
#include <switch.h>

namespace cpushare {
namespace {
constexpr int MAX = 16;
Handle g_threads[MAX];
int g_n = 0;
Mutex g_lock;  // (zero-initialized: an unlocked mutex)
}  // namespace

void track(uint32_t thread_handle) {
    mutexLock(&g_lock);
    if (g_n < MAX) g_threads[g_n++] = thread_handle;
    mutexUnlock(&g_lock);
}

uint64_t ticks() {
    uint64_t sum = 0, t = 0;
    if (R_SUCCEEDED(svcGetInfo(&t, InfoType_ThreadTickCount, envGetMainThreadHandle(), TickCountInfo_Total))) sum += t;
    mutexLock(&g_lock);
    for (int i = 0; i < g_n; i++)
        if (R_SUCCEEDED(svcGetInfo(&t, InfoType_ThreadTickCount, g_threads[i], TickCountInfo_Total))) sum += t;
    mutexUnlock(&g_lock);
    return sum;
}
}  // namespace cpushare

#else
namespace cpushare {
void track(uint32_t) {}
uint64_t ticks() { return 0; }
}  // namespace cpushare
#endif
