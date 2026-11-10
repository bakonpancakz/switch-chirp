#include "avatar.h"

#include <png.h>
#include <setjmp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <switch.h>

#include "chirp.h"

namespace avatar {
namespace {

constexpr size_t MAX_FILE = 256 << 10;
constexpr u64 RETRY_NS = 2'000'000'000ULL;  // not on the card yet: looked for again this often
constexpr size_t KEEP = 96;                 // pictures kept at once (2 KB each)

struct Entry {
    std::vector<uint8_t> px;  // empty: not decoded (yet)
    u64 tried_ns = 0;
};
std::map<std::string, Entry> g_cache;

// Rows of RGB box-filtered down to SIZE x SIZE (the middle square), as RGBA4444
class Shrink {
public:
    Shrink(int w, int h, std::vector<uint8_t>& out) : w_(w), h_(h), out_(out) {
        side_ = w < h ? w : h;
        x0_ = (w - side_) / 2;
        y0_ = (h - side_) / 2;
        acc_.assign(SIZE * 3, 0);
        cnt_.assign(SIZE, 0);
        out_.assign(SIZE * SIZE * 2, 0);
    }
    void row(int y, const uint8_t* rgb) {
        if (y < y0_ || y >= y0_ + side_) return;
        int oy = (int)((int64_t)(y - y0_) * SIZE / side_);
        if (oy != cur_) flush(), cur_ = oy;
        for (int x = 0; x < side_; x++) {
            int ox = (int)((int64_t)x * SIZE / side_);
            const uint8_t* p = rgb + 3 * (x0_ + x);
            acc_[3 * ox] += p[0], acc_[3 * ox + 1] += p[1], acc_[3 * ox + 2] += p[2];
            cnt_[ox]++;
        }
    }
    void finish() { flush(); }

private:
    void flush() {
        if (cur_ < 0) return;
        for (int ox = 0; ox < SIZE; ox++) {
            uint32_t c = cnt_[ox] ? cnt_[ox] : 1;
            uint8_t r = (uint8_t)(acc_[3 * ox] / c), g = (uint8_t)(acc_[3 * ox + 1] / c), b = (uint8_t)(acc_[3 * ox + 2] / c);
            uint8_t* o = &out_[((size_t)cur_ * SIZE + ox) * 2];
            o[0] = (uint8_t)((r & 0xF0) | (g >> 4));
            o[1] = (uint8_t)((b & 0xF0) | 0x0F);
        }
        std::fill(acc_.begin(), acc_.end(), 0);
        std::fill(cnt_.begin(), cnt_.end(), 0);
        cur_ = -1;
    }
    int w_, h_, side_, x0_, y0_, cur_ = -1;
    std::vector<uint32_t> acc_, cnt_;
    std::vector<uint8_t>& out_;
};

struct MemRead {
    const uint8_t* p;
    size_t left;
};

void pngRead(png_structp png, png_bytep dst, png_size_t n) {
    MemRead* m = (MemRead*)png_get_io_ptr(png);
    if (n > m->left) png_error(png, "short");
    memcpy(dst, m->p, n);
    m->p += n;
    m->left -= n;
}

// (only plain data across setjmp: libpng's errors longjmp back here)
bool decodePng(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) return false;
    png_infop info = png_create_info_struct(png);
    uint8_t* volatile row = nullptr;
    Shrink* volatile shrink = nullptr;
    volatile bool ok = false;
    MemRead m = {data, size};
    if (info && !setjmp(png_jmpbuf(png))) {
        png_set_read_fn(png, &m, pngRead);
        png_read_info(png, info);
        png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
        if (png_get_interlace_type(png, info) == PNG_INTERLACE_NONE && w && h && w <= 1024 && h <= 1024) {
            png_set_expand(png);
            png_set_strip_16(png);
            png_set_strip_alpha(png);
            png_set_gray_to_rgb(png);
            png_read_update_info(png, info);
            if (png_get_rowbytes(png, info) == (size_t)w * 3) {
                row = (uint8_t*)malloc((size_t)w * 3);
                shrink = new Shrink((int)w, (int)h, out);
                for (png_uint_32 y = 0; y < h; y++) {
                    png_read_row(png, row, nullptr);
                    shrink->row((int)y, row);
                }
                shrink->finish();
                ok = true;
            }
        }
    }
    delete shrink;
    free(row);
    png_destroy_read_struct(&png, info ? &info : nullptr, nullptr);
    return ok;
}

bool load(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = false;
    if (size > 8 && (size_t)size <= MAX_FILE) {
        std::vector<uint8_t> data((size_t)size);
        if (fread(data.data(), 1, data.size(), f) == data.size() && !memcmp(data.data(), "\x89PNG", 4))
            ok = decodePng(data.data(), data.size(), out);
    }
    fclose(f);
    return ok;
}

// Round: what's outside the circle see-through, its edge half so
void cutRound(std::vector<uint8_t>& px) {
    const float r = SIZE / 2.0f;
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            float dx = x + 0.5f - r, dy = y + 0.5f - r;
            float d2 = dx * dx + dy * dy;
            uint8_t a = d2 <= (r - 1) * (r - 1) ? 0xF : d2 <= r * r ? 0x8 : 0x0;
            uint8_t& lo = px[(size_t)(y * SIZE + x) * 2 + 1];
            lo = (uint8_t)((lo & 0xF0) | a);
        }
    }
}

}  // namespace

const uint8_t* get(const char* file) {
    if (!file || !file[0]) return nullptr;
    auto it = g_cache.find(file);
    if (it == g_cache.end()) {
        if (g_cache.size() >= KEEP) g_cache.clear();  // (a lot of people seen: start over)
        it = g_cache.emplace(file, Entry{}).first;
    }
    Entry& e = it->second;
    if (!e.px.empty()) return e.px.data();
    u64 now = armTicksToNs(armGetSystemTick());
    if (e.tried_ns && now - e.tried_ns < RETRY_NS) return nullptr;
    e.tried_ns = now;
    std::vector<uint8_t> px;
    if (!load(std::string(CHIRP_AVATARS "/") + file + ".png", px)) return nullptr;
    cutRound(px);
    e.px = std::move(px);
    return e.px.data();
}

}  // namespace avatar
