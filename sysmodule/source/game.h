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
// Its name, as the game gives it (English if it has it); false: not now (no buffer, or no data).
// video_capture (if given): whether it lets the system record video (its control data's VideoCapture:
// 0 no, 1 on request, 2 always; -1 unknown)
bool name(uint64_t program_id, char* out, size_t cap, int* video_capture = nullptr);

}  // namespace game
