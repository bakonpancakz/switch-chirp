// Chirp's MLS (the sysmodule's own mlsgroup.cpp) behind a line protocol, so
// interop.mjs can put it in a group with ts-mls, an MLS of someone else's.
// One command per line, one answer per line; bytes in hex.
//   KP <user id>             -> OK <MLSMessage(KeyPackage)>
//   CREATE <group id> <exts> -> OK
//   HANDLE <MLSMessage>      -> OK | ERR <result> <line>
//   COMMIT                   -> OK <commit MLSMessage> <bare Welcome or ->
//   JOIN <bare Welcome>      -> OK | ERR <result> <line>
//   EXPORT <label> <ctx> <n> -> OK <secret>
//   EPOCH                    -> OK <epoch>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <random>
#include <string>
#include <vector>

#include "mlsgroup.h"

static std::random_device g_dev;
static void rnd(void* out, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t*)out)[i] = (uint8_t)g_dev();
}

static std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> v;
    if (s == "-") return v;
    for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t)strtoul(s.substr(i, 2).c_str(), nullptr, 16));
    return v;
}

static std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) s += d[p[i] >> 4], s += d[p[i] & 15];
    return n ? s : "-";
}

static MlsGroup g;
static uint8_t g_a[64 * 1024], g_b[64 * 1024];

int main() {
    static char line[256 * 1024];
    while (fgets(line, sizeof(line), stdin)) {
        std::vector<std::string> w;
        for (char* t = strtok(line, " \r\n"); t; t = strtok(nullptr, " \r\n")) w.push_back(t);
        if (w.empty()) continue;
        const std::string& c = w[0];
        auto err = [&](int r) { printf("ERR %d mlsgroup.cpp:%d\n", r, g.whyLine()); };
        if (c == "KP" && w.size() == 2) {
            uint64_t id = strtoull(w[1].c_str(), nullptr, 10);
            uint8_t be[8];
            for (int i = 0; i < 8; i++) be[i] = (uint8_t)(id >> (56 - 8 * i));
            Writer o(g_a, sizeof(g_a));
            if (g.makeKeyPackage(be, 8, rnd, o)) printf("OK %s\n", hex(g_a, o.n).c_str());
            else printf("ERR makeKeyPackage\n");
        } else if (c == "CREATE" && w.size() == 3) {
            auto gid = unhex(w[1]), ext = unhex(w[2]);
            printf(g.create(Bytes{gid.data(), gid.size()}, Bytes{ext.data(), ext.size()}, rnd) ? "OK\n" : "ERR create\n");
        } else if (c == "HANDLE" && w.size() == 2) {
            auto m = unhex(w[1]);
            MlsResult r = g.handle(Bytes{m.data(), m.size()});
            if (r == Mls_Ok) printf("OK\n");
            else err(r);
        } else if (c == "COMMIT") {
            Writer o(g_a, sizeof(g_a)), wl(g_b, sizeof(g_b));
            bool welcome = false;
            MlsResult r = g.commit(rnd, o, wl, &welcome);
            if (r != Mls_Ok) err(r);
            else printf("OK %s %s\n", hex(g_a, o.n).c_str(), welcome ? hex(g_b, wl.n).c_str() : "-");
        } else if (c == "JOIN" && w.size() == 2) {
            auto m = unhex(w[1]);
            MlsResult r = g.join(Bytes{m.data(), m.size()});
            if (r == Mls_Ok) printf("OK\n");
            else err(r);
        } else if (c == "EXPORT" && w.size() == 4) {
            auto label = unhex(w[1]), ctx = unhex(w[2]);
            size_t n = strtoul(w[3].c_str(), nullptr, 10);
            uint8_t out[64];
            if (n <= sizeof(out) && g.exporter(label.data(), label.size(), ctx.data(), ctx.size(), out, n)) printf("OK %s\n", hex(out, n).c_str());
            else printf("ERR export\n");
        } else if (c == "EPOCH") {
            printf("OK %llu\n", (unsigned long long)g.epoch());
        } else {
            printf("ERR unknown command\n");
        }
        fflush(stdout);
    }
}
