// People's pictures: the PNGs the sysmodule puts in CHIRP_AVATARS, decoded
// small (box-filtered, as Melody does its covers) and cut round. Kept while
// the overlay is open; one that isn't on the card yet is looked for again
// now and then (the sysmodule may be fetching it).
#pragma once
#include <cstdint>

namespace avatar {

constexpr int SIZE = 32;

// SIZE x SIZE pixels for tsl's drawBitmapRGBA4444, or null: none (yet)
const uint8_t* get(const char* file);

}  // namespace avatar
