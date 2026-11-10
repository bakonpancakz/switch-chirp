#include "game.h"

#include <switch.h>
#include <string.h>

#include "screen.h"
#include "util.h"

namespace game {

uint64_t running() {
    // (one session, kept; opened once though two threads ask: the network's and the screen's)
    static Mutex lock = 0;
    static bool pm = false;
    mutexLock(&lock);
    if (!pm) pm = R_SUCCEEDED(pmdmntInitialize());
    mutexUnlock(&lock);
    if (!pm) return 0;
    u64 pid = 0, tid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid)) || R_FAILED(pmdmntGetProgramId(&tid, pid))) return 0;
    return tid;
}

bool name(uint64_t program_id, char* out, size_t cap) {
    out[0] = 0;
    size_t room = 0;
    uint8_t* buf = screen::borrowRing(&room);
    if (!buf) return false;
    bool ok = false;
    if (room >= sizeof(NsApplicationControlData) && R_SUCCEEDED(nsInitialize())) {
        auto* data = (NsApplicationControlData*)buf;
        u64 got = 0;
        if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, program_id, data, sizeof(*data), &got)) &&
            got >= sizeof(NacpStruct) && data->nacp.titles_data_format == 0) {
            // (English first, else the first language it has a name in. A game made for 21.0.0+ may keep its
            // names DEFLATE-compressed, titles_data_format 1: there's no inflate here, so it isn't named)
            const NacpLanguageEntry* pick = nullptr;
            for (int i = 0; i < 16 && !pick; i++)
                if (data->nacp.lang_data.lang[i].name[0]) pick = &data->nacp.lang_data.lang[i];
            if (pick) {
                utf8Copy(out, cap, pick->name, strnlen(pick->name, sizeof(pick->name)));  // (never half a character)
                ok = out[0] != 0;
            }
        }
        nsExit();
    }
    screen::returnRing();
    return ok;
}

}  // namespace game
