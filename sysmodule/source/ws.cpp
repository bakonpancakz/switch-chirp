#include "ws.h"

#include <stdio.h>
#include <string.h>

#include "util.h"

namespace {

const char* const WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr int HANDSHAKE_READ_MS = 250;

// The end of an HTTP header ("\r\n\r\n") in p, or -1
int headerEnd(const uint8_t* p, size_t n) {
    for (size_t i = 3; i < n; i++)
        if (p[i - 3] == '\r' && p[i - 2] == '\n' && p[i - 1] == '\r' && p[i] == '\n') return (int)(i + 1);
    return -1;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

// The value of header `name` (lowercase, with its ':') in the n bytes at p, trimmed
bool headerValue(const char* p, size_t n, const char* name, char* out, size_t cap) {
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl <= n; i++) {
        if (i > 0 && p[i - 1] != '\n') continue;
        size_t k = 0;
        while (k < nl && lower(p[i + k]) == name[k]) k++;
        if (k < nl) continue;
        size_t v = i + nl;
        while (v < n && (p[v] == ' ' || p[v] == '\t')) v++;
        size_t e = v;
        while (e < n && p[e] != '\r' && p[e] != '\n') e++;
        while (e > v && (p[e - 1] == ' ' || p[e - 1] == '\t')) e--;
        if (e - v >= cap) return false;
        memcpy(out, p + v, e - v);
        out[e - v] = 0;
        return true;
    }
    return false;
}

}  // namespace

void WebSocket::reset(Stream* s, RandomFn rnd) {
    s_ = s;
    rnd_ = rnd;
    rx_len_ = 0;
    hdr_len_ = 0;
    hdr_need_ = 2;
    left_ = 0;
    in_payload_ = false;
    in_message_ = false;
    ctl_len_ = 0;
    closed_ = false;
    close_code_ = 0;
    close_reason_[0] = 0;
    error_[0] = 0;
}

bool WebSocket::handshake(const char* host, const char* path, int timeout_ms) {
    uint8_t raw[16];
    rnd_(raw, sizeof(raw));
    char key[32];
    base64(raw, sizeof(raw), key);
    int n = snprintf((char*)tx_, sizeof(tx_),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n",
                     path, host, key);
    if (n <= 0 || n >= (int)sizeof(tx_)) {
        fail("request too long");
        return false;
    }
    if (!s_->write(tx_, n)) {
        fail("couldn't send the upgrade");
        return false;
    }

    // the answer's header (whatever came after it is the first frames)
    size_t got = 0;
    int end = -1, waited = 0;
    while ((end = headerEnd(rx_, got)) < 0) {
        if (got == sizeof(rx_)) {
            fail("the answer's header is too long");
            return false;
        }
        int r = s_->read(rx_ + got, (int)(sizeof(rx_) - got), HANDSHAKE_READ_MS);
        if (r < 0) {
            fail("closed during the upgrade");
            return false;
        }
        if (r == 0 && (waited += HANDSHAKE_READ_MS) >= timeout_ms) {
            fail("no answer to the upgrade");
            return false;
        }
        got += (size_t)r;
    }
    const char* h = (const char*)rx_;
    if (end < 12 || memcmp(h, "HTTP/1.1 101", 12) != 0) {
        fail("the server didn't switch to websocket");
        return false;
    }
    char accept[64];
    if (!headerValue(h, (size_t)end, "sec-websocket-accept:", accept, sizeof(accept))) {
        fail("no Sec-WebSocket-Accept");
        return false;
    }
    char joined[96];
    snprintf(joined, sizeof(joined), "%s%s", key, WS_GUID);
    uint8_t digest[20];
    sha1(joined, strlen(joined), digest);
    char want[32];
    base64(digest, sizeof(digest), want);
    if (strcmp(accept, want) != 0) {
        fail("wrong Sec-WebSocket-Accept");
        return false;
    }
    rx_len_ = got - (size_t)end;
    memmove(rx_, rx_ + end, rx_len_);
    return true;
}

bool WebSocket::poll(int timeout_ms, WsHandler* h) {
    if (closed_) return false;
    if (rx_len_) {
        size_t n = rx_len_;
        rx_len_ = 0;
        if (!handle(rx_, n, h)) return false;
    }
    int r = s_->read(rx_, sizeof(rx_), timeout_ms);
    if (r < 0) {
        fail("the connection went away");
        closed_ = true;
        return false;
    }
    return r == 0 || handle(rx_, (size_t)r, h);
}

bool WebSocket::handle(const uint8_t* p, size_t n, WsHandler* h) {
    while (n > 0) {
        if (!in_payload_) {
            // the frame's header, however many reads it comes in
            while (n > 0 && hdr_len_ < hdr_need_) {
                hdr_[hdr_len_++] = *p++;
                n--;
                if (hdr_len_ == 2) {
                    if (hdr_[1] & 0x80) {
                        fail("the server masked a frame");
                        closed_ = true;
                        return false;
                    }
                    uint8_t len7 = hdr_[1] & 0x7F;
                    hdr_need_ = (uint8_t)(2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0));
                }
            }
            if (hdr_len_ < hdr_need_) return true;  // (the rest comes in the next read)
            fin_ = (hdr_[0] & 0x80) != 0;
            opcode_ = hdr_[0] & 0x0F;
            if (hdr_[0] & 0x70) {
                fail("a frame with RSV bits (no extensions were asked for)");
                closed_ = true;
                return false;
            }
            uint8_t len7 = hdr_[1] & 0x7F;
            if (len7 < 126) {
                left_ = len7;
            } else if (len7 == 126) {
                left_ = (uint64_t)hdr_[2] << 8 | hdr_[3];
            } else {
                left_ = 0;
                for (int i = 0; i < 8; i++) left_ = left_ << 8 | hdr_[2 + i];
            }
            hdr_len_ = 0;
            hdr_need_ = 2;
            if (opcode_ & 0x8) {
                if (!fin_ || left_ > sizeof(ctl_)) {
                    fail("a broken control frame");
                    closed_ = true;
                    return false;
                }
                ctl_len_ = 0;
            } else if (opcode_ == 1 || opcode_ == 2) {
                if (in_message_) {
                    fail("a message inside a message");
                    closed_ = true;
                    return false;
                }
                in_message_ = true;
                if (h) h->messageStart(opcode_ == 1);
            } else if (opcode_ != 0 || !in_message_) {
                fail("an unknown frame");
                closed_ = true;
                return false;
            }
            in_payload_ = true;
        }
        size_t take = left_ < n ? (size_t)left_ : n;
        if (opcode_ & 0x8) {
            memcpy(ctl_ + ctl_len_, p, take);
            ctl_len_ = (uint8_t)(ctl_len_ + take);
        } else if (take && h) {
            h->messageData((const char*)p, take);
        }
        p += take, n -= take, left_ -= take;
        if (left_ == 0) {
            in_payload_ = false;
            if (opcode_ & 0x8) {
                if (!control()) return false;
            } else if (fin_) {
                in_message_ = false;
                if (h) h->messageEnd();
            }
        }
    }
    return true;
}

// A whole control frame in ctl_: false once the connection is over
bool WebSocket::control() {
    if (opcode_ == 0x8) {
        close_code_ = ctl_len_ >= 2 ? (uint16_t)(ctl_[0] << 8 | ctl_[1]) : 1005;
        // (the reason after the code, as text, for the log)
        size_t n = 0;
        for (size_t i = 2; i < ctl_len_ && n < sizeof(close_reason_) - 1; i++)
            close_reason_[n++] = ctl_[i] >= 0x20 && ctl_[i] < 0x7F ? (char)ctl_[i] : '?';
        close_reason_[n] = 0;
        if (!closed_) sendFrame(0x8, ctl_, ctl_len_ >= 2 ? 2 : 0);  // (answered, as the RFC says)
        closed_ = true;
        return false;
    }
    if (opcode_ == 0x9) return sendFrame(0xA, ctl_, ctl_len_);
    return true;  // (a pong: nothing to do)
}

bool WebSocket::sendFrame(uint8_t opcode, const void* data, size_t n) { return sendParts(opcode, data, n, nullptr, 0); }

// One frame of a's bytes then b's, masked through tx_ a piece at a time (so a frame of any size fits)
bool WebSocket::sendParts(uint8_t opcode, const void* a, size_t an, const void* b, size_t bn) {
    size_t n = an + bn, at = 0;
    tx_[at++] = (uint8_t)(0x80 | opcode);
    if (n < 126) {
        tx_[at++] = (uint8_t)(0x80 | n);
    } else if (n < 65536) {
        tx_[at++] = 0x80 | 126;
        tx_[at++] = (uint8_t)(n >> 8);
        tx_[at++] = (uint8_t)n;
    } else {
        tx_[at++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) tx_[at++] = (uint8_t)((uint64_t)n >> (8 * i));
    }
    uint8_t mask[4];
    rnd_(mask, sizeof(mask));
    memcpy(tx_ + at, mask, 4);
    at += 4;
    size_t i = 0;
    while (i < n) {
        while (i < n && at < sizeof(tx_)) {
            uint8_t v = i < an ? ((const uint8_t*)a)[i] : ((const uint8_t*)b)[i - an];
            tx_[at++] = v ^ mask[i & 3];
            i++;
        }
        if (!s_->write(tx_, (int)at)) {
            fail("couldn't send");
            closed_ = true;
            return false;
        }
        at = 0;
    }
    if (at && !s_->write(tx_, (int)at)) {  // (an empty frame: just its header)
        fail("couldn't send");
        closed_ = true;
        return false;
    }
    return true;
}

bool WebSocket::sendText(const char* p, size_t n) { return !closed_ && sendFrame(0x1, p, n); }

bool WebSocket::sendBinary(uint8_t first, const void* rest, size_t n) { return !closed_ && sendParts(0x2, &first, 1, rest, n); }

void WebSocket::sendClose(uint16_t code) {
    if (closed_) return;
    uint8_t b[2] = {(uint8_t)(code >> 8), (uint8_t)code};
    sendFrame(0x8, b, 2);
    closed_ = true;
}
