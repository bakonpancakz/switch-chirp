#include "screen.h"

#include <switch.h>
#include <string.h>

#include "game.h"
#include "opusenc.h"

namespace screen {
namespace {

// grc:d's rules (from SysDVR, which learned them the hard way):
// - Begin (cmd 1) starts recording for everyone, and may be called only ONCE: a second call, from any
//   session, makes grc assert and the Switch crash. So it's called only when a transfer says it hasn't
//   been (0x3E8D4), and never again while Chirp runs; grc remembers it across Chirp's restarts.
// - Transfer (cmd 2) blocks until there's a frame (forever with no game that allows recording).
// - Video needs 0x54000: an IDR frame can be bigger than the 0x32000 the system uses, and fails 2212-0006.
// - Audio comes in 0x1000 chunks (48 kHz, stereo, s16), on its own session, as SysDVR reads it.
constexpr Result NOT_BEGUN = 0x3E8D4;

alignas(0x1000) uint8_t g_buf[SCREEN_FRAME_MAX + SCREEN_HEADROOM];
alignas(0x1000) uint8_t g_abuf[0x1000];
// The frames waiting to be sent, copied out of g_buf so the recorder is read again at once: grc keeps
// no frame nobody's reading, and a keyframe (~100 KB) takes longer than a frame's time to get out over
// Wi-Fi; the small frames after it give the sender time to catch up. One writer (capture: at head), one
// reader (the stream's connection: at tail, moved on by done()). A frame too big for the ring is sent
// from g_buf itself, capture waiting
uint8_t g_ring[SCREEN_RING];
struct Entry {
    uint8_t* p;
    size_t n, cap;
    uint64_t tick;
    bool gap;  // (frames before it were dropped)
};
bool g_gap = false;  // (capture's: a frame's been dropped since the last one queued)
constexpr uint32_t QN = 16;
Entry g_q[QN];
uint32_t g_qhead = 0, g_qtail = 0;  // (atomic)
volatile bool g_flush = false;      // (sharing stopped: what's queued is stale, the reader drops it)
bool g_holding = false;             // (the reader's: it has the tail frame)
Mutex g_ring_lock;                  // (capture's while it copies a frame in; borrowRing()'s while it's lent)

uint32_t qTail() { return __atomic_load_n(&g_qtail, __ATOMIC_ACQUIRE); }

// Room for a frame of `need` bytes after the newest queued one (or at the ring's start); null: none now
uint8_t* room(size_t need) {
    uint32_t head = g_qhead, tail = qTail();
    if (head - tail >= QN) return nullptr;
    if (head == tail) return g_ring;
    const Entry& first = g_q[tail % QN];
    const Entry& last = g_q[(head - 1) % QN];
    if (first.p == g_buf || last.p == g_buf) return nullptr;
    size_t f = (size_t)(first.p - g_ring), e = (size_t)(last.p + last.cap - g_ring);
    if (e > f) {
        if (SCREEN_RING - e >= need) return g_ring + e;
        return f >= need ? g_ring : nullptr;
    }
    return f - e >= need ? g_ring + e : nullptr;
}

void push(uint8_t* p, size_t n, size_t cap, uint64_t tick) {
    g_q[g_qhead % QN] = Entry{p, n, cap, tick, g_gap};
    g_gap = false;
    __atomic_store_n(&g_qhead, g_qhead + 1, __ATOMIC_RELEASE);
}
uint64_t g_last_ts = 0, g_min_gap = 0;  // (the recorder's timestamps: a jump means frames we missed)
int64_t g_ts_offset = 0;  // (the recorder's microseconds to our ticks: set when sharing starts, or drifts too far)

// When the frame was recorded, on our clock: the recorder's own timestamp (microseconds, evenly 33 ms
// apart) moved onto ours. (When it was read instead varies with how long grc and the queue kept it:
// viewers' players pace by these, and uneven ones look like a bad frame rate)
uint64_t recordedTick(uint64_t ts) {
    uint64_t now = armGetSystemTick();
    int64_t t = (int64_t)armNsToTicks(ts * 1000);
    int64_t mapped = t + g_ts_offset;
    int64_t off = (int64_t)now - mapped;
    if (!g_last_ts || mapped <= 0 || off > (int64_t)armNsToTicks(500'000'000) || off < -(int64_t)armNsToTicks(500'000'000)) {
        g_ts_offset = (int64_t)now - t;
        mapped = (int64_t)now;
    }
    return (uint64_t)mapped;
}
Thread g_vthread, g_athread;
volatile bool g_run = false, g_wanted = false, g_call = false;
Service g_vsrv, g_asrv;
bool g_vopen = false, g_aopen = false;
bool g_begun = false;  // (once: see above)
Mutex g_begin_lock;
Mutex g_lock;  // (the stats)
Stats g_stats;
// (when the transfer going on now began, video's and sound's: system ticks, 0 when none is)
volatile uint64_t g_vwait_since = 0, g_await_since = 0;
volatile bool g_no_game = false;  // (sharing, but no game running: video isn't asked for)

// A transfer, timed: how long grc:d took is kept in the stats (the longest), and while it's waiting
// takeStats() says for how long
Result timedRead(Service* srv, GrcStream stream, void* buf, size_t size, u32* data_size, u64* ts, volatile uint64_t* since,
                 uint32_t Stats::*longest);

// the game's sound: 20 ms gathered, encoded, queued for the stream's connection (one writer, one reader)
constexpr int AQ = 16;  // (320 ms: room while a keyframe goes out)
AudioPacket g_aq[AQ];
uint32_t g_aq_head = 0, g_aq_tail = 0;  // (atomic: the audio thread writes at head, the stream reads at tail)
OpusEnc g_enc;
int16_t g_pcm[OPUSENC_FRAME];
int g_pcm_n = 0;
uint32_t g_aframe = 0;
uint64_t g_ats0 = 0, g_apos = 0;  // (the recorder's time the sound began (us), and samples since)
uint64_t g_alast = 0;              // (the last chunk's timestamp)
bool g_ats_us = false;  // (seen one chunk after another 21.3 ms apart: the timestamps are microseconds, as assumed)

// Stereo s16 frames (null: silence) into 20 ms Opus packets
void audioSamples(const int16_t* s, size_t frames) {
    for (size_t i = 0; i < frames; i++) {
        g_pcm[g_pcm_n++] = s ? (int16_t)(((int)s[2 * i] + s[2 * i + 1]) / 2) : 0;
        if (g_pcm_n < OPUSENC_FRAME) continue;
        g_pcm_n = 0;
        uint32_t head = __atomic_load_n(&g_aq_head, __ATOMIC_RELAXED);
        uint32_t tail = __atomic_load_n(&g_aq_tail, __ATOMIC_ACQUIRE);
        uint32_t frame = g_aframe++;
        if (head - tail >= AQ) {  // (not taken: this one's dropped; the timestamps skip it)
            mutexLock(&g_lock);
            g_stats.audio_dropped++;
            mutexUnlock(&g_lock);
            continue;
        }
        AudioPacket& q = g_aq[head % AQ];
        opusencFrame(&g_enc, g_pcm, q.data, SCREEN_AUDIO_BYTES);
        q.n = SCREEN_AUDIO_BYTES;
        q.frame = frame;
        __atomic_store_n(&g_aq_head, head + 1, __ATOMIC_RELEASE);
    }
}

void failed(Result rc) {
    mutexLock(&g_lock);
    g_stats.failed++;
    g_stats.last_rc = rc;
    mutexUnlock(&g_lock);
}

Result transfer(Service* srv, GrcStream stream, void* buf, size_t size, u32* data_size, u64* ts = nullptr) {
    struct {
        u32 num_frames, data_size;
        u64 start_timestamp;
    } out = {};
    u32 in = stream;
    Result rc = serviceDispatchInOut(srv, 2, in, out, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                     .buffers = {{buf, size}}, );
    *data_size = out.data_size;
    if (ts) *ts = out.start_timestamp;
    return rc;
}

// A transfer; if grc says recording hasn't begun, Begin (the one time) and try again
Result read(Service* srv, GrcStream stream, void* buf, size_t size, u32* data_size, u64* ts = nullptr) {
    Result rc = transfer(srv, stream, buf, size, data_size, ts);
    if (rc != NOT_BEGUN) return rc;
    mutexLock(&g_begin_lock);
    bool begin = !g_begun;
    g_begun = true;  // (set first: even a Begin that fails isn't tried again)
    Result brc = begin ? serviceDispatch(&g_vsrv, 1) : 0;
    mutexUnlock(&g_begin_lock);
    if (R_FAILED(brc)) return brc;
    return transfer(srv, stream, buf, size, data_size, ts);
}

bool sharing() { return g_wanted && g_call; }

Result timedRead(Service* srv, GrcStream stream, void* buf, size_t size, u32* data_size, u64* ts, volatile uint64_t* since,
                 uint32_t Stats::*longest) {
    uint64_t t0 = armGetSystemTick();
    *since = t0;
    Result rc = read(srv, stream, buf, size, data_size, ts);
    *since = 0;
    uint32_t ms = (uint32_t)(armTicksToNs(armGetSystemTick() - t0) / 1000000);
    mutexLock(&g_lock);
    if (ms > g_stats.*longest) g_stats.*longest = ms;
    mutexUnlock(&g_lock);
    return rc;
}

void videoThread(void*) {
    while (g_run) {
        if (!sharing()) {
            g_last_ts = g_min_gap = 0;  // (a pause in sharing isn't frames missed; 30 or 60 a second may change meanwhile)
            if (g_qhead != qTail()) g_flush = true;
            svcSleepThread(100'000'000LL);
            continue;
        }
        // Video is only asked for while a game's running. A transfer asked for with none (the HOME menu,
        // or homebrew such as sphaira, which grc doesn't record) is never answered, not even once a game
        // starts: the thread stayed stuck in it, the stream on its loading screen, until the Switch was
        // restarted (restarting Chirp didn't free it). Meanwhile viewers get the loading screen
        if ((game::running() >> 56) != 0x01) {
            g_no_game = true;
            g_last_ts = g_min_gap = 0;
            svcSleepThread(250'000'000LL);
            continue;
        }
        g_no_game = false;
        if (!g_vopen) {  // (opened once, and kept: a transfer may still be waiting in another thread)
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
        Result rc = timedRead(&g_vsrv, GrcStream_Video, g_buf, SCREEN_FRAME_MAX, &size, &ts, &g_vwait_since, &Stats::video_longest_ms);
        if (R_FAILED(rc) || size <= 4 || size > SCREEN_FRAME_MAX) {
            failed(R_FAILED(rc) ? rc : 1);
            svcSleepThread(100'000'000LL);  // (and try again: never Begin again, never reopen)
            continue;
        }
        uint64_t tick = ts ? recordedTick(ts) : armGetSystemTick();
        // (the timestamps step by one frame's time: the smallest step seen is that; a bigger one, frames missed)
        uint32_t missed = 0;
        uint64_t gap = g_last_ts && ts > g_last_ts ? ts - g_last_ts : 0;
        g_last_ts = ts;
        // (a frame's time is 16.7 or 33.3 ms: a step much shorter is the recorder catching up, not the frame time,
        // and taken as it, it made every normal step look like ten missed frames)
        if (gap >= 12000 && (!g_min_gap || gap < g_min_gap)) g_min_gap = gap;
        if (gap && g_min_gap && gap > g_min_gap * 3 / 2) missed = (uint32_t)((gap + g_min_gap / 2) / g_min_gap) - 1;
        if (missed) g_gap = true;
        // into the queue. Full: a wait, as grc keeps its next frames a while (waits of 100 ms lost nothing at
        // 60 fps); only after 300 ms is it dropped instead (marked: the frames after it wait for a keyframe).
        // (Dropped after 5 ms, each drop cost a quarter second of frames waiting for a keyframe: 44 fps, not 60.)
        // If grc does drop frames meanwhile, its timestamps say so (missed, above) and they're marked too
        size_t need = size + SCREEN_HEADROOM;
        bool big = need > SCREEN_RING;
        bool queued = false;
        uint64_t w0 = armGetSystemTick();
        for (int tries = 0; g_run && sharing(); tries++) {
            if (big) {
                if (g_qhead == qTail()) break;
            } else {
                mutexLock(&g_ring_lock);
                if (uint8_t* at = room(need)) {
                    memcpy(at, g_buf, size);
                    push(at, size, need, tick);
                    queued = true;
                }
                mutexUnlock(&g_ring_lock);
                if (queued) break;
                if (tries >= 300) {
                    g_gap = true;
                    mutexLock(&g_lock);
                    g_stats.no_room++;
                    mutexUnlock(&g_lock);
                    break;
                }
            }
            svcSleepThread(1'000'000LL);
        }
        uint32_t waited = (uint32_t)(armTicksToNs(armGetSystemTick() - w0) / 1000000);
        mutexLock(&g_lock);
        g_stats.captured++;
        g_stats.skipped += missed;
        g_stats.waited_ms += waited;
        if (gap && (!g_stats.gap_min || gap < g_stats.gap_min)) g_stats.gap_min = gap;
        if (gap > g_stats.gap_max) g_stats.gap_max = gap;
        mutexUnlock(&g_lock);
        if (big && g_run && sharing()) {  // (not turned off while it waited)
            {
                // (sent from g_buf: kept until it's done)
                push(g_buf, size, sizeof(g_buf), tick);
                while (g_run && sharing() && g_qhead != qTail()) svcSleepThread(1'000'000LL);
            }
        }
    }
}

void audioThread(void*) {
    while (g_run) {
        if (!sharing() || !g_vopen) {
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
        if (R_FAILED(timedRead(&g_asrv, GrcStream_Audio, g_abuf, sizeof(g_abuf), &size, &ts, &g_await_since, &Stats::audio_longest_ms))) {
            svcSleepThread(100'000'000LL);
            continue;
        }
        if (size > sizeof(g_abuf)) size = sizeof(g_abuf);
        mutexLock(&g_lock);
        g_stats.audio_chunks++;
        mutexUnlock(&g_lock);
        if (!sharing()) {
            g_ats0 = 0;
            continue;
        }
        // Where this chunk belongs, by the recorder's clock (its timestamps are microseconds): chunks we
        // missed (read too late) leave a hole. A short one is filled with silence, so the packets keep
        // their place and the viewers' players don't drift; a long one (the HOME menu) moves the
        // timestamps on instead. Counting packets alone, as before, made a missed chunk look like the
        // sound arriving slowly: players stretched it, then rushed to catch up
        uint64_t frames = size / 4;
        if (frames == 1024 && g_alast && ts > g_alast && ts - g_alast >= 20000 && ts - g_alast <= 22700) g_ats_us = true;
        g_alast = ts;
        if (g_ats_us && ts && g_ats0 && ts > g_ats0) {
            uint64_t want = (ts - g_ats0) * 48 / 1000;  // (samples since the start, by the clock)
            if (want > g_apos + 512) {
                uint64_t hole = want - g_apos;
                mutexLock(&g_lock);
                g_stats.audio_holes++;
                mutexUnlock(&g_lock);
                if (hole <= 48000 / 5) {
                    audioSamples(nullptr, (size_t)hole);
                } else {
                    g_pcm_n = 0;  // (what was gathered goes; the next packet starts after the gap)
                    g_aframe += (uint32_t)(hole / OPUSENC_FRAME) + 1;
                }
                g_apos = want;
            } else if (want + 48000 < g_apos) {
                g_ats0 = 0;  // (the clock went back: begun again)
            }
        }
        if (ts && !g_ats0) g_ats0 = ts, g_apos = 0;
        audioSamples((const int16_t*)g_abuf, (size_t)frames);
        g_apos += frames;
    }
}

}  // namespace

bool start() {
    mutexInit(&g_lock);
    mutexInit(&g_begin_lock);
    mutexInit(&g_ring_lock);
    g_run = true;
    // (capture high: it does little but must read each frame while grc still has it; the sound below)
    if (R_FAILED(threadCreate(&g_vthread, videoThread, nullptr, nullptr, 0x2000, 0x27, -2))) {
        g_run = false;
        return false;
    }
    opusencInit(&g_enc);
    // (the sound's thread encodes too: the encoder's stack, as the mic's)
    // (the sound's thread above the stream's connection: a chunk read late is a chunk lost, as video's)
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
volatile bool g_full_range = false;
void setFullRange(bool on) { g_full_range = on; }
bool fullRange() { return g_full_range; }
bool wanted() { return g_wanted; }
void setCall(bool on) { g_call = on; }
bool active() { return g_vopen && sharing(); }

bool take(uint8_t** frame, size_t* n, size_t* cap, uint64_t* tick, bool* gap) {
    if (g_holding) return false;
    uint32_t tail = g_qtail, head = __atomic_load_n(&g_qhead, __ATOMIC_ACQUIRE);
    if (g_flush) {
        g_flush = false;
        __atomic_store_n(&g_qtail, head, __ATOMIC_RELEASE);
        return false;
    }
    if (head == tail) return false;
    const Entry& e = g_q[tail % QN];
    *frame = e.p, *n = e.n, *cap = e.cap, *tick = e.tick, *gap = e.gap;
    g_holding = true;
    return true;
}

void done() {
    if (!g_holding) return;
    g_holding = false;
    __atomic_store_n(&g_qtail, g_qtail + 1, __ATOMIC_RELEASE);
}

bool takeAudio(AudioPacket* p) {
    uint32_t tail = __atomic_load_n(&g_aq_tail, __ATOMIC_RELAXED);
    if (__atomic_load_n(&g_aq_head, __ATOMIC_ACQUIRE) == tail) return false;
    *p = g_aq[tail % AQ];
    __atomic_store_n(&g_aq_tail, tail + 1, __ATOMIC_RELEASE);
    return true;
}

uint8_t* borrowRing(size_t* cap, bool flush) {
    mutexLock(&g_ring_lock);
    // (capture only queues holding this lock: emptied here, it stays empty until it's given back)
    if (flush && !g_holding) __atomic_store_n(&g_qtail, __atomic_load_n(&g_qhead, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    if (g_holding || __atomic_load_n(&g_qhead, __ATOMIC_ACQUIRE) != g_qtail) {
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
    out->open = g_vopen;
    out->no_game = g_no_game;
    uint64_t now = armGetSystemTick(), v = g_vwait_since, a = g_await_since;
    out->video_waiting_ms = v && now > v ? (uint32_t)(armTicksToNs(now - v) / 1000000) : 0;
    out->audio_waiting_ms = a && now > a ? (uint32_t)(armTicksToNs(now - a) / 1000000) : 0;
    g_stats = Stats{};
    mutexUnlock(&g_lock);
}

}  // namespace screen
