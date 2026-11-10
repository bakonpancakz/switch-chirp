// Which game is running, and its name: for Rich Presence ("Playing <name>").
// The name comes with the game's whole control data (icon and all, 0x24000
// bytes), so it's read once per game into the screen-share send queue's buffer
// (screen::borrowRing: lent while no frames wait there), never into memory of its own.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace game {

// The running application's program id (0: none, e.g. the home menu)
uint64_t running();
// Its name, as the game gives it (English if it has it); false: not now (no buffer, or no data)
bool name(uint64_t program_id, char* out, size_t cap);

}  // namespace game
