// The mute and deafen icons: ICON px square pictures for tsl's
// drawBitmapRGBA4444, made once per pair of colours. Each is shapes (a mic,
// or headphones) with a slash across, and a see-through gap cut around the
// slash so it reads on whatever is behind (as Discord draws them). The slash
// can be another colour (a grey mic, a red slash: muted only for us). Edges
// are smoothed (4x4 samples a pixel).
#pragma once
#include <cstdint>

namespace icons {

constexpr int ICON = 20;

enum Kind { MicOff, HeadphonesOff };

struct Rgb {
    uint8_t r, g, b;  // 0..15 (a tsl::Color's)
};

const uint8_t* get(Kind k, Rgb body, Rgb slash);

}  // namespace icons
