// Chirp's own share of core 3: every thread it starts is noted here, and their CPU time is summed for the
// log beside core 3's idle time (what's left is everyone else's: the system's and other sysmodules')
#pragma once
#include <stdint.h>

namespace cpushare {
void track(uint32_t thread_handle);  // (on the Switch only: elsewhere nothing)
uint64_t ticks();                     // all of Chirp's threads' CPU time so far, the main thread's too
}
