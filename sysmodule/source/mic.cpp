#include "mic.h"

#include <math.h>
#include <string.h>

#include "denoise.h"
#include "opusenc.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace mic {

constexpr float OVER_FLOOR_DB = 10;  // automatic: talk is this much above the room
constexpr float MIN_DB = -60;        // ...and at least this loud (before the gain: speech is about -45)
constexpr int HOLD_FRAMES = 15;      // 300 ms after it drops

float levelDb(const int16_t* s, size_t n) {
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (double)s[i] * s[i];
    double rms = sqrt(sum / (n ? n : 1));
    return rms < 1 ? -96.f : (float)(20 * log10(rms / 32768));
}

float Detector::threshold() const {
    if (fixed_db < 0) return fixed_db;
    return floor_db + OVER_FLOOR_DB > MIN_DB ? floor_db + OVER_FLOOR_DB : MIN_DB;
}

bool Detector::feed(float db) {
    // the floor: follows quiet quickly, loudness slowly (1.5 dB a second: speech doesn't drag it up)
    if (db < floor_db) floor_db += (db - floor_db) * 0.3f;
    else floor_db += 0.03f;
    if (floor_db < -90) floor_db = -90;
    if (floor_db > -25) floor_db = -25;
    // deciding the frame before this one: talk starts when it and this one are both over the line
    // (a click is one frame: it doesn't), carries on while frames are, and is held a while after
    float t = threshold();
    bool before = last_db > t, now = db > t;
    last_db = db;
    if (!on) {
        if (before && now) {
            on = true;
            hold = HOLD_FRAMES;
        } else if (before) {
            clicks++;
        }
    } else if (before) {
        hold = HOLD_FRAMES;
    } else if (--hold <= 0) {
        on = false;
    }
    return on;
}

float volumeGain(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 200) percent = 200;
    float v = percent / 100.f;
    return percent <= 100 ? v * v : v;
}

HighPass::HighPass() {
    const double K = tan(3.14159265358979323846 * 100 / 48000), Q = 0.70710678;
    double norm = 1 / (1 + K / Q + K * K);
    b0 = (float)norm, b1 = (float)(-2 * norm), b2 = (float)norm;
    a1 = (float)(2 * (K * K - 1) * norm), a2 = (float)((1 - K / Q + K * K) * norm);
}

void HighPass::run(float* x, int n) {
    for (int i = 0; i < n; i++) {
        float in = x[i];
        float out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1, x1 = in, y2 = y1, y1 = out;
        x[i] = out;
    }
}

float Agc::feed(float raw_db, bool talk) {
    if (talk) {
        float out = raw_db + gain_db;
        if (out > MIC_AGC_TARGET + 3) gain_db -= (out - MIC_AGC_TARGET - 3) < 1.f ? out - MIC_AGC_TARGET - 3 : 1.f;  // (50 dB/s)
        else if (out < MIC_AGC_TARGET - 3) gain_db += 0.05f;  // (2.5 dB/s: quiet syllables don't pump it)
        if (gain_db < MIC_AGC_MIN) gain_db = MIC_AGC_MIN;
        if (gain_db > MIC_AGC_MAX) gain_db = MIC_AGC_MAX;
    }
    return gain_db;
}

#ifdef __SWITCH__
namespace {

constexpr int KNEE = 24000;  // above this, loudness is squeezed (a quarter as steep) rather than clipped
constexpr int BUFS = 12;  // (255 ms: room for the thread to be kept waiting without audin running out)
constexpr size_t BUF_BYTES = 0x1000;  // (audin wants 0x1000-aligned buffers: 1024 stereo frames)
constexpr int QUEUE = 8;
constexpr int SILENCE_FRAMES = 5;

alignas(0x1000) uint8_t g_mem[BUFS][BUF_BYTES];
AudioInBuffer g_bufs[BUFS];
Thread g_thread;
volatile bool g_run = false, g_call = false, g_muted = false;
Agc g_agc;
float g_agc_now = 7.94f;     // (the gain the last frame ended at, as a factor: +18 dB to start)
uint32_t g_frame_no = 0;     // 20 ms frames since the start, on the real clock (RTP's timestamps)
uint64_t g_start_tick = 0;
bool g_open = false;
bool g_reopened = false;  // (the clock jumps over the time the mic was closed: not lost audio)
uint32_t g_channels = 2;

Mutex g_lock;  // the queue, the level and the stats
Packet g_queue[QUEUE];
int g_head = 0, g_count = 0;
Stats g_stats;
volatile bool g_speaking = false;
volatile uint8_t g_level = MIC_SILENT;

OpusEnc g_enc;
Denoiser g_dn;  // (RNNoise v0.2: 25 KB of state)
HighPass g_hp;
Detector g_det;
int16_t g_frame[OPUSENC_FRAME];   // (filling from audin)
float g_prev[OPUSENC_FRAME];      // (the frame before, filtered: the one talk is decided for, a frame late)
float g_prev_db = -96;
// the settings (setSettings: any thread; read once a frame)
volatile int g_volume = 100, g_sensitivity = MIC_SENSITIVITY_AUTO;
volatile bool g_agc_on = true, g_denoise_on = true;
bool g_denoising = false;  // (as the last frame was: switched on, it starts fresh)
volatile int g_meter_db = -96, g_meter_threshold = -60, g_meter_gain = 0;
size_t g_have = 0;
int g_silence_left = 0;  // silence frames still to send after talking

void resetStats() {
    g_stats = Stats{};
    g_stats.floor_db = g_stats.peak_db = -96;
}

void push(const uint8_t* data, size_t n, bool last) {
    mutexLock(&g_lock);
    if (g_count == QUEUE) {  // (the voice connection isn't taking them: the oldest goes)
        g_head = (g_head + 1) % QUEUE;
        g_count--;
        g_stats.dropped++;
    }
    Packet& p = g_queue[(g_head + g_count) % QUEUE];
    memcpy(p.data, data, n);
    p.n = (uint8_t)n;
    p.frame = g_frame_no;
    p.last = last;
    g_count++;
    mutexUnlock(&g_lock);
}

// This thread's own CPU time (the kernel's count: time it was kept waiting doesn't count), in ticks
uint64_t cpuTicks() {
    uint64_t t = 0;
    if (R_SUCCEEDED(svcGetInfo(&t, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, TickCountInfo_Total))) return t;
    return armGetSystemTick();  // (before 13.0.0: the clock, waits and all)
}

void frame() {
    // (frames audin dropped while we were slow are counted anyway: the timestamps follow the clock,
    // so a receiver sees a gap, rather than a sender running slow that it buffers more and more for)
    g_frame_no++;
    uint32_t due = (uint32_t)(armTicksToNs(armGetSystemTick() - g_start_tick) / 20'000'000ULL);
    if (g_reopened) {
        g_reopened = false;
        if (due > g_frame_no) g_frame_no = due;
    } else if (due > g_frame_no + 10) {  // (more behind than audin's buffers hold: some was lost. The drift stays under 200 ms)
        mutexLock(&g_lock);
        g_stats.lost += due - g_frame_no;
        mutexUnlock(&g_lock);
        g_frame_no = due;
    }
    uint64_t t0 = cpuTicks();
    // this frame: the input volume, then the rumble out; its level is what the sensitivity sees
    static float f[OPUSENC_FRAME];
    float vol = volumeGain(g_volume);
    for (size_t i = 0; i < OPUSENC_FRAME; i++) f[i] = g_frame[i] * vol;
    // the room's noise out (two of RNNoise's 10 ms frames), so it neither counts as talk nor gets turned up
    bool dn = g_denoise_on;
    if (dn && !g_denoising) denoiseInit(&g_dn);
    g_denoising = dn;
    if (dn) {
        uint64_t d0 = cpuTicks();
        denoiseFrame(&g_dn, f, f);
        denoiseFrame(&g_dn, f + DENOISE_FRAME, f + DENOISE_FRAME);
        uint64_t us = armTicksToNs(cpuTicks() - d0) / 1000;
        mutexLock(&g_lock);
        g_stats.denoise_us += (uint32_t)us;
        mutexUnlock(&g_lock);
    }
    g_hp.run(f, OPUSENC_FRAME);
    double sum = 0;
    for (size_t i = 0; i < OPUSENC_FRAME; i++) sum += (double)f[i] * f[i];
    double rms = sqrt(sum / OPUSENC_FRAME);
    float raw = rms < 1 ? -96.f : (float)(20 * log10(rms / 32768));
    // the frame before is decided now (talk needs it and this one both loud: see Detector)
    g_det.fixed_db = (float)g_sensitivity;
    bool was = g_det.on;
    uint32_t clicks = g_det.clicks;
    bool talk = g_det.feed(raw);
    // the gain, for the frame before (learning from talk only: silence never turns it up), eased from
    // the last frame's to this one's across the frame (no steps to hear)
    float gain_db = g_agc_on ? g_agc.feed(g_prev_db, talk) : 0.f;
    float from = g_agc_now, to = powf(10.f, gain_db / 20.f);
    g_agc_now = to;
    for (size_t i = 0; i < OPUSENC_FRAME; i++) {
        float g = from + (to - from) * (float)i / OPUSENC_FRAME;
        int v = (int)lrintf(g_prev[i] * g);
        int a = v < 0 ? -v : v;
        if (a > KNEE) a = KNEE + (a - KNEE) / 4;
        if (a > 32767) a = 32767;
        g_frame[i] = (int16_t)(v < 0 ? -a : a);  // (g_frame is done with: it carries the frame out)
    }
    // and this one waits its turn
    memcpy(g_prev, f, sizeof(g_prev));
    g_prev_db = raw;
    g_meter_db = (int)raw, g_meter_threshold = (int)g_det.threshold(), g_meter_gain = (int)gain_db;
    float db = levelDb(g_frame, OPUSENC_FRAME);
    mutexLock(&g_lock);
    g_stats.frames++;
    g_stats.floor_db = (int)g_det.floor_db;
    g_stats.gain_db = (int)gain_db;
    g_stats.clicks += g_det.clicks - clicks;
    if (talk && (int)db > g_stats.peak_db) g_stats.peak_db = (int)db;
    if (talk) g_stats.talking++;
    mutexUnlock(&g_lock);
    g_level = db <= -96 ? MIC_SILENT : (uint8_t)(-db);
    g_speaking = talk;
    if (talk) {
        if (!was) opusencInit(&g_enc);  // (each stretch of talk from a clean start: the decoder's starts clean too)
        static uint8_t pkt[MIC_PACKET_BYTES];
        opusencFrame(&g_enc, g_frame, pkt, MIC_PACKET_BYTES);
        push(pkt, MIC_PACKET_BYTES, false);
        g_silence_left = SILENCE_FRAMES;
    } else if (g_silence_left > 0) {
        static const uint8_t SILENCE[3] = {0xF8, 0xFF, 0xFE};
        g_silence_left--;
        push(SILENCE, sizeof(SILENCE), g_silence_left == 0);
    }
    uint64_t us = armTicksToNs(cpuTicks() - t0) / 1000;
    mutexLock(&g_lock);
    g_stats.busy_us += (uint32_t)us;
    mutexUnlock(&g_lock);
}

bool open() {
    if (g_open) return true;
    Result rc = audinInitialize();
    if (R_SUCCEEDED(rc) && (audinGetPcmFormat() != PcmFormat_Int16 || audinGetSampleRate() != 48000)) {
        audinExit();
        rc = 1;
    }
    for (int i = 0; R_SUCCEEDED(rc) && i < BUFS; i++) {
        g_bufs[i] = AudioInBuffer{};
        g_bufs[i].buffer = g_mem[i];
        g_bufs[i].buffer_size = BUF_BYTES;
        g_bufs[i].data_size = BUF_BYTES;
        rc = audinAppendAudioInBuffer(&g_bufs[i]);
    }
    if (R_SUCCEEDED(rc)) rc = audinStartAudioIn();
    if (R_FAILED(rc)) {
        if (rc != 1) audinExit();
        mutexLock(&g_lock);
        g_stats.open_failed++;
        g_stats.last_rc = rc;
        mutexUnlock(&g_lock);
        return false;
    }
    g_channels = audinGetChannelCount();
    if (g_channels < 1 || g_channels > 2) g_channels = 2;
    g_open = true;
    g_have = 0;
    g_det = Detector{};  // (a fresh start each time the mic opens)
    g_hp = HighPass();
    g_denoising = false;
    memset(g_prev, 0, sizeof(g_prev));
    g_prev_db = -96;
    g_reopened = true;
    g_silence_left = 0;
    return true;
}

void close() {
    if (!g_open) return;
    audinStopAudioIn();
    audinExit();
    g_open = false;
    g_speaking = false;
    g_level = MIC_SILENT;
    if (g_det.on || g_silence_left) {  // (cut off mid-talk: say so, as stopping does)
        static const uint8_t SILENCE[3] = {0xF8, 0xFF, 0xFE};
        push(SILENCE, sizeof(SILENCE), true);
    }
    g_det = Detector{};
    g_silence_left = 0;
}

void micThread(void*) {
    while (g_run) {
        if (!g_call || g_muted) {
            close();
            svcSleepThread(100'000'000LL);
            continue;
        }
        if (!open()) {
            svcSleepThread(2'000'000'000LL);
            continue;
        }
        AudioInBuffer* rel = nullptr;
        u32 count = 0;
        if (R_FAILED(audinWaitCaptureFinish(&rel, &count, 100'000'000ULL)) || !count || !rel) continue;
        size_t n = rel->data_size < BUF_BYTES ? rel->data_size : BUF_BYTES;
        const int16_t* s = (const int16_t*)rel->buffer;
        size_t samples = n / 2 / g_channels;
        for (size_t i = 0; i < samples; i++) {
            int v = g_channels == 2 ? (s[2 * i] + s[2 * i + 1]) / 2 : s[i];
            g_frame[g_have++] = (int16_t)v;
            if (g_have == OPUSENC_FRAME) {
                frame();
                g_have = 0;
            }
        }
        rel->data_size = BUF_BYTES;
        audinAppendAudioInBuffer(rel);
    }
    close();
}

}  // namespace

void start() {
    g_start_tick = armGetSystemTick();
    mutexInit(&g_lock);
    resetStats();
    g_run = true;
    // (just under the mixer, above everything else: it waits on audin most of the time and takes little, but
    // a frame it's late for is lost, and the gaps make a voice robotic. Below the capture and stream threads,
    // as it was, it lost 15-20% of frames while sharing)
    if (R_FAILED(threadCreate(&g_thread, micThread, nullptr, nullptr, 0x6000, 0x24, -2))) {
        g_run = false;
        return;
    }
    threadStart(&g_thread);
}

void stop() {
    if (!g_run) return;
    g_run = false;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
}

void setCall(bool on) {
    g_call = on;
    if (!on) {
        mutexLock(&g_lock);
        g_count = 0;
        mutexUnlock(&g_lock);
    }
}

void setMuted(bool on) { g_muted = on; }

bool take(Packet* p) {
    mutexLock(&g_lock);
    bool got = g_count > 0;
    if (got) {
        *p = g_queue[g_head];
        g_head = (g_head + 1) % QUEUE;
        g_count--;
    }
    mutexUnlock(&g_lock);
    return got;
}

bool active() { return g_open || g_count > 0; }

void setSettings(int volume, int sensitivity_db, bool agc, bool denoise) {
    g_denoise_on = denoise;
    g_volume = volume < 0 ? 0 : volume > 200 ? 200 : volume;
    g_sensitivity = sensitivity_db >= 0 ? MIC_SENSITIVITY_AUTO : sensitivity_db < -96 ? -96 : sensitivity_db;
    g_agc_on = agc;
}

bool meter(int* level_db, int* threshold_db, int* gain_db) {
    *level_db = g_meter_db, *threshold_db = g_meter_threshold, *gain_db = g_meter_gain;
    return g_open;
}
bool speaking() { return g_speaking; }
uint8_t level() { return g_level; }

void takeStats(Stats* out) {
    mutexLock(&g_lock);
    *out = g_stats;
    out->open = g_open;
    resetStats();
    mutexUnlock(&g_lock);
}
#endif

}  // namespace mic
