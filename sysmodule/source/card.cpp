#include "card.h"

namespace card {

static Mutex g_lock;
static int g_users = 0;

bool acquire() {
    mutexLock(&g_lock);
    bool ok = true;
    if (g_users == 0) {
        ok = R_SUCCEEDED(fsInitialize());
        if (ok && R_FAILED(fsdevMountSdmc())) {
            fsExit();
            ok = false;
        }
    }
    if (ok) g_users++;
    mutexUnlock(&g_lock);
    return ok;
}

void release() {
    // (the card stays mounted, on the one fs session, from its first use on: opening a session and
    // mounting for every log line churned sessions through Atmosphere's fs mitm, in ams.mitm, dozens
    // a second when the log was busy. Files are still opened and closed each time, so FTP can copy them)
    mutexLock(&g_lock);
    if (g_users > 1) g_users--;
    mutexUnlock(&g_lock);
}

}  // namespace card
