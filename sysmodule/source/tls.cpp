#include "tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>

namespace {

// One lookup at a time: the gateway's, the avatars' and the voice threads' could ask together,
// and their answers come through one sfdnsres session (and Atmosphere's DNS mitm, in ams.mitm)
Mutex g_lookup;  // (zero is an unlocked libnx Mutex)

// A TCP connection to host:443 that gives up after TLS_CONNECT_MS
int tcpConnect(const char* host, const char* port, char* why, size_t why_cap) {
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    mutexLock(&g_lookup);
    int looked = getaddrinfo(host, port, &hints, &res);
    mutexUnlock(&g_lookup);
    if (looked != 0 || !res) {
        snprintf(why, why_cap, "can't look up %s (no network?)", host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        freeaddrinfo(res);
        snprintf(why, why_cap, "no socket");
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r != 0) {
        pollfd p = {fd, POLLOUT, 0};
        int err = 0;
        socklen_t len = sizeof(err);
        if (poll(&p, 1, TLS_CONNECT_MS) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            ::close(fd);
            snprintf(why, why_cap, "can't connect to %s", host);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}

}  // namespace

bool TlsStream::open(const char* host, char* why, size_t why_cap, const char* port) {
    fd_ = tcpConnect(host, port, why, why_cap);
    if (fd_ < 0) return false;
    Result rc = sslCreateContext(&ctx_, SslVersion_Auto);
    ctx_open_ = R_SUCCEEDED(rc);
    if (ctx_open_) rc = sslContextCreateConnection(&ctx_, &conn_);
    conn_open_ = ctx_open_ && R_SUCCEEDED(rc);
    if (!conn_open_) {
        snprintf(why, why_cap, "the ssl service said no (0x%x)", rc);
        close();
        return false;
    }
    rc = sslConnectionSetOption(&conn_, SslOptionType_DoNotCloseSocket, true);
    if (R_FAILED(rc)) {
        snprintf(why, why_cap, "the ssl service said no to DoNotCloseSocket (0x%x)", rc);
        close();
        return false;
    }
    // (with DoNotCloseSocket no socket comes back: ENOENT, which is fine)
    int back = socketSslConnectionSetSocketDescriptor(&conn_, fd_);
    if (back >= 0) ::close(back);
    else if (errno != ENOENT) {
        snprintf(why, why_cap, "the ssl service didn't take the socket");
        close();
        return false;
    }
    rc = sslConnectionSetHostName(&conn_, host, (u32)strlen(host));
    if (R_SUCCEEDED(rc)) rc = sslConnectionDoHandshake(&conn_, nullptr, nullptr, nullptr, 0);
    if (R_FAILED(rc)) {
        snprintf(why, why_cap, "the TLS handshake failed (0x%x, certificate 0x%x)", rc, sslConnectionGetVerifyCertError(&conn_));
        close();
        return false;
    }
    rc = sslConnectionSetIoMode(&conn_, SslIoMode_NonBlocking);
    if (R_FAILED(rc)) {
        snprintf(why, why_cap, "the ssl service said no to non-blocking (0x%x)", rc);
        close();
        return false;
    }
    return true;
}

void TlsStream::close() {
    if (conn_open_) sslConnectionClose(&conn_);
    if (ctx_open_) sslContextClose(&ctx_);
    conn_open_ = ctx_open_ = false;
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}
