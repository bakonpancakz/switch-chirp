#include "sounds.h"

#include <stdio.h>
#include <string.h>

#ifdef __SWITCH__
#include "app.h"
#include "card.h"
#include "chirp.h"
#include "mixer.h"
#endif

namespace sounds {

namespace {

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

const char* const NAMES[KINDS] = {"voice_join", "voice_leave", "screenshare_join", "screenshare_leave",
                                  "stream_start", "stream_stop", "mute", "unmute", "deafen", "undeafen"};

}  // namespace

const char* name(Kind k) { return k < KINDS ? NAMES[k] : "?"; }

bool wavInfo(const uint8_t* h, size_t n, WavInfo* out) {
    if (n < 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    bool fmt = false;
    *out = WavInfo{};
    // the chunks after "WAVE": "fmt " says what the samples are, "data" holds them (others skipped)
    for (size_t at = 12; at + 8 <= n;) {
        uint32_t size = le32(h + at + 4);
        const uint8_t* body = h + at + 8;
        if (!memcmp(h + at, "fmt ", 4)) {
            if (at + 8 + 16 > n || size < 16) return false;
            uint16_t format = le16(body), channels = le16(body + 2), bits = le16(body + 14);
            uint32_t rate = le32(body + 4);
            // (1: PCM; 0xFFFE: "extensible", whose sub-format is PCM's GUID, starting 01 00)
            bool pcm = format == 1 || (format == 0xFFFE && size >= 40 && at + 8 + 26 <= n && le16(body + 24) == 1);
            if (!pcm || bits != 16 || channels < 1 || channels > 2 || rate < 8000 || rate > 48000) return false;
            out->rate = (int)rate, out->channels = channels;
            fmt = true;
        } else if (!memcmp(h + at, "data", 4)) {
            if (!fmt) return false;
            out->data_at = at + 8;
            out->data_bytes = size;
            return true;
        }
        at += 8 + size + (size & 1);  // (chunks are padded to an even size)
    }
    return false;
}

void toMono(const uint8_t* in, size_t frames, int channels, int16_t* out) {
    for (size_t i = 0; i < frames; i++) {
        if (channels == 2) {
            int l = (int16_t)le16(in + 4 * i), r = (int16_t)le16(in + 4 * i + 2);
            out[i] = (int16_t)((l + r) / 2);
        } else {
            out[i] = (int16_t)le16(in + 2 * i);
        }
    }
}

void fadeOut(int16_t* pcm, size_t n, int rate, int ms) {
    size_t k = (size_t)rate * ms / 1000;
    if (k > n) k = n;
    for (size_t i = 0; i < k; i++) pcm[n - k + i] = (int16_t)(pcm[n - k + i] * (int)(k - 1 - i) / (int)k);
}

#ifdef __SWITCH__
namespace {
int16_t g_pcm[SOUNDS_MAX];
uint8_t g_io[4096];
bool g_said_missing[KINDS];  // (a missing file said once in the log, not every time)
}  // namespace

void load(Kind k) {
    if (k >= KINDS) return;
    char path[96], line[160];
    snprintf(path, sizeof(path), CHIRP_SOUNDS "/%s.wav", name(k));
    Card c;
    if (!c.ok) return;
    FILE* f = fopen(path, "rb");
    if (!f) {
        if (!g_said_missing[k]) {
            snprintf(line, sizeof(line), "sounds: no %s.wav: that one's silent", name(k));
            app::log(line);
            g_said_missing[k] = true;
        }
        return;
    }
    g_said_missing[k] = false;
    size_t got = fread(g_io, 1, sizeof(g_io), f);
    WavInfo w;
    if (!wavInfo(g_io, got, &w)) {
        fclose(f);
        snprintf(line, sizeof(line), "sounds: %s.wav isn't one this can play (16-bit PCM, mono or stereo, 8 to 48 kHz)", name(k));
        app::log(line);
        return;
    }
    mixer::stopSound();  // (the buffer's free once this returns)
    size_t frame_bytes = 2 * (size_t)w.channels, total = w.data_bytes / frame_bytes, n = 0;
    bool cut = total > SOUNDS_MAX;
    if (cut) total = SOUNDS_MAX;
    if (fseek(f, (long)w.data_at, SEEK_SET) == 0) {
        while (n < total) {
            size_t want = (total - n) * frame_bytes;
            if (want > sizeof(g_io)) want = sizeof(g_io) / frame_bytes * frame_bytes;
            size_t r = fread(g_io, 1, want, f);
            if (r < frame_bytes) break;
            toMono(g_io, r / frame_bytes, w.channels, g_pcm + n);
            n += r / frame_bytes;
        }
    }
    fclose(f);
    if (cut) fadeOut(g_pcm, n, w.rate, 20);
    if (n) mixer::playSound(g_pcm, n, w.rate, throughDeaf(k));
}
#endif

}  // namespace sounds
