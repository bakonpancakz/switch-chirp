#include "icons.h"

#include <cmath>
#include <map>
#include <vector>

namespace icons {
namespace {

constexpr float PI = 3.14159265f;
constexpr int SAMPLES = 4;  // a side: 16 samples a pixel

// Inside a w x h rectangle at (x, y) with corners of radius rad
bool inRoundedRect(float px, float py, float x, float y, float w, float h, float rad) {
    if (px < x || py < y || px > x + w || py > y + h) return false;
    float cx = px < x + rad ? x + rad : px > x + w - rad ? x + w - rad : px;
    float cy = py < y + rad ? y + rad : py > y + h - rad ? y + h - rad : py;
    float dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= rad * rad;
}

// On a band `thick` wide around the circle at (cx, cy), between angles a0
// and a1 (degrees, 0 to the right, going clockwise on screen)
bool onArc(float px, float py, float cx, float cy, float radius, float thick, float a0, float a1) {
    float dx = px - cx, dy = py - cy;
    float d = std::sqrt(dx * dx + dy * dy);
    if (std::fabs(d - radius) > thick / 2) return false;
    float a = std::atan2(dy, dx) * 180.0f / PI;
    if (a < 0) a += 360.0f;
    return a >= a0 && a <= a1;
}

// How far (px, py) is from the slash, top left to bottom right
float fromSlash(float px, float py) {
    const float x0 = 3, y0 = 2.5f, x1 = 17, y1 = 17.5f;
    float vx = x1 - x0, vy = y1 - y0;
    float t = ((px - x0) * vx + (py - y0) * vy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float dx = px - (x0 + t * vx), dy = py - (y0 + t * vy);
    return std::sqrt(dx * dx + dy * dy);
}

bool shape(Kind k, float px, float py) {
    if (k == MicOff) {
        return inRoundedRect(px, py, 7, 1, 6, 11, 3)            // the capsule
               || onArc(px, py, 10, 8, 6, 1.8f, 0, 180)         // its holder, under it
               || inRoundedRect(px, py, 9, 14, 2, 3.5f, 0)      // the stem
               || inRoundedRect(px, py, 6, 17, 8, 2, 1);        // the foot
    }
    return onArc(px, py, 10, 11, 8, 2, 180, 360)                // the band, over the top
           || inRoundedRect(px, py, 1, 10, 5, 8, 2)             // the cups
           || inRoundedRect(px, py, 14, 10, 5, 8, 2);
}

// What's at (px, py): 2 the slash, 1 the shape (outside the gap around the slash), 0 nothing
int covered(Kind k, float px, float py) {
    float d = fromSlash(px, py);
    if (d <= 1.1f) return 2;
    if (d <= 2.6f) return 0;  // (the gap)
    return shape(k, px, py) ? 1 : 0;
}

std::map<uint32_t, std::vector<uint8_t>> g_made;

}  // namespace

const uint8_t* get(Kind k, Rgb body, Rgb slash) {
    auto pack = [](Rgb c) { return (uint32_t)(c.r & 15) << 8 | (uint32_t)(c.g & 15) << 4 | (c.b & 15); };
    uint32_t key = (uint32_t)k << 24 | pack(body) << 12 | pack(slash);
    auto it = g_made.find(key);
    if (it != g_made.end()) return it->second.data();
    std::vector<uint8_t> px((size_t)ICON * ICON * 2);
    for (int y = 0; y < ICON; y++) {
        for (int x = 0; x < ICON; x++) {
            int n[3] = {0, 0, 0};
            for (int sy = 0; sy < SAMPLES; sy++)
                for (int sx = 0; sx < SAMPLES; sx++) n[covered(k, x + (sx + 0.5f) / SAMPLES, y + (sy + 0.5f) / SAMPLES)]++;
            // (the gap keeps the two apart: a pixel takes the colour most of it is)
            Rgb c = n[2] >= n[1] ? slash : body;
            int all = n[1] + n[2];
            uint8_t a = (uint8_t)((all * 15 + SAMPLES * SAMPLES / 2) / (SAMPLES * SAMPLES));
            px[(size_t)(y * ICON + x) * 2] = (uint8_t)((c.r & 15) << 4 | (c.g & 15));
            px[(size_t)(y * ICON + x) * 2 + 1] = (uint8_t)((c.b & 15) << 4 | a);
        }
    }
    return g_made.emplace(key, std::move(px)).first->second.data();
}

}  // namespace icons
