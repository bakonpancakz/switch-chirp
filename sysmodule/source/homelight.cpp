#include "homelight.h"

#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "app.h"

namespace homelight {
namespace {

constexpr int PADS = 16;
constexpr u64 REFRESH_NS = 10'000'000'000ULL;  // sent again this often while on
constexpr u64 TIMEOUT_NS = 15'000'000'000ULL;  // and goes out by itself this long after the last

volatile int g_percent = 50;
bool g_ready = false, g_failed = false;  // (hid:sys opened; or it couldn't be: the light's left alone)
bool g_lit = false;
int g_lit_percent = 0;
u64 g_sent_at = 0;
HidsysUniquePadId g_pads[PADS];  // (the ones lit: the same ones are put out)
s32 g_pads_n = 0;
int g_said = 0;  // (the first few times it lights, the log says how it went)

bool ready() {
    if (g_ready) return true;
    if (g_failed) return false;
    Result rc = hidsysInitialize();
    if (R_FAILED(rc)) {
        char line[96];
        snprintf(line, sizeof(line), "home light: hid:sys didn't open (0x%X): no light", rc);
        app::log(line);
        g_failed = true;
        return false;
    }
    g_ready = true;
    return true;
}

// Steady at the brightness (0..15): two mini cycles at that intensity, repeated forever (the shape of
// libnx's example's breathing pattern, flat). (A cycle time of 0 means off, so it can't be left out.)
void pattern(HidsysNotificationLedPattern* p, int intensity) {
    memset(p, 0, sizeof(*p));
    if (intensity <= 0) return;  // (all zero: off)
    p->baseMiniCycleDuration = 0x8;  // 100 ms
    p->totalMiniCycles = 0x1;        // two
    p->totalFullCycles = 0x0;        // forever
    p->startIntensity = (u8)intensity;
    for (int i = 0; i < 2; i++) {
        p->miniCycles[i].ledIntensity = (u8)intensity;
        p->miniCycles[i].transitionSteps = 0x1;
        p->miniCycles[i].finalStepDuration = 0xF;
    }
}

// Every controller connected: each player's (a pair of Joy-Cons is two) and the handheld's
void findPads() {
    static const HidNpadIdType IDS[9] = {HidNpadIdType_Handheld, HidNpadIdType_No1, HidNpadIdType_No2,
                                         HidNpadIdType_No3,      HidNpadIdType_No4, HidNpadIdType_No5,
                                         HidNpadIdType_No6,      HidNpadIdType_No7, HidNpadIdType_No8};
    g_pads_n = 0;
    for (int k = 0; k < 9 && g_pads_n < PADS; k++) {
        HidsysUniquePadId ids[2];
        s32 n = 0;
        if (R_FAILED(hidsysGetUniquePadsFromNpad(IDS[k], ids, 2, &n))) continue;
        for (s32 i = 0; i < n && i < 2 && g_pads_n < PADS; i++) g_pads[g_pads_n++] = ids[i];
    }
}

void send(bool on, int percent) {
    HidsysNotificationLedPattern p;
    int level = (percent * 15 + 50) / 100;
    pattern(&p, on ? (level < 1 ? 1 : level) : 0);  // (1..15: any percent at all is at least the dimmest)
    if (on) findPads();  // (who's connected now: a controller may have come or gone since)
    Result first = 0, plain = 0;
    for (s32 i = 0; i < g_pads_n; i++) {
        // (with a timeout from 9.0.0, the controllers' own firmware too; else plainly)
        Result rc = on ? hidsysSetNotificationLedPatternWithTimeout(&p, g_pads[i], TIMEOUT_NS) : (Result)1;
        if (on && i == 0) first = rc;
        if (R_SUCCEEDED(rc)) continue;
        rc = hidsysSetNotificationLedPattern(&p, g_pads[i]);
        if (R_FAILED(rc)) plain = rc;
    }
    if (on && g_said < 3) {
        char line[160];
        snprintf(line, sizeof(line), "home light: on at %d%% on %d controller(s); with a timeout 0x%X, else 0x%X", percent,
                 (int)g_pads_n, first, plain);
        app::log(line);
        g_said++;
    }
    if (!on) g_pads_n = 0;
    g_lit = on, g_lit_percent = percent;
    g_sent_at = armTicksToNs(armGetSystemTick());
}

}  // namespace

void setBrightness(int percent) { g_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent; }

void update(bool talk) {
    int percent = g_percent;
    u64 now = armTicksToNs(armGetSystemTick());
    bool want = talk && percent > 0;  // (exactly as the mic decides: pauses between words show)
    if (!want && !g_lit) return;
    if (!ready()) return;
    if (want != g_lit || (want && percent != g_lit_percent) ||
        (want && now - g_sent_at >= REFRESH_NS))
        send(want, percent);
}

}  // namespace homelight
