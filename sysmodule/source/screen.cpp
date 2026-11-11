#include "screen.h"

#include <switch.h>
#include <string.h>

#include "framequeue.h"
#include "opusenc.h"
#include "screenclock.h"

namespace screen {
namespace {

// grc:d's rules (SysDVR learned most of them the hard way; the last, Chirp):
// - Begin (cmd 1) starts recording for everyone, and may be called only ONCE: a second call, from any
//   session, makes grc assert and the Switch crash. So it's called only when a transfer says it hasn't
//   been (0x3E8D4), and never again while Chirp runs; grc remembers it across Chirp's restarts.
// - Transfer (cmd 2) waits until there's something: forever when nothing records.
// - Video needs 0x54000: an IDR frame can be bigger than the 0x32000 the system uses, and fails 2212-0006.
// - Audio comes in 0x1000 chunks (48 kHz, stereo, s16), on its own session, as SysDVR reads it.
// - A video transfer asked for while nothing records is never answered, not even once something does:
//   capture stayed stuck in it, the stream on its loading screen, until the Switch was restarted
//   (restarting Chirp didn't free it). The one seen: asked for a second after the first title since the
//   boot opened. So video is asked for only while a title runs (any title: a game, or sphaira and what
//   runs in it), and not in its first seconds.
//   (Not by its control data's VideoCapture setting: a title that says no may record all the same, as
//   with an installer's patch (DBI's), and the setting was read late: capture ran, then stopped for good)
constexpr Result NOT_BEGUN = 0x3E8D4;
constexpr u64 QUEUE_WAIT_MS = 300;  // (a full queue: how long a frame waits for room before it's dropped)
constexpr u64 TITLE_SETTLE_MS = 2000;   // (a title just opened: this long before video's asked for)

alignas(0x1000) uint8_t g_buf[SCREEN_FRAME_MAX + SCREEN_HEADROOM];
alignas(0x1000) uint8_t g_abuf[0x1000];
uint8_t g_ring[SCREEN_RING];
FrameQueue g_q;
Mutex g_ring_lock;              // (capture's while it copies a frame in; borrowRing()'s while it's lent)
volatile bool g_flush = false;  // (sharing stopped: what's queued is stale, the reader drops it)
bool g_gap = false;             // (capture's: a frame's been dropped since the last one queued)

Thread g_vthread, g_athread;
volatile bool g_run = false, g_wanted = false, g_call = false, g_full_range = false;
Service g_vsrv, g_asrv;
bool g_vopen = false, g_aopen = false;
bool g_begun = false;  // (once: see above)
Mutex g_begin_lock;

Mutex g_lock;  // (the stats)
Stats g_stats;
volatile uint64_t g_vwait_since = 0, g_await_since = 0;  // (when the transfer going on now began: ticks, 0 none)
volatile bool g_no_game = false;
// the title running, as app.cpp says (g_title_lock)
Mutex g_title_lock;
uint64_t g_title = 0, g_title_since = 0;  // (since: when it was first said, ticks)

// Whether video may be asked for now (see grc:d's rules above)
bool titleRecords() {
    mutexLock(&g_title_lock);
    uint64_t title = g_title, since = g_title_since;
    mutexUnlock(&g_title_lock);
    return title && armTicksToNs(armGetSystemTick() - since) / 1000000 >= TITLE_SETTLE_MS;
}

VideoClock g_vclock;
AudioClock g_aclock;

// the game's sound: 20 ms gathered, encoded, queued for the stream's connection (one writer, one reader)
constexpr int AQ = 16;  // (320 ms: room while a keyframe goes out)
AudioPacket g_aq[AQ];
uint32_t g_aq_head = 0, g_aq_tail = 0;  // (atomic: the audio thread writes at head, the stream reads at tail)
OpusEnc g_enc;
int16_t g_pcm[OPUSENC_FRAME];
int g_pcm_n = 0;
uint32_t g_aframe = 0;  // (which 20 ms the next packet is)

uint64_t msSince(uint64_t tick) { return armTicksToNs(armGetSystemTick() - tick) / 1000000; }

template <class Fn>
void stats(Fn fn) {
    mutexLock(&g_lock);
    fn(g_stats);
    mutexUnlock(&g_lock);
}

bool sharing() { return g_wanted && g_call; }

// --- grc:d ---

Result transfer(Service* srv, GrcStream stream, void* buf, size_t size, u32* data_size, u64* ts) {
    struct {
        u32 num_frames, data_size;
        u64 start_timestamp;
    } out = {};
    u32 in = stream;
    Result rc = serviceDispatchInOut(srv, 2, in, out, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                     .buffers = {{buf, size}}, );
    *data_size = out.data_size;
    *ts = out.start_timestamp;
    return rc;
}

// A transfer; if grc says recording hasn't begun, Begin (the one time) and try again. Timed: the
// longest in the stats, and while it waits takeStats() says for how long
Result read(Service* srv, GrcStream stream, void* buf, size_t size, u32* data_size, u64* ts, volatile uint64_t* since,
            uint32_t Stats::*longest) {
    uint64_t t0 = armGetSystemTick();
    *since = t0;
    Result rc = transfer(srv, stream, buf, size, data_size, ts);
    if (rc == NOT_BEGUN) {
        mutexLock(&g_begin_lock);
        bool begin = !g_begun;
        g_begun = true;  // (set first: even a Begin that fails isn't tried again)
        Result brc = begin ? serviceDispatch(&g_vsrv, 1) : 0;
        mutexUnlock(&g_begin_lock);
        rc = R_FAILED(brc) ? brc : transfer(srv, stream, buf, size, data_size, ts);
    }
    *since = 0;
    uint32_t ms = (uint32_t)msSince(t0);
    stats([&](Stats& s) {
        if (ms > s.*longest) s.*longest = ms;
    });
    return rc;
}

void failed(Result rc) {
    stats([&](Stats& s) {
        s.failed++;
        s.last_rc = rc;
    });
}

// --- video ---

// Into the queue. Full: a wait, as grc keeps its next frames a while (waits of 100 ms lost nothing at
// 60 fps); only after 300 ms is it dropped instead (marked: the frames after it wait for a keyframe).
// (Dropped after 5 ms, each drop cost a quarter second of frames waiting for a keyframe: 44 fps, not 60.)
// A frame too big for the ring is queued from g_buf itself, capture waiting until it's sent
void queue(u32 size, uint64_t tick) {
    size_t need = size + SCREEN_HEADROOM;
    uint64_t w0 = armGetSystemTick();
    if (need > SCREEN_RING) {
        while (g_run && sharing() && !g_q.empty()) svcSleepThread(1'000'000LL);
        if (g_run && sharing()) {
            g_q.push(g_buf, size, sizeof(g_buf), tick, g_gap);
            g_gap = false;
            while (g_run && sharing() && !g_q.empty()) svcSleepThread(1'000'000LL);  // (g_buf is in use until then)
        }
    } else {
        bool queued = false;
        while (g_run && sharing()) {
            mutexLock(&g_ring_lock);
            if (uint8_t* at = g_q.room(need)) {
                memcpy(at, g_buf, size);
                g_q.push(at, size, need, tick, g_gap);
                g_gap = false;
                queued = true;
            }
            mutexUnlock(&g_ring_lock);
            if (queued) break;
            if (msSince(w0) >= QUEUE_WAIT_MS) {
                g_gap = true;
                stats([](Stats& s) { s.no_room++; });
                break;
            }
            svcSleepThread(1'000'000LL);
        }
    }
    uint32_t waited = (uint32_t)msSince(w0);
    stats([&](Stats& s) { s.waited_ms += waited; });
}

void videoThread(void*) {
    while (g_run) {
        if (!sharing()) {
            g_vclock.reset();
            if (!g_q.empty()) g_flush = true;
            svcSleepThread(100'000'000LL);
            continue;
        }
        if (!titleRecords()) {  // (nothing that records: see grc:d's rules above)
            g_no_game = true;
            g_vclock.reset();
            svcSleepThread(250'000'000LL);
            continue;
        }
        g_no_game = false;
        if (!g_vopen) {  // (opened once, and kept: a transfer may still be waiting on it)
            Result rc = smGetService(&g_vsrv, "grc:d");
            if (R_FAILED(rc)) {
                failed(rc);
                svcSleepThread(2'000'000'000LL);
                continue;
            }
            g_vopen = true;
        }
        u32 size = 0;
        u64 ts = 0;
        Result rc = read(&g_vsrv, GrcStream_Video, g_buf, SCREEN_FRAME_MAX, &size, &ts, &g_vwait_since, &Stats::video_longest_ms);
        if (R_FAILED(rc) || size <= 4 || size > SCREEN_FRAME_MAX) {
            failed(R_FAILED(rc) ? rc : 1);
            svcSleepThread(100'000'000LL);  // (and try again: never Begin again, never reopen)
            continue;
        }
        uint64_t now_ns = armTicksToNs(armGetSystemTick());
        uint64_t tick = ts ? armNsToTicks(g_vclock.recorded(ts, now_ns)) : armGetSystemTick();
        uint64_t gap = 0;
        uint32_t missed = g_vclock.missed(ts, &gap);
        // (counted, not taken as lost: a step of 2-3 frames' time is grc not encoding a picture that didn't
        // change, and the stream is whole. Taken as lost, the frames after waited for a keyframe: up to 938
        // of 1553 in 30 s thrown away while capture never waited, and the stream stuttered all the time)
        stats([&](Stats& s) {
            s.captured++;
            s.skipped += missed;
            if (gap && (!s.gap_min || gap < s.gap_min)) s.gap_min = gap;
            if (gap > s.gap_max) s.gap_max = gap;
        });
        queue(size, tick);
    }
}

// --- the game's sound ---

uint64_t g_pkt_ns = 0;  // (when the packet being gathered began, on the video's clock: 0, not known)

// Stereo s16 frames (null: silence), mixed to mono, into 20 ms Opus packets. first_ns: when the first
// of them was recorded, on the video's clock (0: not known), so each packet says when it begins
void audioSamples(const int16_t* stereo, size_t frames, uint64_t first_ns) {
    for (size_t i = 0; i < frames; i++) {
        if (g_pcm_n == 0) g_pkt_ns = first_ns ? first_ns + i * 1'000'000'000ULL / AudioClock::RATE : 0;
        g_pcm[g_pcm_n++] = stereo ? (int16_t)(((int)stereo[2 * i] + stereo[2 * i + 1]) / 2) : 0;
        if (g_pcm_n < OPUSENC_FRAME) continue;
        g_pcm_n = 0;
        uint32_t head = __atomic_load_n(&g_aq_head, __ATOMIC_RELAXED);
        uint32_t tail = __atomic_load_n(&g_aq_tail, __ATOMIC_ACQUIRE);
        uint32_t frame = g_aframe++;
        if (head - tail >= AQ) {  // (not taken: this one's dropped; the timestamps skip it)
            stats([](Stats& s) { s.audio_dropped++; });
            continue;
        }
        AudioPacket& q = g_aq[head % AQ];
        opusencFrame(&g_enc, g_pcm, q.data, SCREEN_AUDIO_BYTES);
        q.n = SCREEN_AUDIO_BYTES;
        q.frame = frame;
        q.ns = g_pkt_ns;
        __atomic_store_n(&g_aq_head, head + 1, __ATOMIC_RELEASE);
    }
}

void audioThread(void*) {
    while (g_run) {
        if (!sharing() || !g_vopen) {  // (Begin goes through video's session: it's opened first)
            svcSleepThread(100'000'000LL);
            continue;
        }
        if (!g_aopen) {
            if (R_FAILED(smGetService(&g_asrv, "grc:d"))) {
                svcSleepThread(2'000'000'000LL);
                continue;
            }
            g_aopen = true;
        }
        u32 size = 0;
        u64 ts = 0;
        if (R_FAILED(read(&g_asrv, GrcStream_Audio, g_abuf, sizeof(g_abuf), &size, &ts, &g_await_since, &Stats::audio_longest_ms))) {
            svcSleepThread(100'000'000LL);
            continue;
        }
        if (size > sizeof(g_abuf)) size = sizeof(g_abuf);
        if (!sharing()) {
            g_aclock.reset();
            continue;
        }
        uint32_t frames = size / 4;
        AudioClock::Place at = g_aclock.place(ts, frames);
        if (at.hole) stats([](Stats& s) { s.audio_holes++; });
        // (when the chunk was recorded, on the video's clock: what lines the sound up with the pictures)
        uint64_t chunk_ns = 0;
        if (!ts || !g_vclock.toLocal(ts, &chunk_ns)) chunk_ns = 0;
        if (at.silence)
            audioSamples(nullptr, at.silence,
                         chunk_ns ? chunk_ns - (uint64_t)at.silence * 1'000'000'000ULL / AudioClock::RATE : 0);
        if (at.skip) {
            g_pcm_n = 0;  // (what was gathered goes; the next packet starts after the hole)
            g_aframe += at.skip / OPUSENC_FRAME + 1;
        }
        audioSamples((const int16_t*)g_abuf, frames, chunk_ns);
        stats([](Stats& s) { s.audio_chunks++; });
    }
}

}  // namespace

bool start() {
    mutexInit(&g_lock);
    mutexInit(&g_begin_lock);
    mutexInit(&g_ring_lock);
    mutexInit(&g_title_lock);
    g_q.init(g_ring, sizeof(g_ring));
    g_run = true;
    // (capture high: it does little but must read each frame while grc still has it)
    if (R_FAILED(threadCreate(&g_vthread, videoThread, nullptr, nullptr, 0x2000, 0x27, -2))) {
        g_run = false;
        return false;
    }
    opusencInit(&g_enc);
    // (the sound's thread encodes too: the encoder's stack, as the mic's; above the stream's connection:
    // a chunk read late is a chunk lost, as video's)
    if (R_FAILED(threadCreate(&g_athread, audioThread, nullptr, nullptr, 0x6000, 0x27, -2))) {
        g_run = false;
        threadClose(&g_vthread);
        return false;
    }
    threadStart(&g_vthread);
    threadStart(&g_athread);
    return true;
}

void stop() {
    if (!g_run) return;
    g_run = false;
    // (a transfer waiting for a frame returns with the next one; with no game it waits on: the
    // sysmodule only stops when the Switch does)
    threadWaitForExit(&g_vthread);
    threadWaitForExit(&g_athread);
    threadClose(&g_vthread);
    threadClose(&g_athread);
    if (g_vopen) serviceClose(&g_vsrv);
    if (g_aopen) serviceClose(&g_asrv);
    g_vopen = g_aopen = false;
}

void setWanted(bool on) { g_wanted = on; }
bool wanted() { return g_wanted; }
void setFullRange(bool on) { g_full_range = on; }
bool fullRange() { return g_full_range; }
void setCall(bool on) { g_call = on; }

void setTitle(uint64_t title) {
    mutexLock(&g_title_lock);
    if (title != g_title) g_title = title, g_title_since = armGetSystemTick();
    mutexUnlock(&g_title_lock);
}
// (sharing, whether or not grc:d's open yet: before the first game since Chirp started it isn't, and
// the stream wasn't announced at all then, so viewers got nothing, not even the loading screen)
bool active() { return sharing(); }

bool take(uint8_t** frame, size_t* n, size_t* cap, uint64_t* tick, bool* gap) {
    if (g_flush && !g_q.holding()) {
        g_flush = false;
        g_q.drop();
        return false;
    }
    FrameQueue::Frame f;
    if (!g_q.take(&f)) return false;
    *frame = f.p, *n = f.n, *cap = f.cap, *tick = f.tick, *gap = f.gap;
    return true;
}

void done() { g_q.done(); }

bool takeAudio(AudioPacket* p) {
    uint32_t tail = __atomic_load_n(&g_aq_tail, __ATOMIC_RELAXED);
    if (__atomic_load_n(&g_aq_head, __ATOMIC_ACQUIRE) == tail) return false;
    *p = g_aq[tail % AQ];
    __atomic_store_n(&g_aq_tail, tail + 1, __ATOMIC_RELEASE);
    return true;
}

uint8_t* borrowRing(size_t* cap, bool flush, bool wait) {
    if (!wait) {
        if (!mutexTryLock(&g_ring_lock)) return nullptr;
    } else {
        mutexLock(&g_ring_lock);
    }
    // (capture only queues holding this lock: emptied here, it stays empty until it's given back)
    if (flush) g_q.drop();
    if (g_q.holding() || !g_q.empty()) {
        mutexUnlock(&g_ring_lock);
        return nullptr;
    }
    *cap = sizeof(g_ring);
    return g_ring;
}

void returnRing() { mutexUnlock(&g_ring_lock); }

void takeStats(Stats* out) {
    mutexLock(&g_lock);
    *out = g_stats;
    g_stats = Stats{};
    mutexUnlock(&g_lock);
    out->open = g_vopen;
    out->no_game = g_no_game;
    uint64_t v = g_vwait_since, a = g_await_since;
    out->video_waiting_ms = v ? (uint32_t)msSince(v) : 0;
    out->audio_waiting_ms = a ? (uint32_t)msSince(a) : 0;
}

}  // namespace screen
