// The system's resource limit, the one Chirp shares with every other
// sysmodule (memory, threads, events, transfer memories, sessions): how much
// of each is used, for the log. When one runs out, other system services fail
// (ns: 2001-0132, "limit reached"), so this says which, and how close it is.
#pragma once
#include <stddef.h>

namespace limits {

// "system: memory 31.2/32.0 MB, threads 380/800, ..., Chirp 1.7 MB" into out
void describe(char* out, size_t cap);

// The memory numbers, for the overlay's Debug (KB): what's left of the memory all sysmodules share,
// Chirp's own, and its heap's use and size. False: the limit can't be read
bool memory(unsigned* system_free_kb, unsigned* chirp_kb, unsigned* heap_used_kb, unsigned* heap_kb);

// Called often (every second or so): logs describe()'s line when anything moved since the
// last one it logged (memory by 64 KB or more, any count): the steps up to a crash, not a flood
void watch(void (*log)(const char* line));

}  // namespace limits
