#include "daveframe.h"

#include <string.h>

#include "mlskeys.h"

bool daveBaseSecret(const uint8_t exporter_secret[MLS_NH], uint64_t user, uint8_t out[DAVE_KEY]) {
    uint8_t ctx[8];
    for (int i = 0; i < 8; i++) ctx[i] = (uint8_t)(user >> (8 * i));  // (little-endian, unlike the credential)
    return mlsExport(exporter_secret, DAVE_LABEL, strlen(DAVE_LABEL), ctx, 8, out, DAVE_KEY);
}

bool DaveRatchet::init(uint64_t user_id, const uint8_t base[DAVE_KEY]) {
    // (MLS's hash ratchet starts from the base secret as is: here 16 bytes, not Nh)
    wipeAll();
    user = user_id;
    memcpy(secret, base, DAVE_KEY);
    secret_n = DAVE_KEY;
    next_gen = 0;
    has_key = false;
    return true;
}

const Gcm* DaveRatchet::forGeneration(uint32_t gen) {
    if (has_key && key_gen == gen) return &key;
    if (gen < next_gen) return nullptr;  // (passed: its key is gone, as the ratchet only goes forward)
    while (next_gen <= gen) {
        uint8_t k[DAVE_KEY], next[MLS_NH];
        // key = DeriveTreeSecret(secret, "key", generation, 16); secret' = DeriveTreeSecret(secret, "secret", generation, 32)
        if (!mlsDeriveTreeSecretN(secret, secret_n, "key", next_gen, k, DAVE_KEY) ||
            !mlsDeriveTreeSecretN(secret, secret_n, "secret", next_gen, next, MLS_NH))
            return nullptr;
        if (next_gen == gen) {
            key.init(k, DAVE_KEY);
            key_gen = gen;
            has_key = true;
        }
        memcpy(secret, next, MLS_NH);
        secret_n = MLS_NH;
        next_gen++;
        wipe(k, sizeof(k));
        wipe(next, sizeof(next));
    }
    return &key;
}

void DaveRatchet::wipeAll() { wipe(this, sizeof(*this)); }

bool daveIsSilence(const uint8_t* p, size_t n) { return n == 3 && p[0] == 0xF8 && p[1] == 0xFF && p[2] == 0xFE; }

namespace {

void nonce12(uint32_t counter, uint8_t n[12]) {
    memset(n, 0, 12);
    for (int i = 0; i < 4; i++) n[8 + i] = (uint8_t)(counter >> (8 * i));
}

}  // namespace

bool daveDecrypt(DaveRatchet& r, const uint8_t* frame, size_t n, uint8_t* out, size_t* out_n) {
    // from the end: the marker, the supplemental size, then the tag, nonce and ranges it covers
    if (n < DAVE_TAG + 1 + 2 + 1 || frame[n - 1] != 0xFA || frame[n - 2] != 0xFA) return false;
    size_t supp = frame[n - 3];
    if (supp < DAVE_TAG + 1 + 2 + 1 || supp > n) return false;
    const uint8_t* tag = frame + n - supp;
    const uint8_t* p = tag + DAVE_TAG;
    const uint8_t* end = frame + n - 3;
    uint64_t counter = 0;
    int shift = 0;
    for (;;) {
        if (p >= end || shift > 28) return false;
        uint8_t b = *p++;
        counter |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    if (counter > 0xffffffffull || p != end) return false;  // (ranges: an Opus frame has none)
    size_t ct_n = n - supp;
    const Gcm* g = r.forGeneration((uint32_t)(counter >> 24));
    if (!g) return false;
    uint8_t iv[12];
    nonce12((uint32_t)counter, iv);
    if (!g->open(iv, nullptr, 0, frame, ct_n, tag, DAVE_TAG, out)) return false;
    *out_n = ct_n;
    return true;
}

bool daveEncrypt(DaveRatchet& r, uint32_t* counter, const uint8_t* opus, size_t n, uint8_t* out, size_t cap, size_t* out_n) {
    uint32_t c = ++*counter;
    const Gcm* g = r.forGeneration(c >> 24);
    if (!g || cap < n + DAVE_SUPPLEMENTAL_MAX) return false;
    uint8_t iv[12];
    nonce12(c, iv);
    g->seal(iv, nullptr, 0, opus, n, out, out + n, DAVE_TAG);
    size_t at = n + DAVE_TAG;
    uint32_t v = c;
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        out[at++] = (uint8_t)(b | (v ? 0x80 : 0));
    } while (v);
    size_t supp = at - n + 1 + 2;
    out[at++] = (uint8_t)supp;
    out[at++] = 0xFA;
    out[at++] = 0xFA;
    *out_n = at;
    return true;
}

// --- video: H.264 ---

namespace {

// The next start code (00 00 01, or 00 00 00 01) at or after `from`: where it begins, and its length
bool nextStartCode(const uint8_t* p, size_t n, size_t from, size_t* at, size_t* len) {
    for (size_t i = from; i + 3 <= n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            bool four = i > from && p[i - 1] == 0;
            *at = four ? i - 1 : i;
            *len = four ? 4 : 3;
            return true;
        }
    }
    return false;
}

// How many bytes of a slice's payload (after its 1-byte NAL header) cover its first three ue(v)
// values, the third being pic_parameter_set_id: the depacketizer reads that, so it stays readable.
// (Emulation-prevention bytes, 00 00 03, are passed over when reading bits, and counted in the size)
size_t bytesCoveringPps(const uint8_t* p, size_t n) {
    size_t byte = 0;
    int bit = 0, zeros = 0;
    auto readBit = [&](int* out) {
        if (byte >= n) return false;
        if (bit == 0 && byte >= 2 && p[byte] == 3 && p[byte - 1] == 0 && p[byte - 2] == 0) {
            if (++byte >= n) return false;
        }
        *out = (p[byte] >> (7 - bit)) & 1;
        if (++bit == 8) bit = 0, byte++;
        return true;
    };
    for (int v = 0; v < 3; v++) {
        int b = 0;
        zeros = 0;
        while (readBit(&b) && b == 0)
            if (++zeros > 31) return n;
        for (int i = 0; i < zeros; i++)
            if (!readBit(&b)) return n;
    }
    size_t covered = byte + (bit ? 1 : 0);
    return covered < n ? covered : n;
}

}  // namespace

bool daveEncryptH264(DaveRatchet& r, uint32_t* counter, uint8_t* buf, size_t n, size_t cap, size_t* out_n) {
    // every start code 4 bytes (the receiver's WebRTC makes them so, and the ranges count from that)
    size_t at = 0, len = 0;
    if (!nextStartCode(buf, n, 0, &at, &len) || at != 0) return false;
    for (size_t from = 0; nextStartCode(buf, n, from, &at, &len);) {
        if (len == 3) {
            if (n + 1 > cap) return false;
            memmove(buf + at + 1, buf + at, n - at);
            buf[at] = 0;
            n++;
        }
        from = at + 4;
    }
    // what stays readable: [offset, size) ranges, in order, touching ones joined
    constexpr int MAX_RANGES = 64;
    size_t off[MAX_RANGES], size[MAX_RANGES];
    int ranges = 0;
    auto keep = [&](size_t o, size_t s) {
        if (!s) return true;
        if (ranges && off[ranges - 1] + size[ranges - 1] == o) {
            size[ranges - 1] += s;
            return true;
        }
        if (ranges == MAX_RANGES) return false;
        off[ranges] = o, size[ranges] = s, ranges++;
        return true;
    };
    for (size_t from = 0; nextStartCode(buf, n, from, &at, &len);) {
        size_t nal = at + 4, end = n, next = 0, nlen = 0;
        if (nextStartCode(buf, n, nal, &next, &nlen)) end = next;
        if (nal >= end) {
            if (!keep(at, end - at)) return false;
        } else {
            int type = buf[nal] & 0x1f;
            size_t readable = end - at;  // (anything but a slice: all of it)
            if (type == 1 || type == 5) readable = 4 + 1 + bytesCoveringPps(buf + nal + 1, end - nal - 1);
            if (!keep(at, readable)) return false;
        }
        from = end;
    }
    // the trailer's room: tag, nonce (5 at most), the ranges (2 ULEB128s each, 5 bytes at most each), size, marker
    if (n + DAVE_TAG + 5 + ranges * 10 + 3 > cap) return false;
    auto gaps = [&](auto&& fn) {  // (each encrypted stretch, in order)
        size_t pos = 0;
        for (int i = 0; i <= ranges; i++) {
            size_t stop = i < ranges ? off[i] : n;
            if (stop > pos) fn(buf + pos, stop - pos);
            if (i < ranges) pos = off[i] + size[i];
        }
    };
    for (int attempt = 0; attempt < 10; attempt++) {
        uint32_t c = ++*counter;
        const Gcm* g = r.forGeneration(c >> 24);
        if (!g) return false;
        uint8_t iv[12];
        nonce12(c, iv);
        GcmStream st;
        st.start(*g, iv);
        for (int i = 0; i < ranges; i++) st.aad(buf + off[i], size[i]);
        gaps([&](uint8_t* p, size_t k) { st.encrypt(p, k); });
        size_t t = n;
        st.finish(buf + t, DAVE_TAG);
        t += DAVE_TAG;
        auto uleb = [&](uint32_t v) {
            do {
                uint8_t b = v & 0x7f;
                v >>= 7;
                buf[t++] = (uint8_t)(b | (v ? 0x80 : 0));
            } while (v);
        };
        uleb(c);
        for (int i = 0; i < ranges; i++) uleb((uint32_t)off[i]), uleb((uint32_t)size[i]);
        size_t supp = t - n + 3;
        if (supp > 255) return false;  // (its size is one byte: a frame with this many ranges can't be sent)
        buf[t++] = (uint8_t)supp;
        buf[t++] = 0xFA;
        buf[t++] = 0xFA;
        // no start code may appear but the frame's own (the packetizer splits on them)
        bool clean = true;
        size_t want = 0;
        for (size_t from = 0; clean && nextStartCode(buf, t, from, &at, &len);) {
            while (want < (size_t)ranges && off[want] + size[want] <= at) want++;
            bool ours = len == 4 && want < (size_t)ranges && at >= off[want] && at + 4 <= off[want] + size[want] &&
                        buf[at] == 0 && buf[at + 1] == 0 && buf[at + 2] == 0 && buf[at + 3] == 1;
            if (!ours) clean = false;
            from = at + len;
        }
        if (clean) {
            *out_n = t;
            return true;
        }
        // undo (the same keystream again) and try the next nonce
        st.start(*g, iv);
        gaps([&](uint8_t* p, size_t k) { st.decrypt(p, k); });
    }
    return false;
}
