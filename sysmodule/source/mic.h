// The microphone: its own thread recording from audin (the default mic: the
// headset's, 48 kHz), mixed down to mono, turned up or down by the input
// volume, the room's noise taken out (RNNoise, if it's on), the rumble filtered
// out, and while someone's talking, made louder or
// quieter toward speech level (automatic gain, if it's on) and encoded
// (opusenc.h) into packets for the voice connection to send.
//
// Whether someone's talking is the input sensitivity: a level a frame must be
// louder than, either automatic (the room's noise floor plus 10 dB) or fixed.
// Talk only starts when two 20 ms frames in a row are over it, so a click or a
// button press (a few ms) never opens the mic, and the first of them goes out
// too, so no word is clipped (the sound is a frame, 20 ms, behind). It's held
// 300 ms after it drops. When talking stops, five frames of silence follow (as
// Discord wants), the last one marked so the voice connection can say it stopped.
// audin is only open while in a call, not muted.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MIC_PACKET_BYTES 160  // 64 kbps
#define MIC_SILENT 255

namespace mic {

void start();
void stop();

void setCall(bool on);    // the voice connection is up (it sends what's recorded)
void setMuted(bool on);   // muted or deafened: the mic is closed

// The settings (settings.ini, the overlay's Settings page): input volume (0..200 percent),
// sensitivity (0: automatic; else the level talk must be over, dBFS, e.g. -45), automatic gain on or off,
// noise suppression (RNNoise) on or off
#define MIC_SENSITIVITY_AUTO 0
void setSettings(int volume, int sensitivity_db, bool agc, bool denoise);

struct Packet {
    uint8_t data[MIC_PACKET_BYTES];
    uint8_t n;
    uint32_t frame;  // which 20 ms of the mic's it is (RTP's timestamp follows these, pauses and all)
    bool last;  // talking stopped with this one (one of the silence frames)
};
// The next packet to send, in order; false: none waiting
bool take(Packet* p);
bool active();  // recording (the voice connection looks for packets often while it is)

// For the lists: talking now, and how loud the mic is (-dBFS; MIC_SILENT: closed)
bool speaking();
uint8_t level();

// For the Settings page's meter: the mic's level as the sensitivity sees it (after the input volume,
// before the gain), the level talk must be over now, and the automatic gain (dB); false: the mic's closed
bool meter(int* level_db, int* threshold_db, int* gain_db);

struct Stats {
    uint32_t frames;    // 20 ms recorded
    uint32_t talking;   // of them, sent
    uint32_t dropped;   // packets the voice connection didn't take in time
    uint32_t open_failed;
    uint32_t last_rc;
    int floor_db;  // the room's noise (dBFS, before the gain)
    int gain_db;   // the automatic gain, as it is now
    int peak_db;   // the loudest frame sent (dBFS, after the gain)
    uint32_t busy_us;     // CPU time spent on the frames (noise suppression, filtering, gain, encoding)
    uint32_t denoise_us;  // of it, on noise suppression
    uint32_t lost;     // 20 ms frames audin never gave us (we were too slow: the timestamps skip them)
    uint32_t clicks;   // single loud frames that didn't open the mic
    bool open;
};
void takeStats(Stats* out);

// The talking detector, apart for the PC tests. One frame's level in; whether the frame BEFORE it
// is talk (a frame late, so talk can need two loud frames in a row and still send the first)
struct Detector {
    float floor_db = -60;
    float fixed_db = 0;  // 0: automatic (the floor + 10 dB, at least -60), else this
    int hold = 0;
    bool on = false;
    float last_db = -96;   // (the frame before: the one being decided)
    uint32_t clicks = 0;   // (a loud frame alone, not talk)
    float threshold() const;
    bool feed(float db);
};
float levelDb(const int16_t* s, size_t n);
// The input volume's percent as a factor: as the mixer's volumes, (v/100)^2 up to 100%, then on to x2 at 200%
float volumeGain(int percent);

// A 100 Hz high-pass (2nd-order Butterworth): handling rumble and thumps live below the voice
struct HighPass {
    float b0, b1, b2, a1, a2;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    HighPass();  // (100 Hz at 48 kHz)
    void run(float* x, int n);
};

// Automatic gain: only while talking, the mic is brought toward speech at TARGET dBFS: down quickly
// when it comes in too loud, up slowly when too quiet; held where it is in silence (the room's
// noise is never what it turns up). One 20 ms frame's level before the gain in, the gain out (dB)
#define MIC_AGC_TARGET -20.f
#define MIC_AGC_MAX 30.f
#define MIC_AGC_MIN -12.f  // (a hot mic can be turned down too)
struct Agc {
    float gain_db = 18;
    float feed(float raw_db, bool talk);
};

}  // namespace mic
