#include "rtp.h"

#include <string.h>

#include "aes.h"
#include "chacha.h"

namespace {

uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

bool open(RtpMode mode, const uint8_t key[RTP_KEY], const uint8_t* nonce4, const uint8_t* aad, size_t aad_n, const uint8_t* ct, size_t n,
          const uint8_t* tag, uint8_t* out) {
    if (mode == Rtp_Aes256Gcm) {
        uint8_t iv[GCM_IV] = {0};
        memcpy(iv, nonce4, 4);
        // (the key schedule is a few hundred cycles: kept per call, so nothing of the key stays around)
        Gcm g;
        g.init(key, RTP_KEY);
        bool ok = g.open(iv, aad, aad_n, ct, n, tag, GCM_TAG, out);
        g.wipeKey();
        return ok;
    }
    if (mode == Rtp_XChaCha) {
        uint8_t iv[XCHACHA_NONCE] = {0};
        memcpy(iv, nonce4, 4);
        return xchachaOpen(key, iv, aad, aad_n, ct, n, tag, out);
    }
    return false;
}

}  // namespace

bool rtpOpen(RtpMode mode, const uint8_t key[RTP_KEY], const uint8_t* pkt, size_t n, uint8_t* out, RtpPacket* p) {
    if (n < RTP_HEADER + 16 + 4 || (pkt[0] >> 6) != 2 || (pkt[1] & 0x7f) != RTP_OPUS) return false;
    bool padding = (pkt[0] & 0x20) != 0, extension = (pkt[0] & 0x10) != 0;
    size_t header = RTP_HEADER + 4u * (pkt[0] & 0x0f);
    size_t ext_words = 0;
    if (extension) {
        if (n < header + 4) return false;
        ext_words = be16(pkt + header + 2);
        header += 4;  // (the preamble is part of the authenticated header; its body is encrypted)
    }
    if (n < header + 16 + 4) return false;
    size_t ct_n = n - header - 16 - 4;
    if (!open(mode, key, pkt + n - 4, pkt, header, pkt + header, ct_n, pkt + n - 20, out)) return false;
    size_t start = 4 * ext_words, end = ct_n;
    if (padding && end > 0) {
        size_t pad = out[end - 1];
        if (pad < end) end -= pad;
    }
    if (start > end) return false;
    p->seq = be16(pkt + 2);
    p->timestamp = be32(pkt + 4);
    p->ssrc = be32(pkt + 8);
    p->payload = out + start;
    p->payload_n = end - start;
    return true;
}

size_t rtcpOpen(RtpMode mode, const uint8_t key[RTP_KEY], const uint8_t* pkt, size_t n, uint8_t* out) {
    constexpr size_t AAD = 8;
    if (n < AAD + 16 + 4 || (pkt[0] >> 6) != 2 || pkt[1] < 200 || pkt[1] > 206) return 0;
    size_t ct_n = n - AAD - 16 - 4;
    if (!open(mode, key, pkt + n - 4, pkt, AAD, pkt + AAD, ct_n, pkt + n - 20, out + AAD)) return 0;
    memcpy(out, pkt, AAD);
    return AAD + ct_n;
}

size_t rtcpSenderReport(RtpMode mode, const uint8_t key[RTP_KEY], uint32_t* counter, uint32_t ssrc, uint64_t wall_ns,
                        uint32_t rtp_ts, uint32_t packets, uint32_t octets, uint8_t* out) {
    auto put32 = [](uint8_t* p, uint32_t v) {
        for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - 8 * i));
    };
    out[0] = 0x80, out[1] = 200, out[2] = 0, out[3] = 6;  // (V=2, no report blocks; 7 words)
    put32(out + 4, ssrc);
    uint8_t body[20];
    uint64_t sec = wall_ns / 1'000'000'000ULL, ns = wall_ns % 1'000'000'000ULL;
    put32(body, (uint32_t)(sec + 2208988800ULL));                    // (NTP: seconds since 1900)
    put32(body + 4, (uint32_t)((ns << 32) / 1'000'000'000ULL));      // (and its fraction)
    put32(body + 8, rtp_ts);
    put32(body + 12, packets);
    put32(body + 16, octets);
    uint32_t c = ++*counter;
    uint8_t* nonce4 = out + 8 + 20 + 16;
    put32(nonce4, c);
    if (mode == Rtp_Aes256Gcm) {
        uint8_t iv[GCM_IV] = {0};
        memcpy(iv, nonce4, 4);
        Gcm g;
        g.init(key, RTP_KEY);
        g.seal(iv, out, 8, body, 20, out + 8, out + 28, GCM_TAG);
        g.wipeKey();
    } else {
        uint8_t iv[XCHACHA_NONCE] = {0};
        memcpy(iv, nonce4, 4);
        xchachaSeal(key, iv, out, 8, body, 20, out + 8, out + 28);
    }
    return RTCP_SR_SIZE;
}

size_t rtcpFeedback(const uint8_t* p, size_t n, uint32_t media_ssrc, uint16_t* lost, size_t max, RtcpFeedback* fb) {
    size_t got = 0;
    while (n >= 4 && (p[0] >> 6) == 2) {
        size_t len = 4u * (be16(p + 2) + 1u);
        if (len > n) break;
        uint8_t fmt = p[0] & 0x1f, pt = p[1];
        uint32_t media = len >= 12 ? be32(p + 8) : 0;
        if (pt == 205 && fmt == 1 && len >= 16) {  // (Generic NACK: a lost packet's number, and a bitmask of the 16 after it)
            if (media == media_ssrc) {
                fb->nacks++;
                for (size_t i = 12; i + 4 <= len; i += 4) {
                    uint16_t pid = be16(p + i), blp = be16(p + i + 2);
                    for (int b = -1; b < 16; b++) {
                        if (b >= 0 && !(blp >> b & 1)) continue;
                        fb->lost++;
                        if (got < max) lost[got++] = (uint16_t)(pid + 1 + b);
                    }
                }
            } else {
                fb->other++;
            }
        } else if (pt == 206 && (fmt == 1 || fmt == 4)) {  // (PLI, FIR: a keyframe wanted)
            fb->keyframes++;
        } else if (pt == 206 && fmt == 15 && len >= 16 && memcmp(p + 12, "REMB", 4) == 0) {
            fb->remb++;
        } else {
            fb->other++;
        }
        p += len, n -= len;
    }
    return got;
}

size_t rtpSeal(RtpMode mode, const uint8_t key[RTP_KEY], uint32_t* counter, uint16_t seq, uint32_t timestamp, uint32_t ssrc,
               const uint8_t* payload, size_t payload_n, uint8_t* out) {
    return rtpSealAs(mode, key, counter, RTP_OPUS, false, seq, timestamp, ssrc, payload, payload_n, out);
}

size_t rtpSealAs(RtpMode mode, const uint8_t key[RTP_KEY], uint32_t* counter, uint8_t type, bool marker, uint16_t seq,
                 uint32_t timestamp, uint32_t ssrc, const uint8_t* payload, size_t payload_n, uint8_t* out) {
    out[0] = 0x80;
    out[1] = (uint8_t)(type | (marker ? 0x80 : 0));
    out[2] = (uint8_t)(seq >> 8), out[3] = (uint8_t)seq;
    for (int i = 0; i < 4; i++) out[4 + i] = (uint8_t)(timestamp >> (24 - 8 * i)), out[8 + i] = (uint8_t)(ssrc >> (24 - 8 * i));
    uint32_t c = ++*counter;
    uint8_t* nonce4 = out + RTP_HEADER + payload_n + 16;
    for (int i = 0; i < 4; i++) nonce4[i] = (uint8_t)(c >> (24 - 8 * i));
    if (mode == Rtp_Aes256Gcm) {
        uint8_t iv[GCM_IV] = {0};
        memcpy(iv, nonce4, 4);
        Gcm g;
        g.init(key, RTP_KEY);
        g.seal(iv, out, RTP_HEADER, payload, payload_n, out + RTP_HEADER, out + RTP_HEADER + payload_n, GCM_TAG);
        g.wipeKey();
    } else {
        uint8_t iv[XCHACHA_NONCE] = {0};
        memcpy(iv, nonce4, 4);
        xchachaSeal(key, iv, out, RTP_HEADER, payload, payload_n, out + RTP_HEADER, out + RTP_HEADER + payload_n);
    }
    return RTP_HEADER + payload_n + RTP_OVERHEAD - RTP_HEADER;
}

namespace {

// Bits of an RBSP (the NAL's payload, emulation-prevention bytes taken out), read and written
struct BitReader {
    const uint8_t* p;
    size_t n, pos = 0;  // (in bits)
    bool bad = false;
    uint32_t u(int k) {
        uint32_t v = 0;
        while (k--) {
            if (pos >= n * 8) {
                bad = true;
                return 0;
            }
            v = v << 1 | ((p[pos >> 3] >> (7 - (pos & 7))) & 1);
            pos++;
        }
        return v;
    }
    uint32_t ue() {
        int z = 0;
        while (!bad && u(1) == 0)
            if (++z > 31) bad = true;
        return bad ? 0 : (uint32_t)((1ull << z) - 1 + u(z));
    }
};

struct BitWriter {
    uint8_t* p;
    size_t cap, pos = 0;
    bool bad = false;
    void u(uint32_t v, int k) {
        while (k--) {
            if (pos >= cap * 8) {
                bad = true;
                return;
            }
            uint8_t m = (uint8_t)(0x80 >> (pos & 7));
            if ((v >> k) & 1) p[pos >> 3] |= m;
            else p[pos >> 3] &= (uint8_t)~m;
            pos++;
        }
    }
    void ue(uint32_t v) {
        uint64_t x = (uint64_t)v + 1;
        int len = 0;
        while ((x >> len) > 1) len++;
        u(0, len);
        u((uint32_t)x, len + 1);
    }
};

void skipHrd(BitReader& r) {
    uint32_t cpb = r.ue() + 1;
    r.u(4), r.u(4);
    for (uint32_t i = 0; i < cpb && i < 32 && !r.bad; i++) r.ue(), r.ue(), r.u(1);
    r.u(5), r.u(5), r.u(5), r.u(5);
}

}  // namespace

size_t h264FixSps(const uint8_t* sps, size_t n, uint8_t* out, size_t cap, int full_range) {
    if (n < 4 || n > 256 || (sps[0] & 0x1f) != 7) return 0;
    uint8_t rbsp[256];
    size_t rn = 0;
    for (size_t i = 1, zeros = 0; i < n; i++) {
        if (zeros >= 2 && sps[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = sps[i] == 0 ? zeros + 1 : 0;
        rbsp[rn++] = sps[i];
    }
    BitReader r{rbsp, rn};
    uint32_t profile = r.u(8);
    r.u(8), r.u(8), r.ue();
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        if (r.ue() == 3) r.u(1);
        r.ue(), r.ue(), r.u(1);
        if (r.u(1)) return 0;  // (scaling matrices: not read here; the Switch's recorder sends none)
    }
    r.ue();
    uint32_t poc = r.ue();
    if (poc == 0) {
        r.ue();
    } else if (poc == 1) {
        r.u(1), r.ue(), r.ue();
        uint32_t k = r.ue();
        for (uint32_t i = 0; i < k && i < 256 && !r.bad; i++) r.ue();
    }
    uint32_t refs = r.ue();
    r.u(1), r.ue(), r.ue();
    if (!r.u(1)) r.u(1);  // (frame_mbs_only, else mb_adaptive_frame_field)
    r.u(1);
    if (r.u(1)) r.ue(), r.ue(), r.ue(), r.ue();  // (cropping)
    size_t vui_at = r.pos;
    size_t keep = 0;  // (bits kept as they are, before what's written new)
    size_t range_at = 0;  // (video_full_range_flag's bit, if the VUI has a video signal type; 0: none)
    bool restricted = false;  // (the restriction's already as WebRTC wants it: nothing more written)
    bool vui = r.u(1);
    if (vui) {
        if (r.u(1) && r.u(8) == 255) r.u(16), r.u(16);  // (aspect ratio, extended)
        if (r.u(1)) r.u(1);                             // (overscan)
        if (r.u(1)) {                                   // (video signal type)
            r.u(3);
            range_at = r.pos;
            r.u(1);
            if (r.u(1)) r.u(8), r.u(8), r.u(8);
        }
        if (r.u(1)) r.ue(), r.ue();                     // (chroma location)
        if (r.u(1)) r.u(32), r.u(32), r.u(1);           // (timing)
        bool nal = r.u(1);
        if (nal) skipHrd(r);
        bool vcl = r.u(1);
        if (vcl) skipHrd(r);
        if (nal || vcl) r.u(1);
        r.u(1);
        keep = r.pos;
        if (r.u(1)) {  // (a restriction already: fine as it is if it says what WebRTC wants)
            r.u(1), r.ue(), r.ue(), r.ue(), r.ue();
            uint32_t reorder = r.ue(), dec = r.ue();
            if (r.bad) return 0;
            if (reorder == 0 && dec <= refs) {
                if (full_range < 0 || !range_at) {
                    if (n > cap) return 0;
                    memcpy(out, sps, n);
                    return n;
                }
                restricted = true;  // (only the range flag to set: everything else as it is)
                keep = r.pos;
            }
        }
    } else {
        keep = vui_at;
    }
    if (r.bad) return 0;
    // what was there up to `keep`, then (a VUI with nothing else in it if there was none) the restriction
    uint8_t w[260];
    memcpy(w, rbsp, rn);
    BitWriter b{w, sizeof(w)};
    if (full_range >= 0 && range_at) {
        b.pos = range_at;
        b.u((uint32_t)full_range, 1);
    }
    b.pos = keep;
    if (restricted) {
        b.u(1, 1);  // (the stop bit)
        while (b.pos & 7) b.u(0, 1);
    } else if (!vui) {
        b.u(1, 1);  // (vui_parameters_present)
        b.u(0, 8);  // (aspect ratio, overscan, signal type, chroma location, timing, nal hrd, vcl hrd, pic_struct: none)
    }
    if (!restricted) {
        // bitstream_restriction: motion vectors may cross picture edges, no byte or bit limits, 16-bit mv
        // lengths, no reordering, a frame buffer as big as the references (WebRTC's own values)
        b.u(1, 1);
        b.u(1, 1);
        b.ue(2), b.ue(1), b.ue(16), b.ue(16), b.ue(0), b.ue(refs);
        b.u(1, 1);  // (the stop bit)
        while (b.pos & 7) b.u(0, 1);
    }
    if (b.bad) return 0;
    size_t wn = b.pos / 8;
    // back to a NAL: the header, then emulation prevention where 00 00 would run into 00..03
    size_t o = 0;
    if (cap < 1) return 0;
    out[o++] = sps[0];
    for (size_t i = 0, zeros = 0; i < wn; i++) {
        if (zeros >= 2 && w[i] <= 3) {
            if (o >= cap) return 0;
            out[o++] = 3;
            zeros = 0;
        }
        if (o >= cap) return 0;
        out[o++] = w[i];
        zeros = w[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

namespace {

// An RBSP as a NAL's payload: emulation prevention where 00 00 would run into 00..03
size_t escapeRbsp(const uint8_t* r, size_t n, uint8_t* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0, zeros = 0; i < n; i++) {
        if (zeros >= 2 && r[i] <= 3) {
            if (o >= cap) return 0;
            out[o++] = 3;
            zeros = 0;
        }
        if (o >= cap) return 0;
        out[o++] = r[i];
        zeros = r[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

}  // namespace

size_t h264Placeholder(const H264Params* p, int idr_id, uint8_t* out, size_t cap) {
    if (!p->sps_n || cap < 4 + p->sps_n) return 0;
    size_t o = 0;
    auto start = [&](uint8_t header) {
        if (o + 5 > cap) return false;
        out[o++] = 0, out[o++] = 0, out[o++] = 0, out[o++] = 1, out[o++] = header;
        return true;
    };
    // (the SPS as WebRTC wants it, always: a raw one is rewritten by the viewer's WebRTC, and DAVE's tag over
    // it no longer matches. A grey frame sent with grc's raw SPS was refused, every one: viewers saw nothing)
    uint8_t sps[sizeof(p->sps)];
    size_t sps_n = h264FixSps(p->sps, p->sps_n, sps, sizeof(sps), p->full_range);
    if (!sps_n) memcpy(sps, p->sps, p->sps_n), sps_n = p->sps_n;
    if (cap < 4 + sps_n) return 0;
    if (!start(sps[0])) return 0;
    memcpy(out + o, sps + 1, sps_n - 1);
    o += sps_n - 1;
    uint8_t r[3800];
    // the PPS: id 1, SPS 0, CAVLC, one slice group, one reference, no weighting, QP 26, deblocking control
    // present, no constrained intra, no redundant pictures
    {
        memset(r, 0, 16);
        BitWriter b{r, 16};
        b.ue(1), b.ue(0), b.u(0, 1), b.u(0, 1), b.ue(0), b.ue(0), b.ue(0), b.u(0, 1), b.u(0, 2);
        b.ue(0), b.ue(0), b.ue(0);  // (se(0): pic_init_qp, pic_init_qs, chroma_qp_index_offset)
        b.u(1, 1), b.u(0, 1), b.u(0, 1);
        b.u(1, 1);
        while (b.pos & 7) b.u(0, 1);
        if (!start(0x68)) return 0;
        size_t k = escapeRbsp(r, b.pos / 8, out + o, cap - o);
        if (!k) return 0;
        o += k;
    }
    // the picture: one I slice of all 3600 macroblocks
    {
        BitWriter b{r, sizeof(r)};
        b.ue(0);        // first_mb_in_slice
        b.ue(7);        // slice_type: I (all of the picture's)
        b.ue(1);        // pic_parameter_set_id
        b.u(0, 8);      // frame_num
        b.ue(idr_id & 1);
        b.u(0, 1), b.u(0, 1);  // (dec_ref_pic_marking: no_output_of_prior_pics, long_term_reference)
        b.ue(0);        // slice_qp_delta (se(0))
        b.ue(1);        // disable_deblocking_filter_idc: off
        for (int mb = 0; mb < 80 * 45; mb++) {
            b.ue(3);    // mb_type I_16x16_2_0_0: DC prediction, no coded luma or chroma
            b.ue(0);    // intra_chroma_pred_mode: DC
            b.ue(0);    // mb_qp_delta (se(0))
            b.u(1, 1);  // the luma DC block's coeff_token: no coefficients (nC 0)
        }
        b.u(1, 1);
        while (b.pos & 7) b.u(0, 1);
        if (b.bad || !start(0x65)) return 0;
        size_t k = escapeRbsp(r, b.pos / 8, out + o, cap - o);
        if (!k) return 0;
        o += k;
    }
    return o;
}

size_t rtpH264WithParams(uint8_t* buf, size_t n, size_t cap, H264Params* p, bool* idr) {
    *idr = false;
    bool has_sps = false, has_pps = false;
    // each NAL: from after a start code to the next one
    size_t i = 0;
    while (i + 3 <= n) {
        if (!(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)) {
            i++;
            continue;
        }
        size_t nal = i + 3, end = n;
        for (size_t j = nal; j + 3 <= n; j++)
            if (buf[j] == 0 && buf[j + 1] == 0 && (buf[j + 2] == 1 || (buf[j + 2] == 0 && j + 3 < n && buf[j + 3] == 1))) {
                end = j;
                break;
            }
        if (nal < end) {
            int type = buf[nal] & 0x1f;
            size_t len = end - nal;
            if (type == 7) {
                has_sps = true;
                // (as WebRTC wants it, in the frame too: grown in place)
                uint8_t fixed[sizeof(p->sps)];
                size_t fn = h264FixSps(buf + nal, len, fixed, sizeof(fixed), p->full_range);
                if (fn && n - len + fn <= cap) {
                    memmove(buf + nal + fn, buf + end, n - end);
                    memcpy(buf + nal, fixed, fn);
                    n = n - len + fn;
                    end = nal + fn;
                    len = fn;
                }
                if (len <= sizeof(p->sps)) memcpy(p->sps, buf + nal, len), p->sps_n = len;
            } else if (type == 8) {
                has_pps = true;
                if (len <= sizeof(p->pps)) memcpy(p->pps, buf + nal, len), p->pps_n = len;
            } else if (type == 5) {
                *idr = true;
            }
        }
        i = end;
    }
    if (!*idr || (has_sps && has_pps) || !p->sps_n || !p->pps_n) return n;
    {  // (the kept one too, from before it was fixed: grc's own to start with)
        uint8_t fixed[sizeof(p->sps)];
        size_t fn = h264FixSps(p->sps, p->sps_n, fixed, sizeof(fixed), p->full_range);
        if (fn) memcpy(p->sps, fixed, fn), p->sps_n = fn;
    }
    size_t add = 4 + p->sps_n + 4 + p->pps_n;
    if (n + add > cap) return n;
    memmove(buf + add, buf, n);
    uint8_t* q = buf;
    memcpy(q, "\0\0\0\1", 4), q += 4;
    memcpy(q, p->sps, p->sps_n), q += p->sps_n;
    memcpy(q, "\0\0\0\1", 4), q += 4;
    memcpy(q, p->pps, p->pps_n);
    return n + add;
}
