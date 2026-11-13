// The game's own volume, apart from the call's: the audio server keeps a master volume for each
// process, and aud:a sets it (as Melody and sys-tune do). It's set when it changes and for a new game's
// first seconds, then it stays (sleep and the HOME menu keep it). Nothing is done while it's at 100%
// and nothing was lowered, so it and Melody's game volume only meet when both are turned down.
#pragma once
#include <stdint.h>

namespace gamevol {

void init(int percent);  // (settings.ini's, before anything runs: not applied until a game's seen)
int percent();           // 0..100
void set(int percent);   // (the IPC thread's) applied to the game running at once
// Once a second (the network thread's): the running application's process id (0: none). A new game
// isn't listening yet when it starts, so it's given the volume each second for its first 15 s
void tick(uint64_t pid);

}  // namespace gamevol
