// What the screen share sends, frame by frame (the Go Live connection, voice.cpp, does the sending):
//  - the recorder's frames, each keyframe with the SPS and PPS a decoder needs (rtp.h);
//  - after a frame was lost (the recorder's we never read, one with no room to queue, one only partly
//    sent), the frames after it wait for the next keyframe: they refer to pictures viewers never got;
//  - with no frames for a while (no game, the HOME menu), or the recorder's blank picture (the same
//    keyframe again and again, nothing between), a loading screen (CHIRP_LOADING), else plain grey, again
//    every 2 s for viewers who come later. The game's picture is back at its next keyframe.
// Nothing here knows about the Switch or the network: test/unit_test.cpp tests it with stand-ins.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "rtp.h"

namespace share {

// Where frames come from (screen.h)
struct Frames {
    // The next frame (Annex-B, in cap bytes it may grow into), when recorded (system ticks), whether
    // frames before it were lost; false: none ready
    virtual bool take(uint8_t** f, size_t* n, size_t* cap, uint64_t* tick, bool* gap) = 0;
    virtual void done() = 0;
    // The queue's memory while it's empty (a loading screen is read into it); flush: what's queued is
    // thrown away first. Null: frames are waiting
    virtual uint8_t* borrow(size_t* cap, bool flush) = 0;
    virtual void giveBack() = 0;
    virtual bool fullRange() = 0;  // (the video labelled full range, whatever the recorder says: screen.h)
};

// The loading screens: H.264 keyframes on the card
struct Loading {
    // One at random into name (cap bytes); false: none there (*why says why, for the log)
    virtual bool pick(char* name, size_t cap, const char** why) = 0;
    // Its bytes into buf (at most cap); how many, or -1 if it couldn't be read
    virtual long read(const char* name, uint8_t* buf, size_t cap) = 0;
};

// Where frames go: DAVE, RTP, the socket. False: it didn't all get out
struct Sink {
    virtual bool send(uint8_t* f, size_t n, size_t cap, uint64_t tick) = 0;
    virtual void log(const char* line) = 0;
};

#define SHARE_IDLE_MS 700         // no frames this long: a placeholder
#define SHARE_PLACEHOLDER_MS 2000 // and again this often, for viewers who come later
#define SHARE_LOADING_MAX (200 * 1024)

class Video {
public:
    Video(Frames* frames, Loading* loading, Sink* sink) : frames_(frames), loading_(loading), sink_(sink) {}
    void reset(uint64_t now_ms);  // (a new connection)
    // Frames older than this when their turn comes (system ticks since recorded; 0: never) aren't sent:
    // the ones after wait for the next keyframe. Late frames sent anyway made viewers' players freeze,
    // then rush through them to catch up; skipped, the picture jumps to now at the next keyframe
    void setStale(uint64_t ticks) { stale_ticks_ = ticks; }
    // The next frame sent (or a placeholder, when it's time for one): true if one was taken, and another
    // may be waiting. now_tick: the system's ticks now (a placeholder's recorded time)
    bool step(uint64_t now_ms, uint64_t now_tick);

    struct Stats {
        uint32_t sent, keyframes, waited, failed, placeholders, stale;
    };
    void takeStats(Stats* out);
    // The range the video goes out labelled with: full when that's asked for (fullRange()), else as the
    // recorder's SPS kept says (grc's own until one comes): 0 limited, 1 full, -1 it doesn't say
    int colorRange() const { return frames_->fullRange() ? 1 : h264SpsRange(params_.sps, params_.sps_n); }

private:
    void placeholder(uint64_t now_ms, uint64_t now_tick, bool flush);
    bool sendLoading(uint64_t now_tick, bool flush);
    void say(const char* fmt, ...);

    Frames* frames_;
    Loading* loading_;
    Sink* sink_;
    H264Params params_;       // (the recorder's: keyframes carry the SPS and PPS)
    H264Params grey_params_;  // (grey's: the recorder's SPS, as the frames it stands in for)
    H264Params loading_params_;  // (the loading screens' own, ids 1)
    bool need_idr_ = false;
    uint64_t stale_ticks_ = 0;
    uint64_t last_frame_ms_ = 0, next_placeholder_ms_ = 0;
    int grey_id_ = 0;
    char pick_[160] = {0};  // (this time's loading screen: picked when the game's picture goes)
    // the recorder's blank picture: keyframes alike, back to back
    size_t last_key_n_ = 0;
    int same_keys_ = 0;
    bool blank_ = false;
    Stats stats_ = {};
};

}  // namespace share
