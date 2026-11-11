#include "screenclock.h"

uint64_t VideoClock::recorded(uint64_t ts_us, uint64_t now_ns) {
    constexpr int64_t DRIFT_NS = 500'000'000;
    int64_t t = (int64_t)(ts_us * 1000);
    int64_t mapped = t + offset_ns_;
    int64_t off = (int64_t)now_ns - mapped;
    if (!last_ts_ || mapped <= 0 || off > DRIFT_NS || off < -DRIFT_NS) {
        __atomic_store_n(&offset_ns_, (int64_t)now_ns - t, __ATOMIC_RELAXED);
        __atomic_store_n(&have_, true, __ATOMIC_RELEASE);
        mapped = (int64_t)now_ns;
    }
    return (uint64_t)mapped;
}

uint32_t VideoClock::missed(uint64_t ts_us, uint64_t* gap_us) {
    uint64_t gap = last_ts_ && ts_us > last_ts_ ? ts_us - last_ts_ : 0;
    last_ts_ = ts_us;
    *gap_us = gap;
    // (a frame's time is 16.7 or 33.3 ms: a step much shorter is the recorder catching up, not the frame
    // time, and taken as it, it made every normal step look like ten missed frames)
    if (gap >= 12000 && (!min_gap_ || gap < min_gap_)) min_gap_ = gap;
    if (gap && min_gap_ && gap > min_gap_ * 3 / 2) return (uint32_t)((gap + min_gap_ / 2) / min_gap_) - 1;
    return 0;
}

AudioClock::Place AudioClock::place(uint64_t ts_us, uint32_t samples) {
    Place p = {0, 0, false};
    if (samples == 1024 && last_ts_ && ts_us > last_ts_ && ts_us - last_ts_ >= 20000 && ts_us - last_ts_ <= 22700) us_ = true;
    last_ts_ = ts_us;
    if (us_ && ts_us && start_ts_ && ts_us > start_ts_) {
        uint64_t want = (ts_us - start_ts_) * (RATE / 1000) / 1000;  // (samples since the start, by the clock)
        if (want > pos_ + 512) {
            uint64_t hole = want - pos_;
            p.hole = true;
            if (hole <= FILL_MAX) p.silence = (uint32_t)hole;
            else p.skip = (uint32_t)hole;
            pos_ = want;
        } else if (want + RATE < pos_) {
            start_ts_ = 0;  // (the clock went back: begun again)
        }
    }
    if (ts_us && !start_ts_) start_ts_ = ts_us, pos_ = 0;
    pos_ += samples;
    return p;
}
