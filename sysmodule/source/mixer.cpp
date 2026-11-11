#include "mixer.h"
#include "cpushare.h"

#include <math.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace mixer {

int gainQ12(int percent) {
    if (percent <= 0) return 0;
    if (percent > 200) percent = 200;
    if (percent <= 100) return percent * percent * MIXER_UNITY / 10000;
    return percent * MIXER_UNITY / 100;
}

void mix(const int16_t* const* frames, const int* gains, size_t n, int16_t* out) {
    static int32_t sum[MIXER_FRAME];
    memset(sum, 0, sizeof(sum));
    for (size_t s = 0; s < n; s++) {
        int g = gains[s];
        if (g <= 0 || !frames[s]) continue;
        const int16_t* f = frames[s];
        for (size_t i = 0; i < MIXER_FRAME; i++) sum[i] += (f[i] * g) >> 12;
    }
    for (size_t i = 0; i < MIXER_FRAME; i++) {
        int32_t v = sum[i] > 32767 ? 32767 : sum[i] < -32768 ? -32768 : sum[i];
        out[i * 2] = out[i * 2 + 1] = (int16_t)v;
    }
}

namespace {
// Play from `to` on: the slots before it are dropped (counted in *count)
void dropBefore(Jitter& j, uint16_t to, uint32_t* count) {
    for (Jitter::Slot& sl : j.slots)
        if (sl.used && (int16_t)(sl.seq - to) < 0) sl.used = false, (*count)++;
    j.next = to;
}

// How far ahead of next the newest packet is (-1: none)
int ahead(const Jitter& j) {
    int a = -1;
    for (const Jitter::Slot& sl : j.slots)
        if (sl.used && (int16_t)(sl.seq - j.next) > a) a = (int16_t)(sl.seq - j.next);
    return a;
}
}  // namespace

void Jitter::push(uint16_t seq, const uint8_t* opus, size_t n, Stats* st) {
    if (n == 0 || n > MIXER_PACKET_MAX) return;
    if (playing) {
        int16_t d = (int16_t)(seq - next);
        if (d < 0) {  // (its turn has played)
            st->late++;
            return;
        }
        // (it would land on one not played yet: the oldest go, so the newest is kept)
        if (d >= MIXER_JITTER) dropBefore(*this, (uint16_t)(seq - (MIXER_JITTER - 1)), &st->trimmed);
    }
    Slot& sl = slots[seq % MIXER_JITTER];
    sl.used = true;
    sl.seq = seq;
    sl.n = (uint16_t)n;
    memcpy(sl.data, opus, n);
}

const Jitter::Slot* Jitter::pop(Stats* st) {
    if (!playing) {
        int have = 0;
        for (const Slot& sl : slots) have += sl.used;
        if (have < start_at) return nullptr;
        // (start from the oldest packet there is)
        bool first = true;
        for (const Slot& sl : slots) {
            if (!sl.used) continue;
            if (first || (int16_t)(sl.seq - next) < 0) next = sl.seq;
            first = false;
        }
        playing = true;
        idle = 0;
    }
    // too far behind (the waits added up): back to START_AT's cushion in one skip
    int a = ahead(*this);
    if (a >= trim_at) dropBefore(*this, (uint16_t)(next + a - (start_at - 1)), &st->trimmed);
    Slot& sl = slots[next % MIXER_JITTER];
    if (sl.used && sl.seq == next) {
        sl.used = false;
        next++;
        st->waited += idle;  // (the silences before this were a wait, not the end of talking)
        idle = 0;
        return &sl;
    }
    // not here: lost if a later one is (skip it), else not here yet (wait: its 20 ms is silence)
    if (a > 0) next++, st->gaps++;
    else if (++idle > 10) playing = false, idle = 0;  // (nothing for 200 ms: they stopped talking)
    return nullptr;
}

bool soundFrame(const int16_t* pcm, size_t n, int rate, uint64_t* pos, int16_t* out) {
    if (rate <= 0 || !n) {
        memset(out, 0, MIXER_FRAME * sizeof(int16_t));
        return false;
    }
    uint64_t step = ((uint64_t)rate << 16) / MIXER_RATE;  // (input samples per output sample, Q16)
    uint64_t p = *pos;
    bool more = true;
    for (size_t i = 0; i < MIXER_FRAME; i++, p += step) {
        size_t k = (size_t)(p >> 16);
        if (k >= n) {
            out[i] = 0;
            more = false;
            continue;
        }
        int a = pcm[k], b = k + 1 < n ? pcm[k + 1] : 0;
        int frac = (int)(p & 0xFFFF);
        out[i] = (int16_t)(a + (((b - a) * frac) >> 16));
    }
    *pos = p;
    return more && (p >> 16) < n;
}

#ifdef __SWITCH__
namespace {

constexpr int BUFFERS = 5;  // queued in audout: 100 ms (by ear on the Switch: 80 was just short)
constexpr size_t BUF_BYTES = 0x1000;  // (audout wants 0x1000-aligned buffers; a frame is 3840 bytes)
constexpr int TONES = 3;
const float TONE_HZ[TONES] = {440.0f, 554.37f, 659.25f};  // A major, a note each
constexpr int TONE_GAIN = 30 * MIXER_UNITY / 100;  // (each: the three together come close to full)
constexpr float CYCLE_S = 1.5f;    // each note comes in a quarter second after the one before

struct Buf {
    AudioOutBuffer ab;  // (first: a released AudioOutBuffer* is its Buf*)
    bool queued;
};

alignas(0x1000) u8 g_mem[BUFFERS][BUF_BYTES];
Buf g_bufs[BUFFERS];
int16_t g_scratch[TONES][MIXER_FRAME];

Thread g_thread;
volatile bool g_run = false, g_check = false, g_deaf = false;
bool g_out = false;
bool g_fresh = false;  // (audout just opened: its buffers are all out because none went in yet)

struct Tone {
    float phase, level;  // level: 0..1, eased toward on/off so a note doesn't click
};
Tone g_tones[TONES];
uint64_t g_frames = 0;

// the sound effect playing (sounds.h), under g_lock: the mixer reads it there, so stopSound() leaves
// pcm free once it returns
const int16_t* g_sound = nullptr;
size_t g_sound_n = 0;
int g_sound_rate = 0;
uint64_t g_sound_pos = 0;
bool g_sound_through = false;  // (heard while deafened: our own mute and deafen)
int16_t g_sound_frame[MIXER_FRAME];
// (a third: -9.5 dB. The sounds are normalized loud, about -15 dBFS RMS, and at x1 they came out louder
// than the people talking, who sit around -20..-25; a cue should be under the voices, as Discord's are)
constexpr int SOUND_GAIN = MIXER_UNITY / 3;

void toneFrame(int k, int16_t* f) {
    Tone& t = g_tones[k];
    float step = 2.0f * 3.14159265f * TONE_HZ[k] / MIXER_RATE;
    float at = fmodf((float)(g_frames * MIXER_FRAME) / MIXER_RATE, CYCLE_S);
    float target = at >= 0.25f * k ? 1.0f : 0.0f;
    for (size_t i = 0; i < MIXER_FRAME; i++) {
        t.level += (target - t.level) * 0.002f;
        f[i] = (int16_t)(sinf(t.phase) * t.level * 32767.0f);
        t.phase += step;
        if (t.phase > 6.2831853f) t.phase -= 6.2831853f;
    }
}

bool outOpen() {
    if (g_out) return true;
    if (R_FAILED(audoutInitialize())) return false;
    if (R_FAILED(audoutStartAudioOut())) {
        audoutExit();
        return false;
    }
    for (int i = 0; i < BUFFERS; i++) {
        g_bufs[i].ab.next = nullptr;
        g_bufs[i].ab.buffer = g_mem[i];
        g_bufs[i].ab.buffer_size = BUF_BYTES;
        g_bufs[i].ab.data_size = 0;
        g_bufs[i].ab.data_offset = 0;
        g_bufs[i].queued = false;
    }
    g_out = true;
    g_fresh = true;
    return true;
}

void outClose() {
    if (!g_out) return;
    audoutStopAudioOut();
    audoutExit();
    g_out = false;
}

// --- the call: each speaker's packets in a jitter buffer, decoded by a hardware decoder lent to them ---

constexpr int DECODERS = 4;         // speakers heard at once (48 KB of work memory each, from the heap)
constexpr uint64_t DECODER_IDLE_MS = 2000, SPEAKER_GONE_MS = 5000, SPEAKING_MS = 250;
constexpr int SPEAKING_DB = 50;     // louder than -50 dBFS: speaking

struct Speaker {
    bool used;
    uint64_t user;
    uint32_t gen;   // (which of the place's speakers this is: a new one each time it's someone new)
    Jitter jitter;
    uint64_t last_packet_ms, last_sound_ms;
    uint8_t level;  // -dBFS of the last frame (MIXER_SILENT: none)
};

// The decoders, lent to speakers. Whose each one is lives here only, under g_lock, and only this
// thread changes it: a speaker's place given to someone new (the voice thread) just makes the old
// lending stale (its gen no longer matches). Once, the voice thread freed decoders itself while this
// thread was lending them, unlocked: two speakers could end up decoding with one decoder's state
struct Lent {
    int speaker;  // (-1: free)
    uint32_t gen;
};

Mutex g_lock;  // the speakers and the lendings (the voice thread adds packets, this one plays them)
Speaker g_speakers[MIXER_SPEAKERS];
uint32_t g_gen = 0;
volatile bool g_voice = false;
int (*g_gain)(uint64_t user) = nullptr;

HwopusDecoder g_dec[DECODERS];
bool g_dec_open[DECODERS];  // (this thread's alone)
Lent g_lent[DECODERS];
int16_t g_pcm[MIXER_FRAME * 2];
int16_t g_voices[MIXER_SPEAKERS][MIXER_FRAME];
alignas(16) uint8_t g_packet[8 + MIXER_PACKET_MAX];  // (hwopus's 8-byte header, then the packet: aligned as the slots)
Stats g_stats;  // (under g_lock)

void resetStats() {
    g_stats = Stats{};
    g_stats.loudest = MIXER_SILENT;
    g_stats.last_gain = -1;
}
volatile int g_master = MIXER_UNITY;
volatile int g_cushion = MIXER_START_AT;

uint64_t nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

// (under g_lock) Whether decoder d is lent to someone still there (the same speaker, in their place)
bool lentLive(int d) {
    const Lent& l = g_lent[d];
    return l.speaker >= 0 && g_speakers[l.speaker].used && g_speakers[l.speaker].gen == l.gen;
}

// (under g_lock) The decoder lent to speaker s now, or -1
int lentTo(int s) {
    for (int d = 0; d < DECODERS; d++)
        if (lentLive(d) && g_lent[d].speaker == s) return d;
    return -1;
}

// (under g_lock) A decoder to lend speaker s: a free one (or one whose speaker's gone), else the one lent
// to someone quiet now; -1: everyone's talking, this one waits
int pickDecoder() {
    for (int d = 0; d < DECODERS; d++)
        if (!lentLive(d)) return d;
    for (int d = 0; d < DECODERS; d++)
        if (!g_speakers[g_lent[d].speaker].jitter.playing) return d;
    return -1;
}

// Decoder d made fresh for its new speaker (it changes hands: no state carried over); false: it
// couldn't be opened (its lending is given back)
bool openDecoder(int d) {
    if (g_dec_open[d]) hwopusDecoderExit(&g_dec[d]);
    Result rc = hwopusDecoderInitialize(&g_dec[d], MIXER_RATE, 1);
    g_dec_open[d] = R_SUCCEEDED(rc);
    mutexLock(&g_lock);
    if (g_dec_open[d]) g_stats.dec_opened++;
    else g_stats.dec_failed++, g_stats.last_rc = rc, g_lent[d].speaker = -1;
    mutexUnlock(&g_lock);
    return g_dec_open[d];
}

void closeDecoders() {
    for (int d = 0; d < DECODERS; d++) {
        if (g_dec_open[d]) hwopusDecoderExit(&g_dec[d]);
        g_dec_open[d] = false;
    }
    mutexLock(&g_lock);
    for (Lent& l : g_lent) l.speaker = -1;
    mutexUnlock(&g_lock);
}

// The next packet speaker s has to play, copied into g_packet (after hwopus's 8-byte header); 0: none
size_t nextPacket(Speaker& sp) {
    const Jitter::Slot* sl = sp.jitter.pop(&g_stats);  // (under g_lock: voiceFrame holds it)
    if (!sl) return 0;
    uint32_t n = sl->n;
    g_packet[0] = (uint8_t)(n >> 24), g_packet[1] = (uint8_t)(n >> 16), g_packet[2] = (uint8_t)(n >> 8), g_packet[3] = (uint8_t)n;
    memset(g_packet + 4, 0, 4);  // (the final range: not checked)
    memcpy(g_packet + 8, sl->data, n);
    return n;
}

// The call's 20 ms: each speaker decoded into g_voices; how many there are
size_t voiceFrame(const int16_t** frames, int* gains) {
    size_t n = 0;
    uint64_t now = nowMs();
    static uint64_t retry_ms = 0;  // (a decoder that couldn't be opened isn't tried again for a second)
    for (int s = 0; s < MIXER_SPEAKERS; s++) {
        mutexLock(&g_lock);
        Speaker& sp = g_speakers[s];
        size_t len = 0;
        int d = -1;
        bool fresh = false;
        if (sp.used && now - sp.last_packet_ms > SPEAKER_GONE_MS) sp = Speaker{};  // (its lending goes stale with it)
        if (sp.used) {
            sp.jitter.start_at = g_cushion;
            sp.jitter.trim_at = g_cushion + (MIXER_TRIM_AT - MIXER_START_AT);
            len = nextPacket(sp);
            d = lentTo(s);
            if (d >= 0 && !len && !sp.jitter.playing && now - sp.last_packet_ms > DECODER_IDLE_MS) {
                g_lent[d].speaker = -1;  // (quiet a while: someone else may have it)
                d = -1;
            }
            if (len && d < 0 && now >= retry_ms && (d = pickDecoder()) >= 0) {
                g_lent[d] = Lent{s, sp.gen};
                fresh = true;
            }
        }
        uint64_t user = sp.user;
        uint32_t gen = sp.gen;
        mutexUnlock(&g_lock);
        if (!len || d < 0) continue;
        if (fresh && !openDecoder(d)) {
            retry_ms = now + 1000;
            continue;
        }
        s32 used = 0, samples = 0;
        Result rc = hwopusDecodeInterleaved(&g_dec[d], &used, &samples, g_packet, 8 + len, g_pcm, sizeof(g_pcm));
        if (R_FAILED(rc) || samples <= 0) {
            mutexLock(&g_lock);
            g_stats.decode_failed++;
            g_stats.last_rc = R_FAILED(rc) ? rc : 1;  // (1: it said fine, but gave no samples)
            mutexUnlock(&g_lock);
            continue;
        }
        if (samples > (s32)MIXER_FRAME) samples = MIXER_FRAME;
        memcpy(g_voices[n], g_pcm, samples * 2);
        if (samples < (s32)MIXER_FRAME) memset(g_voices[n] + samples, 0, (MIXER_FRAME - samples) * 2);
        // how loud, for the ring and the overlay's dB
        uint64_t sum = 0;
        for (size_t i = 0; i < MIXER_FRAME; i++) sum += (uint64_t)((int32_t)g_voices[n][i] * g_voices[n][i]);
        float rms = sqrtf((float)sum / MIXER_FRAME);
        int db = rms < 1.0f ? 255 : (int)(-20.0f * log10f(rms / 32768.0f));
        int gain = g_deaf ? 0 : ((g_gain ? g_gain(user) : MIXER_UNITY) * g_master) >> 12;
        mutexLock(&g_lock);
        if (sp.used && sp.gen == gen) {  // (unless it was someone new meanwhile)
            sp.level = (uint8_t)(db > 255 ? 255 : db < 0 ? 0 : db);
            if (sp.level < SPEAKING_DB) sp.last_sound_ms = now;
        }
        g_stats.decoded++;
        if (db < g_stats.loudest) g_stats.loudest = (uint8_t)(db < 0 ? 0 : db);
        g_stats.last_gain = gain * 100 / MIXER_UNITY;
        mutexUnlock(&g_lock);
        frames[n] = g_voices[n];
        gains[n] = gain;
        n++;
    }
    return n;
}

// The next 20 ms into b: the call, and the sound check's tones
void fill(Buf& b) {
    const int16_t* frames[TONES + 1 + MIXER_SPEAKERS];
    int gains[TONES + 1 + MIXER_SPEAKERS];
    size_t n = 0;
    if (g_check) {
        for (int k = 0; k < TONES; k++) {
            toneFrame(k, g_scratch[k]);
            frames[n] = g_scratch[k];
            gains[n++] = g_deaf ? 0 : (TONE_GAIN * g_master) >> 12;
        }
    }
    mutexLock(&g_lock);
    if (g_sound) {
        if (!soundFrame(g_sound, g_sound_n, g_sound_rate, &g_sound_pos, g_sound_frame)) g_sound = nullptr;
        frames[n] = g_sound_frame;  // (its last frame, the rest silence, still goes)
        gains[n++] = g_deaf && !g_sound_through ? 0 : (SOUND_GAIN * g_master) >> 12;
    }
    mutexUnlock(&g_lock);
    if (g_voice) {
        size_t v = voiceFrame(frames + n, gains + n);
        if (v) {
            mutexLock(&g_lock);
            g_stats.mixed++;
            mutexUnlock(&g_lock);
        }
        n += v;
    }
    mix(frames, gains, n, (int16_t*)b.ab.buffer);
    b.ab.data_size = MIXER_FRAME * 4;
    b.ab.data_offset = 0;
    g_frames++;
}

void mixThread(void*) {
    while (g_run) {
        if (!g_check && !g_voice && !soundPlaying()) {
            closeDecoders();
            outClose();
            svcSleepThread(100'000'000LL);
            continue;
        }
        if (!outOpen()) {
            svcSleepThread(1'000'000'000LL);
            continue;
        }
        // every buffer that isn't in audout gets the next frame and goes in (all of them out: it
        // had played everything we gave it, and there was a gap)
        bool all = true;
        for (const Buf& b : g_bufs) all = all && !b.queued;
        if (all && !g_fresh) {
            mutexLock(&g_lock);
            g_stats.dry++;
            mutexUnlock(&g_lock);
        }
        g_fresh = false;
        for (Buf& b : g_bufs) {
            if (b.queued) continue;
            fill(b);
            if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&b.ab))) b.queued = true;
        }
        AudioOutBuffer* rel = nullptr;
        u32 count = 0;
        if (R_SUCCEEDED(audoutWaitPlayFinish(&rel, &count, 100'000'000ULL)) && count && rel) ((Buf*)rel)->queued = false;
        for (;;) {
            rel = nullptr;
            count = 0;
            if (R_FAILED(audoutGetReleasedAudioOutBuffer(&rel, &count)) || count == 0 || !rel) break;
            ((Buf*)rel)->queued = false;
        }
    }
    closeDecoders();
    outClose();
}

}  // namespace

void setVoice(bool on) {
    mutexLock(&g_lock);
    if (!on)
        for (Speaker& sp : g_speakers) sp = Speaker{};
    g_voice = on;
    mutexUnlock(&g_lock);
}

void setGainSource(int (*gain)(uint64_t user)) { g_gain = gain; }
void setMaster(int gain) { g_master = gain; }

void voicePacket(uint64_t user, uint16_t seq, const uint8_t* opus, size_t n) {
    if (!g_voice || n == 0 || n > MIXER_PACKET_MAX) return;
    uint64_t now = nowMs();
    mutexLock(&g_lock);
    Speaker* sp = nullptr;
    for (Speaker& s : g_speakers)
        if (s.used && s.user == user) sp = &s;
    if (!sp) {
        // (someone new: a free place, else the one heard from longest ago)
        for (Speaker& s : g_speakers)
            if (!sp && !s.used) sp = &s;
        if (!sp) {
            sp = &g_speakers[0];
            for (Speaker& s : g_speakers)
                if (s.last_packet_ms < sp->last_packet_ms) sp = &s;
        }
        *sp = Speaker{};  // (whoever had the place: their decoder's lending is stale now, see Lent)
        sp->used = true;
        sp->user = user;
        sp->gen = ++g_gen;
        sp->level = MIXER_SILENT;
    }
    sp->last_packet_ms = now;
    g_stats.packets++;
    sp->jitter.push(seq, opus, n, &g_stats);
    mutexUnlock(&g_lock);
}

void setCushion(int packets) {
    if (packets < 1) packets = 1;
    if (packets > MIXER_JITTER - (MIXER_TRIM_AT - MIXER_START_AT)) packets = MIXER_JITTER - (MIXER_TRIM_AT - MIXER_START_AT);
    g_cushion = packets;
}

void busy(int* speakers, int* decoders, int* speakers_max, int* decoders_max) {
    int s = 0, dc = 0;
    mutexLock(&g_lock);
    for (int i = 0; i < MIXER_SPEAKERS; i++) s += g_speakers[i].used;
    for (int i = 0; i < DECODERS; i++) dc += lentLive(i);
    mutexUnlock(&g_lock);
    *speakers = s, *decoders = dc, *speakers_max = MIXER_SPEAKERS, *decoders_max = DECODERS;
}

bool speaker(uint64_t user, bool* speaking, uint8_t* level) {
    uint64_t now = nowMs();
    bool found = false;
    mutexLock(&g_lock);
    for (const Speaker& s : g_speakers) {
        if (!s.used || s.user != user) continue;
        found = true;
        *speaking = s.last_sound_ms && now - s.last_sound_ms < SPEAKING_MS;
        *level = now - s.last_packet_ms < SPEAKING_MS ? s.level : MIXER_SILENT;
    }
    mutexUnlock(&g_lock);
    return found;
}

void takeStats(Stats* out) {
    mutexLock(&g_lock);
    *out = g_stats;
    out->deaf = g_deaf;
    out->out_open = g_out;
    resetStats();
    mutexUnlock(&g_lock);
}

void start() {
    mutexInit(&g_lock);
    resetStats();
    for (Lent& l : g_lent) l.speaker = -1;
    g_run = true;
    // (30: just below the system's sockets (bsd's network threads: 25 to 28, its socket calls 29), so they
    // never wait their turn behind it)
    if (R_FAILED(threadCreate(&g_thread, mixThread, nullptr, nullptr, 0x4000, 0x1E, -2))) {
        g_run = false;
        return;
    }
    threadStart(&g_thread);
    cpushare::track(g_thread.handle);
}

void stop() {
    if (!g_run) return;
    g_run = false;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
}

void setSoundCheck(bool on) {
    if (on && !g_check) {
        g_frames = 0;
        for (Tone& t : g_tones) t = Tone{0, 0};
    }
    g_check = on;
}

bool soundCheck() { return g_check; }

void playSound(const int16_t* pcm, size_t n, int rate, bool through_deaf) {
    mutexLock(&g_lock);
    g_sound = n && rate > 0 ? pcm : nullptr;
    g_sound_n = n, g_sound_rate = rate, g_sound_pos = 0;
    g_sound_through = through_deaf;
    mutexUnlock(&g_lock);
}

void stopSound() {
    mutexLock(&g_lock);
    g_sound = nullptr;
    mutexUnlock(&g_lock);
}

bool soundPlaying() {
    mutexLock(&g_lock);
    bool on = g_sound != nullptr;
    mutexUnlock(&g_lock);
    return on;
}
void setDeaf(bool on) { g_deaf = on; }
#endif

}  // namespace mixer
