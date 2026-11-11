#include "share.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace share {

static uint8_t g_grey[H264_PLACEHOLDER_MAX + 1024];  // (room for DAVE's trailer)

void Video::say(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    sink_->log(line);
}

void Video::reset(uint64_t now_ms) {
    need_idr_ = false;
    last_frame_ms_ = now_ms;
    next_placeholder_ms_ = 0;
    last_key_n_ = 0, same_keys_ = 0, blank_ = false;
    stats_ = {};
}

void Video::takeStats(Stats* out) {
    *out = stats_;
    stats_ = {};
}

bool Video::step(uint64_t now_ms, uint64_t now_tick) {
    uint8_t* f;
    size_t n, cap;
    uint64_t tick;
    bool gap = false;
    if (!frames_->take(&f, &n, &cap, &tick, &gap)) {
        if (now_ms - last_frame_ms_ >= SHARE_IDLE_MS) {
            // (keyframes either side of a time with no frames aren't back to back: the count starts again)
            same_keys_ = 0, last_key_n_ = 0;
            placeholder(now_ms, now_tick, false);
        }
        return false;
    }
    last_frame_ms_ = now_ms;
    bool idr = false;
    params_.full_range = frames_->fullRange() ? 1 : -1;  // (full when asked for, else as grc labels it)
    n = rtpH264WithParams(f, n, cap - 1024, &params_, &idr);

    // The recorder's blank picture (out of the game): the same keyframe again and again, back to back,
    // nothing between. A game whose picture only holds still (a pause menu, a dialogue box) sends
    // keyframes alike too, but with ordinary frames between them: counted alike only one right after
    // another. Out of it at the first keyframe that differs, or the first ordinary frame
    if (idr) {
        same_keys_ = n == last_key_n_ ? same_keys_ + 1 : 0;
        last_key_n_ = n;
    } else {
        same_keys_ = 0, last_key_n_ = 0;
    }
    if (!blank_ && same_keys_ >= 2) {
        blank_ = true;
        say("screen: the recorder's sending a still, blank picture (out of the game?): the loading screen instead (%u-byte keyframes)",
            (unsigned)n);
    } else if (blank_ && same_keys_ == 0) {
        blank_ = false;
        need_idr_ = true;  // (an ordinary frame first: it refers to pictures the viewers never got)
        say("screen: the game's picture is back");
    }
    if (blank_) {
        frames_->done();
        placeholder(now_ms, now_tick, true);
        return true;
    }

    next_placeholder_ms_ = 0;  // (the next time without the game's picture: a new pick, at once)
    if (gap) need_idr_ = true;
    // Too late to be worth sending: skipped, and the ones after wait for a keyframe (a keyframe goes even
    // late: it starts the picture over, and skipping it too could leave viewers frozen until the next)
    if (!idr && stale_ticks_ && now_tick > tick && now_tick - tick > stale_ticks_) {
        stats_.stale++;
        need_idr_ = true;
        frames_->done();
        return true;
    }
    if (need_idr_ && !idr) {
        stats_.waited++;
        frames_->done();
        return true;
    }
    need_idr_ = false;
    if (sink_->send(f, n, cap, tick)) {
        stats_.sent++;
        if (idr) stats_.keyframes++;
    } else {
        stats_.failed++;
        need_idr_ = true;  // (only partly sent: the frames after it break as after a lost one)
    }
    frames_->done();
    return true;
}

// Nothing of the game's to show: a loading screen (or grey), again every 2 s for viewers who come later.
// flush: the recorder's frames queued meanwhile are thrown away (its blank picture), so the queue's free.
// After it, the recorder's frames wait for its next keyframe
void Video::placeholder(uint64_t now_ms, uint64_t now_tick, bool flush) {
    if (now_ms < next_placeholder_ms_) return;
    if (!next_placeholder_ms_) {  // (a new time without the game's picture: a new pick)
        const char* why = nullptr;
        if (loading_->pick(pick_, sizeof(pick_), &why)) say("screen: nothing to show: loading screen %s", pick_);
        else pick_[0] = 0, say("screen: nothing to show, and %s: grey", why ? why : "no loading screens");
    }
    bool sent = sendLoading(now_tick, flush);
    if (!sent) {
        grey_params_.full_range = colorRange();  // (labelled as the game's video is)
        size_t g = h264Placeholder(&grey_params_, grey_id_++, g_grey, H264_PLACEHOLDER_MAX);
        sent = g && sink_->send(g_grey, g, sizeof(g_grey), now_tick);
    }
    if (sent) stats_.placeholders++;
    need_idr_ = true;
    next_placeholder_ms_ = now_ms + SHARE_PLACEHOLDER_MS;
}

// This time's loading screen, read into the (empty) queue and sent from there; false: none, or it
// couldn't be (too big, unreadable, not a keyframe, frames came meanwhile)
bool Video::sendLoading(uint64_t now_tick, bool flush) {
    if (!pick_[0]) return false;
    size_t cap = 0;
    uint8_t* buf = frames_->borrow(&cap, flush);
    if (!buf) return false;  // (frames came meanwhile: the game's back)
    const char* why = nullptr;
    bool sent = false;
    long got = loading_->read(pick_, buf, SHARE_LOADING_MAX + 1 < cap ? SHARE_LOADING_MAX + 1 : cap);
    if (got < 0) {
        why = "couldn't be read";
    } else if (got <= 4) {
        why = "is empty";
    } else if ((size_t)got > SHARE_LOADING_MAX) {
        why = "is over 200 KB";
    } else {
        // (its SPS made as WebRTC wants it, as the recorder's are; its own ids (1), kept apart from those)
        bool idr = false;
        size_t n = rtpH264WithParams(buf, (size_t)got, cap - 1024, &loading_params_, &idr);
        if (!idr) why = "isn't a keyframe";
        else sent = sink_->send(buf, n, cap, now_tick);
    }
    frames_->giveBack();
    if (why) {
        say("screen: loading screen %s %s: grey instead", pick_, why);
        pick_[0] = 0;  // (grey from now on, this time)
    }
    return sent;
}

}  // namespace share
