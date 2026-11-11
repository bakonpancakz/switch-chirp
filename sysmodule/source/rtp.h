// Discord's voice packets over UDP: RTP, encrypted in one of the "rtpsize"
// modes (the voice docs; discord.js's VoiceReceiver):
//   [RTP header: 12 bytes, 4 per CSRC, and the extension's 4-byte preamble: the AAD]
//   [ciphertext: the extension's body, then the payload][tag: 16][nonce: 4, big-endian]
// The AEAD nonce is those 4 bytes then zeros (12 for AES-256-GCM, 24 for XChaCha20-Poly1305).
// After decrypting: the extension body (4 * its length) is skipped, and RTP padding dropped.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RTP_OPUS 0x78
#define RTP_HEADER 12
#define RTP_OVERHEAD (RTP_HEADER + 16 + 4)  // what our own packets add to their payload
#define RTP_KEY 32

enum RtpMode : uint8_t { Rtp_None = 0, Rtp_Aes256Gcm = 1, Rtp_XChaCha = 2 };

struct RtpPacket {
    uint16_t seq;
    uint32_t timestamp, ssrc;
    const uint8_t* payload;  // (in the caller's out buffer)
    size_t payload_n;
};

// A received packet: false if it isn't Opus RTP, or doesn't open. out: at least n bytes.
bool rtpOpen(RtpMode mode, const uint8_t key[RTP_KEY], const uint8_t* pkt, size_t n, uint8_t* out, RtpPacket* p);
// One of ours: header (seq, timestamp, ssrc), the payload sealed, the counter after it.
// out: payload_n + RTP_OVERHEAD bytes; *counter is incremented first (the first nonce is 1).
size_t rtpSeal(RtpMode mode, const uint8_t key[RTP_KEY], uint32_t* counter, uint16_t seq, uint32_t timestamp, uint32_t ssrc,
               const uint8_t* payload, size_t payload_n, uint8_t* out);
// The same with another payload type, and the marker bit (video: on a frame's last packet)
size_t rtpSealAs(RtpMode mode, const uint8_t key[RTP_KEY], uint32_t* counter, uint8_t type, bool marker, uint16_t seq,
                 uint32_t timestamp, uint32_t ssrc, const uint8_t* payload, size_t payload_n, uint8_t* out);

// --- RTCP: viewers' feedback on our video ---
// A received RTCP packet (it may be several, one after another). In the rtpsize modes its first 8 bytes
// (the first one's header and sender SSRC) are the AAD and the rest is sealed as RTP's is. Opened into
// out (at least n bytes), header and all: its length, or 0 if it didn't open
size_t rtcpOpen(RtpMode mode, const uint8_t key[RTP_KEY], const uint8_t* pkt, size_t n, uint8_t* out);

struct RtcpFeedback {
    uint32_t nacks;  // lost-packet reports (Generic NACK, RFC 4585)
    uint32_t lost;   // the sequence numbers they named (more than fit in lost[] too)
    uint32_t keyframes;  // keyframe requests (PLI, FIR)
    uint32_t remb;   // receivers' bandwidth estimates (they want nothing)
    uint32_t other;  // reports and the rest
};
// What an opened RTCP packet says about media_ssrc's video: the sequence numbers reported lost go in
// lost[] (max at most; how many returned), the rest counted in *fb. Stops at anything malformed
size_t rtcpFeedback(const uint8_t* p, size_t n, uint32_t media_ssrc, uint16_t* lost, size_t max, RtcpFeedback* fb);

// One of ours: a Sender Report (RFC 3550) for ssrc, so viewers can line its RTP timestamps up with the
// other streams': at wall-clock time wall_ns (any clock, the same for every stream), the stream was at
// rtp_ts; and what it has sent. Sealed as rtcpOpen() opens (*counter incremented first, as rtpSeal's).
// out: RTCP_SR_SIZE bytes; its length returned
#define RTCP_SR_SIZE (8 + 20 + 16 + 4)
size_t rtcpSenderReport(RtpMode mode, const uint8_t key[RTP_KEY], uint32_t* counter, uint32_t ssrc, uint64_t wall_ns,
                        uint32_t rtp_ts, uint32_t packets, uint32_t octets, uint8_t* out);

// --- H.264 (RFC 6184) ---
#define RTP_H264 101
#define RTP_H264_CLOCK 90000

// The parameter sets a decoder needs before any picture: the latest seen, kept. They start as grc's own
// (720p High 3.2, PPS id 0, as SysDVR hardcodes them): the recorder sends them only once, when a game
// launches, so a capture started later never sees any
struct H264Params {
    uint8_t sps[128] = {0x67, 0x64, 0x0c, 0x20, 0xac, 0x2b, 0x40, 0x28, 0x02, 0xdd, 0x35, 0x01, 0x0d, 0x01, 0xe0, 0x80};
    uint8_t pps[64] = {0x68, 0xee, 0x3c, 0xb0};
    size_t sps_n = 16, pps_n = 4;
    int full_range = -1;  // (the SPS's video_full_range_flag set to this: 0 limited, 1 full; -1 as the recorder says)
};
// A frame (Annex-B) as sent: its SPS and PPS kept when it has them; a keyframe (IDR) without them gets
// the kept ones put in front (the relay forwards nothing from before a viewer asks, and the recorder
// sends them only now and then: without them nothing decodes). buf: cap bytes. Returns its new size;
// *idr: it's a keyframe
size_t rtpH264WithParams(uint8_t* buf, size_t n, size_t cap, H264Params* p, bool* idr);
// An SPS NAL (with its header byte, no start code) as WebRTC wants it: its VUI saying no frames are
// reordered (bitstream_restriction, max_num_reorder_frames 0, max_dec_frame_buffering the reference
// count). A viewer's WebRTC rewrites any SPS that doesn't, and DAVE's tag covers the SPS: changed
// under it, the keyframe no longer opens. Returns the new size (the same bytes when it's already so),
// 0 if it can't be read (sent as it is, then). out: cap bytes, not the input
// full_range: its video_full_range_flag set (0 limited, 1 full) where the VUI has one; -1 left as it is
size_t h264FixSps(const uint8_t* sps, size_t n, uint8_t* out, size_t cap, int full_range = -1);
// A keyframe of plain mid grey, for when the recorder has nothing (no game, or the HOME menu): the kept
// SPS, a PPS of our own (id 1: CAVLC, simple enough to write a picture by hand), and an IDR picture whose
// every macroblock is Intra 16x16, DC-predicted, with no residual. For grc's SPS (1280x720, 8-bit
// frame_num, POC type 2). idr_id: 0 or 1, different from the last keyframe's. Annex-B; 0: no room
#define H264_PLACEHOLDER_MAX 4096
size_t h264Placeholder(const H264Params* p, int idr_id, uint8_t* out, size_t cap);
// An Annex-B frame's NAL units as RTP payloads of at most max bytes: a NAL that fits goes whole, a
// bigger one in FU-A fragments (built in scratch: max bytes). fn(payload, n, last) for each, last on
// the frame's final one (its marker). The start codes aren't sent: the receiver puts them back
template <class Fn>
void rtpH264Payloads(const uint8_t* f, size_t n, size_t max, uint8_t* scratch, Fn fn) {
    auto startAt = [&](size_t from, size_t* at, size_t* len) {
        for (size_t i = from; i + 3 <= n; i++)
            if (f[i] == 0 && f[i + 1] == 0 && f[i + 2] == 1) {
                bool four = i > from && f[i - 1] == 0;
                *at = four ? i - 1 : i;
                *len = four ? 4 : 3;
                return true;
            }
        return false;
    };
    size_t at = 0, len = 0;
    if (!startAt(0, &at, &len)) return;
    while (true) {
        size_t nal = at + len, next = n, nlen = 0, end = n;
        bool more = startAt(nal, &next, &nlen);
        if (more) end = next;
        if (end > nal) {
            size_t size = end - nal;
            if (size <= max) {
                fn(f + nal, size, !more);
            } else {
                uint8_t hdr = f[nal];
                const uint8_t* p = f + nal + 1;
                size_t left = size - 1;
                bool first = true;
                while (left) {
                    size_t k = left < max - 2 ? left : max - 2;
                    scratch[0] = (uint8_t)((hdr & 0xE0) | 28);  // FU indicator: F, NRI, type 28
                    scratch[1] = (uint8_t)((hdr & 0x1F) | (first ? 0x80 : 0) | (k == left ? 0x40 : 0));  // S, E, the type
                    memcpy(scratch + 2, p, k);
                    p += k, left -= k;
                    fn(scratch, k + 2, !more && !left);
                    first = false;
                }
            }
        }
        if (!more) break;
        at = next, len = nlen;
    }
}
