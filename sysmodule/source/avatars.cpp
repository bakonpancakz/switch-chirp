#include "avatars.h"

#include <sys/stat.h>
#include <switch.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "card.h"
#include "chirp.h"
#include "tls.h"
#include "util.h"

namespace avatars {
namespace {

const char* const CDN = "cdn.discordapp.com";
constexpr int QUEUE = 16;
constexpr int SEEN = 128;               // pictures asked for already (by a hash of their file name)
constexpr size_t MAX_BYTES = 256 << 10;  // (a 64 px PNG is a few KB)
constexpr int IDLE_MS = 10000;           // nothing from the CDN this long: given up on
constexpr int SIZE = 64;                 // asked for at this size; the overlay shrinks it

struct Want {
    uint64_t user, guild;
    char file[CHIRP_AVATAR];
};

Mutex g_lock;
CondVar g_cv;
Thread g_thread;
volatile bool g_run = false;
Want g_queue[QUEUE];
int g_head = 0, g_count = 0;
uint64_t g_seen[SEEN];
int g_seen_next = 0;
// (pictures that didn't come: asked for again only after a while. Once, a failure was never tried again
// until Chirp restarted, the picture stayed a letter all session; and the overlay asks every poll)
constexpr int FAILED = 16;
constexpr uint64_t RETRY_MS = 60'000;
struct Failed {
    uint64_t hash, until_ms;
} g_failed[FAILED];
int g_failed_next = 0;

uint64_t nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

uint64_t fnv(const char* s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ULL;
    return h;
}

TlsStream g_tls;
char g_buf[2048];

// One picture: false (and why) if it couldn't be had
bool fetch(const Want& w, char* why, size_t why_cap) {
    char path[160], uid[21], gid[21];
    idString(w.user, uid);
    idString(w.guild, gid);
    // ("g": a server's own picture of someone; "i": a server's icon (.png: an animated one's first frame))
    bool server = w.file[0] == 'g', icon = w.file[0] == 'i';
    const char* hash = server || icon ? w.file + 1 : w.file;
    if (icon) snprintf(path, sizeof(path), "/icons/%s/%s.png?size=%d", gid, hash, SIZE);
    else if (server) snprintf(path, sizeof(path), "/guilds/%s/users/%s/avatars/%s.png?size=%d", gid, uid, hash, SIZE);
    else snprintf(path, sizeof(path), "/avatars/%s/%s.png?size=%d", uid, hash, SIZE);

    if (!g_tls.open(CDN, why, why_cap)) return false;
    // (HTTP/1.0: the body ends when the connection does, never chunked)
    int n = snprintf(g_buf, sizeof(g_buf), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Chirp (Nintendo Switch)\r\n\r\n", path, CDN);
    if (!g_tls.write(g_buf, n)) {
        snprintf(why, why_cap, "couldn't ask the CDN");
        g_tls.close();
        return false;
    }

    char part[96], done[96];
    snprintf(part, sizeof(part), CHIRP_AVATARS "/%s.part", w.file);
    snprintf(done, sizeof(done), CHIRP_AVATARS "/%s.png", w.file);
    Card c;
    FILE* f = c.ok ? fopen(part, "wb") : nullptr;
    if (!f) {
        snprintf(why, why_cap, "can't write the card (%s)", w.file);
        g_tls.close();
        return false;
    }

    size_t got = 0, header = 0, body = 0, length = 0;
    bool header_done = false, ok = true;
    int idle = 0;
    for (;;) {
        int r = g_tls.read(g_buf + got, (int)(sizeof(g_buf) - got), 500);
        if (r < 0) break;  // (the CDN closed it: that's the end of the body)
        if (r == 0) {
            if ((idle += 500) >= IDLE_MS) {
                snprintf(why, why_cap, "the CDN stopped answering");
                ok = false;
                break;
            }
            continue;
        }
        idle = 0;
        got += (size_t)r;
        size_t from = 0;
        if (!header_done) {
            // the status line and the headers, all in the first 2 KB
            for (size_t i = 3; i < got && !header_done; i++) {
                if (g_buf[i - 3] == '\r' && g_buf[i - 2] == '\n' && g_buf[i - 1] == '\r' && g_buf[i] == '\n') {
                    header_done = true;
                    header = i + 1;
                }
            }
            if (!header_done) {
                if (got == sizeof(g_buf)) {
                    snprintf(why, why_cap, "the CDN's header is too long");
                    ok = false;
                    break;
                }
                continue;
            }
            if (header < 12 || memcmp(g_buf, "HTTP/1.", 7) != 0 || memcmp(g_buf + 9, "200", 3) != 0) {
                snprintf(why, why_cap, "the CDN said %.12s", g_buf);
                ok = false;
                break;
            }
            for (size_t i = 0; i + 16 < header; i++) {
                if (strncasecmp(g_buf + i, "\ncontent-length:", 16) == 0) {
                    length = 0;
                    for (size_t k = i + 16; k < header && (g_buf[k] == ' ' || (g_buf[k] >= '0' && g_buf[k] <= '9')); k++)
                        if (g_buf[k] != ' ') length = length * 10 + (size_t)(g_buf[k] - '0');
                }
            }
            from = header;
        }
        size_t n_body = got - from;
        if (body + n_body > MAX_BYTES) {
            snprintf(why, why_cap, "a picture over %u KB", (unsigned)(MAX_BYTES >> 10));
            ok = false;
            break;
        }
        if (n_body && fwrite(g_buf + from, 1, n_body, f) != n_body) {
            snprintf(why, why_cap, "the card is full?");
            ok = false;
            break;
        }
        body += n_body;
        got = 0;
        if (length && body >= length) break;
    }
    g_tls.close();
    fclose(f);
    if (ok && (!header_done || body == 0 || (length && body != length))) {
        snprintf(why, why_cap, "the picture came cut short");
        ok = false;
    }
    if (ok) {
        remove(done);
        ok = rename(part, done) == 0;
        if (!ok) snprintf(why, why_cap, "can't rename %s.part", w.file);
    }
    if (!ok) remove(part);
    return ok;
}

void thread(void*) {
    {
        Card c;
        if (c.ok) mkdir(CHIRP_AVATARS, 0777);
    }
    while (g_run) {
        mutexLock(&g_lock);
        while (g_run && g_count == 0) condvarWaitTimeout(&g_cv, &g_lock, 1'000'000'000ULL);
        if (!g_run) {
            mutexUnlock(&g_lock);
            break;
        }
        Want w = g_queue[g_head];
        g_head = (g_head + 1) % QUEUE;
        g_count--;
        mutexUnlock(&g_lock);

        char done[96];
        snprintf(done, sizeof(done), CHIRP_AVATARS "/%s.png", w.file);
        struct stat st;
        bool there;
        {
            Card c;
            there = c.ok && stat(done, &st) == 0 && st.st_size > 0;
        }
        if (there) continue;
        char why[96] = {0};
        if (!fetch(w, why, sizeof(why))) {
            char line[160];
            snprintf(line, sizeof(line), "an avatar didn't come: %s (asked for again in a minute)", why);
            app::log(line);
            uint64_t h = fnv(w.file);
            mutexLock(&g_lock);
            for (uint64_t& s : g_seen)
                if (s == h) s = 0;
            g_failed[g_failed_next] = Failed{h, nowMs() + RETRY_MS};
            g_failed_next = (g_failed_next + 1) % FAILED;
            mutexUnlock(&g_lock);
        }
    }
}

}  // namespace

void start() {
    if (g_run) return;
    mutexInit(&g_lock);
    condvarInit(&g_cv);
    g_run = true;
    if (R_FAILED(threadCreate(&g_thread, thread, nullptr, nullptr, 0x4000, 0x2D, -2))) {
        g_run = false;
        return;
    }
    threadStart(&g_thread);
}

void stop() {
    if (!g_run) return;
    g_run = false;
    condvarWakeAll(&g_cv);
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
}

void want(uint64_t user, uint64_t guild, const char* file) {
    if (!g_run || !file || !file[0]) return;
    if (user == 0 && file[0] != 'i') return;  // (a server's icon has no user: anything else needs one)
    uint64_t h = fnv(file);
    mutexLock(&g_lock);
    bool seen = false;
    for (uint64_t s : g_seen) seen = seen || s == h;
    uint64_t now = nowMs();
    for (const Failed& f : g_failed) seen = seen || (f.hash == h && now < f.until_ms);
    if (!seen && g_count < QUEUE) {
        g_seen[g_seen_next] = h;
        g_seen_next = (g_seen_next + 1) % SEEN;
        Want& w = g_queue[(g_head + g_count) % QUEUE];
        w.user = user;
        w.guild = guild;
        snprintf(w.file, sizeof(w.file), "%s", file);
        g_count++;
        condvarWakeOne(&g_cv);
    }
    mutexUnlock(&g_lock);
}

}  // namespace avatars
