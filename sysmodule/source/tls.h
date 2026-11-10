#pragma once
#include <poll.h>
#include <switch.h>

#include <stddef.h>

#include "ws.h"

// TLS through the Switch's ssl service, on a socket we connect.
//
// As libcurl's Switch backend does it: the socket stays ours
// (DoNotCloseSocket), so it can be waited on with poll(); the connection is
// non-blocking once the handshake is done, so a read or write never waits
// (and never holds app.cpp's lock while it does); and what the ssl service has
// already decrypted (Pending) counts as there to read, since the socket's
// poll can't see it.

#define TLS_CONNECT_MS 10000

const Result SSL_WOULD_BLOCK = MAKERESULT(123, 204);  // (PR_WOULD_BLOCK_ERROR)
constexpr int WRITE_MS = 5000;  // the longest a write waits for room

class TlsStream : public Stream {
public:
    bool open(const char* host, char* why, size_t why_cap, const char* port = "443");
    int fd() const { return fd_; }
    // Bytes the ssl service has decrypted already (a poll on the socket can't see them)
    bool pending() {
        s32 n = 0;
        return R_SUCCEEDED(sslConnectionPending(&conn_, &n)) && n > 0;
    }
    void close();
    // Bytes waiting (or the connection ending) within timeout_ms
    bool wait(int timeout_ms) {
        s32 pending = 0;
        if (R_SUCCEEDED(sslConnectionPending(&conn_, &pending)) && pending > 0) return true;
        pollfd p = {fd_, POLLIN, 0};
        return poll(&p, 1, timeout_ms) != 0;  // (an error too: the read finds out what it is)
    }
    int read(void* buf, int cap, int timeout_ms) override {
        if (!wait(timeout_ms)) return 0;
        u32 got = 0;
        Result rc = sslConnectionRead(&conn_, buf, (u32)cap, &got);
        if (R_VALUE(rc) == SSL_WOULD_BLOCK) return 0;  // (part of a TLS record: the rest is coming)
        if (R_FAILED(rc) || got == 0) return -1;
        return (int)got;
    }
    bool write(const void* data, int n) override {
        const u8* p = (const u8*)data;
        int waited = 0;
        while (n > 0) {
            u32 put = 0;
            Result rc = sslConnectionWrite(&conn_, p, (u32)n, &put);
            if (R_VALUE(rc) == SSL_WOULD_BLOCK || (R_SUCCEEDED(rc) && put == 0)) {
                pollfd w = {fd_, POLLOUT, 0};
                poll(&w, 1, 50);
                if ((waited += 50) >= WRITE_MS) return false;
                continue;
            }
            if (R_FAILED(rc)) return false;
            p += put, n -= (int)put;
        }
        return true;
    }

private:
    SslContext ctx_ = {};
    SslConnection conn_ = {};
    bool ctx_open_ = false, conn_open_ = false;
    int fd_ = -1;
};
