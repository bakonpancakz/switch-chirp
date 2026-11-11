// The recorder's clocks (grc:d's timestamps, microseconds), for the screen's capture (screen.cpp).
// Nothing here knows about the Switch: test/unit_test.cpp tests it on a PC.
#pragma once
#include <stdint.h>

// Video: when each frame was recorded, on our clock, and how many the recorder made that we never read
class VideoClock {
public:
    // (a pause in sharing isn't frames missed, and 30 or 60 a second may change meanwhile)
    void reset() { last_ts_ = min_gap_ = 0; }

    // The frame's recorded time moved onto our clock (ns): viewers' players pace by these, and when a
    // frame was read instead varies with how long grc and the queue kept it. Set again from now_ns when
    // it's the first, or has drifted more than half a second
    uint64_t recorded(uint64_t ts_us, uint64_t now_ns);

    // Frames missed before this one: the timestamps step by a frame's time (the smallest step seen), a
    // bigger step is frames we never read. *gap_us: the step (0: the first, or none). Call after recorded()
    uint32_t missed(uint64_t ts_us, uint64_t* gap_us);

    // Any recorder time (us) on our clock (ns), by the offset recorded() set last; false before it has
    // set one. (The sound's timestamps are the same clock's: so the sound is lined up with the video.
    // Read from the sound's thread: atomic)
    bool toLocal(uint64_t ts_us, uint64_t* ns) const {
        if (!__atomic_load_n(&have_, __ATOMIC_ACQUIRE)) return false;
        *ns = (uint64_t)((int64_t)(ts_us * 1000) + __atomic_load_n(&offset_ns_, __ATOMIC_RELAXED));
        return true;
    }

private:
    uint64_t last_ts_ = 0, min_gap_ = 0;
    int64_t offset_ns_ = 0;
    bool have_ = false;
};

// The game's sound: where each 1024-frame chunk (48 kHz) belongs by the recorder's clock. A chunk read
// too late leaves a hole: a short one is filled with silence, so the packets keep their place and the
// viewers' players don't drift; a long one (the HOME menu) moves the packets on instead. (Counting
// chunks alone made a missed one look like the sound arriving slowly: players stretched it, then rushed)
class AudioClock {
public:
    static constexpr uint32_t RATE = 48000;
    static constexpr uint32_t FILL_MAX = RATE / 5;  // (holes up to 200 ms are filled)

    void reset() { start_ts_ = 0; }  // (sharing stopped: the next chunk begins again)

    struct Place {
        uint32_t silence;  // samples of silence to put before the chunk
        uint32_t skip;     // or (a long hole) samples to move the packets on by
        bool hole;
    };
    // The chunk at ts_us, `samples` long: what goes before it
    Place place(uint64_t ts_us, uint32_t samples);

private:
    uint64_t start_ts_ = 0, pos_ = 0;  // (the recorder's time the sound began, and samples since)
    uint64_t last_ts_ = 0;
    bool us_ = false;  // (seen one chunk after another 21.3 ms apart: the timestamps are microseconds)
};
