// The HOME button's light (the ring on a Pro Controller's and a right Joy-Con's HOME button) as the
// mic's talk light: on while what the mic hears counts as talk and is being sent, off otherwise.
// Every controller connected lights up. hid:sys's notification LED: a pattern per controller, sent
// over Bluetooth, so only when it changes (and again every few seconds while it's on, each with a
// timeout: if Chirp stopped, the light would go out by itself). A Switch Lite has no light. The light
// has 15 brightness levels: a percentage is taken to the nearest (1..100 is never 0).
// (it follows the mic's talk exactly, pauses between words and all: on purpose)
// Called from the mic's thread (mic.cpp); the brightness from anywhere.
#pragma once

namespace homelight {

// 0: off (never lights); 1..100 percent
void setBrightness(int percent);
// talk or not, once a frame (sends only what changed)
void update(bool talk);

}  // namespace homelight
