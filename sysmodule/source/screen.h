// Screen sharing: the game's video as the system records it (grc:d, the
// service SysDVR uses), already H.264, a frame at a time: 1280x720, 30 a second.
// Its own thread reads a frame into the one buffer there is, and hands it to the
// voice connection (which owns the DAVE and RTP state: it encrypts, packetizes
// and sends it), then reads the next once that's done. Only while wanted (the
// overlay's Share Screen) and in a call whose voice server gave a video stream.
// With Go Live, the stream's own connection (voice.h) sends it, and the game's
// sound too: read on its own thread, mixed to mono, encoded to Opus here.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SCREEN_FRAME_MAX 0x54000  // (SysDVR's: an IDR frame can outgrow the system's own 0x32000, and fail)
#define SCREEN_HEADROOM 0x2000    // (DAVE's growth: 4-byte start codes, the trailer)
#define SCREEN_RING 0x60000       // (frames are queued here for sending: capture never waits for a keyframe to get out. 384 KB: a keyframe at 10 Mbps, ~200 KB, and the frames after it)
#define SCREEN_AUDIO_BYTES 240    // the game's sound: Opus, 20 ms in 240 bytes (96 kbps, mono)

namespace screen {

bool start();  // (false: its threads couldn't be made)
void stop();

void setWanted(bool on);  // the overlay's toggle
// The video's colour range as labelled for viewers: grc labels it limited (16..235); full (0..255) if its
// pictures really are (blacks crushed and whites clipped otherwise). Takes effect at the next keyframe
void setFullRange(bool on);
bool fullRange();
bool wanted();
void setCall(bool on);    // the voice connection can send video
bool active();            // capturing

// The next frame, if one's ready: Annex-B H.264 in a buffer of *cap bytes (it may be changed in place),
// and when it was captured (system ticks). *gap: frames before it were dropped (the recorder's we read
// too late, or ones with no room to queue): the ones after refer to pictures the viewers never got, so
// they wait for the next keyframe. done() hands it back
bool take(uint8_t** frame, size_t* n, size_t* cap, uint64_t* tick, bool* gap);
void done();

// The game's sound as grc:d records it (48 kHz stereo), mixed to mono and encoded (opusenc.h): the
// next packet, in order; false: none waiting
struct AudioPacket {
    uint8_t data[SCREEN_AUDIO_BYTES];
    uint16_t n;
    uint32_t frame;  // which 20 ms it is (the RTP timestamp follows these)
};
bool takeAudio(AudioPacket* p);

// The send queue's memory, lent while it's empty (a loading screen is read into it and sent from it):
// null if frames are waiting (flush: they're thrown away first, as when the recorder's sending only a
// blank picture). Capture waits meanwhile. returnRing() when done, from the same thread
uint8_t* borrowRing(size_t* cap, bool flush = false);
void returnRing();

struct Stats {
    uint32_t captured, failed;  // frames read; grc:d calls that failed
    uint32_t last_rc;
    uint32_t audio_dropped;  // game sound packets not taken in time
    uint32_t audio_holes;    // gaps in the game's sound (chunks read too late), by its timestamps
    uint32_t skipped;        // frames the recorder made that we never read (its timestamps jumped)
    uint32_t no_room;        // frames read but dropped: the queue was full (the network's slower than the video)
    uint32_t waited_ms;      // capture waiting for the previous frame to be sent
    uint64_t gap_min, gap_max;  // the recorder's timestamps between frames read (its units)
    // grc:d's transfers (each waits until the recorder has something): the longest one this time, and
    // how long the one going on now has been waiting (0: none), video's and sound's; sound chunks read
    uint32_t video_longest_ms, video_waiting_ms, audio_longest_ms, audio_waiting_ms;
    uint32_t audio_chunks;
    bool no_game;  // (no game running now: video isn't asked for until one is)
    bool open;
};
void takeStats(Stats* out);

}  // namespace screen
