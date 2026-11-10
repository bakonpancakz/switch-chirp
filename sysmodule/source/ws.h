// A websocket client (RFC 6455) over any Stream: TLS through the Switch's
// ssl service, or plain TCP in the PC tests. Messages are handed on as their
// bytes arrive, never held whole; only control frames (125 bytes at most)
// are. What we send is masked through a fixed buffer, WS_SEND_MAX at a time.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define WS_RECV_BUF 4096
#define WS_SEND_MAX 2048

class Stream {
public:
    virtual ~Stream() {}
    // >0: bytes read; 0: nothing within timeout_ms; -1: closed or failed
    virtual int read(void* buf, int cap, int timeout_ms) = 0;
    virtual bool write(const void* data, int n) = 0;  // all of it, or false
};

class WsHandler {
public:
    virtual ~WsHandler() {}
    virtual void messageStart(bool text) {}
    virtual void messageData(const char* p, size_t n) {}
    virtual void messageEnd() {}
};

class WebSocket {
public:
    // random bytes, for the handshake's key and each frame's mask
    typedef void (*RandomFn)(void* out, size_t n);

    void reset(Stream* s, RandomFn rnd);
    // The HTTP upgrade; false (and why, in error()) if the server didn't take it
    bool handshake(const char* host, const char* path, int timeout_ms);
    // Waits up to timeout_ms for bytes and handles all that came; false once
    // the connection is over (closed by either side, or broken)
    bool poll(int timeout_ms, WsHandler* h);
    bool sendText(const char* p, size_t n);
    // A binary message of one byte, then n more (Discord's voice gateway: [opcode][body]); any size
    bool sendBinary(uint8_t first, const void* rest, size_t n);
    void sendClose(uint16_t code);

    uint16_t closeCode() const { return close_code_; }  // the server's (0: none came)
    const char* closeReason() const { return close_reason_; }
    const char* error() const { return error_; }

private:
    bool sendFrame(uint8_t opcode, const void* p, size_t n);
    bool sendParts(uint8_t opcode, const void* a, size_t an, const void* b, size_t bn);
    bool handle(const uint8_t* p, size_t n, WsHandler* h);
    bool control();
    void fail(const char* why) {
        if (!error_[0]) {
            size_t i = 0;
            for (; why[i] && i < sizeof(error_) - 1; i++) error_[i] = why[i];
            error_[i] = 0;
        }
    }

    Stream* s_ = nullptr;
    RandomFn rnd_ = nullptr;
    uint8_t rx_[WS_RECV_BUF];
    size_t rx_len_ = 0;  // left over after the handshake, for poll() to handle first
    uint8_t tx_[WS_SEND_MAX + 14];

    // the frame being read
    uint8_t hdr_[14];
    uint8_t hdr_len_ = 0, hdr_need_ = 2;
    uint8_t opcode_ = 0;
    bool fin_ = false;
    uint64_t left_ = 0;   // payload bytes still to come
    bool in_payload_ = false;  // the header is read, its payload is coming
    bool in_message_ = false;
    uint8_t ctl_[125];
    uint8_t ctl_len_ = 0;

    bool closed_ = false;
    uint16_t close_code_ = 0;
    char close_reason_[64] = {0};
    char error_[64] = {0};
};
