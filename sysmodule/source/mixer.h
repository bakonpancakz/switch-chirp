// Everyone's sound into one, every 20 ms, out through audout (beside
// whatever else plays: the game, Melody). Each source is asked for its next
// 20 ms of mono into one shared scratch buffer, scaled by its gain and added
// into an int32 sum; the sum is clamped once, at the end. Nothing is kept
// per source but what it needs to make its next frame.
//
// For now the sources are the sound check's test tones; the people in a
// call come later (each a jitter buffer of Opus packets and a decoder).
// audout is only open while something plays.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MIXER_RATE 48000
#define MIXER_FRAME 960  // 20 ms
#define MIXER_SOURCES 8

namespace mixer {

void start();
void stop();

void setSoundCheck(bool on);
bool soundCheck();
void setDeaf(bool on);  // nothing comes out (the sources still run)

// The call: speakers' Opus packets (decrypted), played through a small jitter buffer each
#define MIXER_SPEAKERS 8  // (people followed at once: heard in the last 5 s)
#define MIXER_SILENT 255
void setVoice(bool on);  // (audout is open while this, or the sound check, is on)
void voicePacket(uint64_t user, uint16_t seq, const uint8_t* opus, size_t n);
// Gains are Q12: MIXER_UNITY is x1
#define MIXER_UNITY 4096
// A volume slider's percent (0..200) as a gain: a curve, as loudness is heard. Up to 100% the
// amplitude is (v/100)^2 (50%: -12 dB, about half as loud; plain linear 50% is only -6 dB and
// sounds hardly quieter); above, linear to x2 at 200% (+6 dB, as Discord's boost)
int gainQ12(int percent);
// Each speaker's gain, asked for every frame (Q12; 0 for muted)
void setGainSource(int (*gain)(uint64_t user));
// Everything's volume (Q12): the call and the sound check
void setMaster(int gain);
// The jitter buffers' cushion, in 20 ms packets (MIXER_START_AT normally; more while sharing the screen,
// when the Wi-Fi's busy sending video and packets come in clumps)
void setCushion(int packets);
// For the lists: whether someone is speaking now, and how loud (-dBFS; MIXER_SILENT: nothing)
bool speaker(uint64_t user, bool* speaking, uint8_t* level);
// For the overlay's Debug: the people being followed now (heard in the last few seconds), and the
// hardware decoders lent out; and how many there can be of each
void busy(int* speakers, int* decoders, int* speakers_max, int* decoders_max);

// What the call's sound went through since the last time this was asked (for the log)
struct Stats {
    uint32_t packets;                // handed in by the voice connection
    uint32_t dec_opened, dec_failed;  // hardware decoders lent (opened), and failures to open one
    uint32_t decoded, decode_failed;  // packets decoded, and that hwopus turned down
    uint32_t mixed;                   // 20 ms frames that had someone's voice in them
    uint32_t last_rc;                 // the last failure's result code (0: none)
    uint8_t loudest;                  // -dBFS of the loudest frame decoded (MIXER_SILENT: none)
    int last_gain;                    // the last gain a voice was mixed at (percent; -1: none)
    uint32_t gaps;                    // packets missing while someone talked (lost: skipped over)
    uint32_t late;                    // packets that came after their turn had played (dropped)
    uint32_t waited;                  // 20 ms frames held for a packet that came later (the voice thread or network was slow)
    uint32_t trimmed;                 // packets skipped to catch up when too far behind (the lag that waiting added)
    uint32_t dry;                     // times audout had played everything we'd given it (we were late: a click)
    bool deaf, out_open;
};
void takeStats(Stats* out);

// One speaker's jitter buffer (apart for the PC tests). Playing starts once START_AT packets
// are in; then each 20 ms takes the next. One that isn't here yet is waited for (silence),
// which adds lag, so when it gets more than TRIM_AT ahead it skips back to START_AT in one go
// (a single skip, rather than lag that only grows until the buffer overflows).
#define MIXER_JITTER 8        // packets kept (160 ms: twice what TRIM_AT lets build up; a power of two, so seq % it survives the u16 wrap)
#define MIXER_PACKET_MAX 1275  // (Opus's most for a frame, RFC 6716: Discord's 384 kbps channels are ~960 bytes a packet, more at peaks)
#define MIXER_START_AT 1      // 20 ms of cushion: Horizon may keep the voice thread waiting
#define MIXER_TRIM_AT 4       // 80 ms buffered: catch up
struct Jitter {
    struct Slot {
        alignas(16) uint8_t data[MIXER_PACKET_MAX];  // (first, on a 16-byte line: copied in and out in whole chunks)
        uint16_t seq, n;
        bool used;
    };
    Slot slots[MIXER_JITTER];
    int start_at = MIXER_START_AT, trim_at = MIXER_TRIM_AT;  // (the cushion: the mixer's, set for everyone)
    bool playing;
    uint16_t next;  // the sequence number to play next
    int idle;       // frames in a row with nothing to play

    void push(uint16_t seq, const uint8_t* opus, size_t n, Stats* st);
    // This 20 ms's packet (valid until the next push), or null: silence (after 10 of those
    // in a row it stops playing, and starts again from START_AT)
    const Slot* pop(Stats* st);
};

// The mixing itself, apart for the PC tests: n sources' mono frames, each
// with its gain (Q12, MIXER_UNITY is x1), into interleaved stereo s16
void mix(const int16_t* const* frames, const int* gains, size_t n, int16_t* out);

}  // namespace mixer
