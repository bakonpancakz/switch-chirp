// Chirp: Discord voice chat on the Switch, whatever runs in front, through a
// bot of your own. It starts with the Switch and stays signed in to the
// gateway; its overlay talks to it over the "chirp" service (common/chirp.h).
//
// Three threads: this one answers IPC, app.cpp's speaks to Discord,
// mixer.cpp's feeds audout. Everything we keep is in fixed tables (store.h);
// the heap is for what libnx gives the system services (the sockets' buffers).
#include <switch.h>

#include <cstring>

#include "app.h"
#include "chirp.h"
#include "ipc_msg.h"
#include "mixer.h"

// 480 KB: libnx's, all of it (Chirp itself allocates nothing): the sockets' transfer memory (96 KB), 2 hardware
// Opus decoders (48 KB each), the threads' stacks (104 KB), and libnx's own (lookups, files, ssl: more than it
// looks; 384 KB left no room for even one decoder). chirp.log's system: lines say how much is in use.
// 608 KB since Go Live: the stream's connection (its stack, 40 KB) and a bigger socket pool (+53 KB) left
// 480 KB with no room for the screen's threads
#define INNER_HEAP_SIZE 0x98000

extern "C" {

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    timeInitialize();  // (the log's times; it copes when it isn't there)
    // (the card is opened when it's needed, card.cpp; the sockets and ssl by the network thread)
}

void __appExit(void) {
    timeExit();
    smExit();
}

}  // extern "C"

namespace {

constexpr int MAX_SESSIONS = 4;
const Result ERR_UNKNOWN_COMMAND = MAKERESULT(Module_Libnx, LibnxError_NotFound);
const Result ERR_BAD_ARGS = MAKERESULT(Module_Libnx, LibnxError_BadInput);

void reply(Result rc, const void* data, u32 size) { ipcReply(armGetTls(), rc, data, size); }

template <typename T>
bool inData(const HipcParsedRequest& req, T* out) {
    // (the request's raw data words hold at least the CMIF header and T)
    if (req.meta.num_data_words * 4 < 16 + sizeof(CmifInHeader) + sizeof(T)) return false;
    memcpy(out, ipcInData(armGetTls(), &req), sizeof(T));
    return true;
}

// One request: false when the session is to be closed
bool handle() {
    void* tls = armGetTls();
    HipcParsedRequest req = hipcParseRequest(tls);
    if (req.meta.type == CmifCommandType_Close) return false;
    const CmifInHeader* h = (const CmifInHeader*)cmifGetAlignedDataStart(req.data.data_words, tls);
    if (req.meta.type == CmifCommandType_Control) {
        if (h->command_id == 3) {  // QueryPointerBufferSize: none here (buffers are mapped)
            u16 size = 0;
            reply(0, &size, sizeof(size));
        } else {
            reply(ERR_UNKNOWN_COMMAND, nullptr, 0);
        }
        return true;
    }
    if (req.meta.type != CmifCommandType_Request || h->magic != CMIF_IN_HEADER_MAGIC) {
        reply(ERR_UNKNOWN_COMMAND, nullptr, 0);
        return true;
    }
    auto recvBuf = [&](size_t* size) -> void* {
        if (req.meta.num_recv_buffers < 1) return nullptr;
        *size = hipcGetBufferSize(&req.data.recv_buffers[0]);
        return hipcGetBufferAddress(&req.data.recv_buffers[0]);
    };
    // (each reads all of its request before doing anything: the card's IPC uses the TLS it's in)
    size_t size = 0;
    switch (h->command_id) {
        case ChirpCmd_GetVersion: {
            u32 v = CHIRP_PROTOCOL;
            reply(0, &v, sizeof(v));
            break;
        }
        case ChirpCmd_GetStatus: {
            // (into the caller's buffer: the status outgrew what fits in an IPC message itself, 0x100 bytes)
            void* p = recvBuf(&size);
            if (!p || size < sizeof(ChirpStatus)) {
                reply(ERR_BAD_ARGS, nullptr, 0);
                break;
            }
            ChirpStatus s;
            app::status(&s);
            memcpy(p, &s, sizeof(s));
            reply(0, nullptr, 0);
            break;
        }
        case ChirpCmd_ListGuilds:
        case ChirpCmd_ListChannels:
        case ChirpCmd_ListMembers: {
            u32 cmd = h->command_id;
            u64 key = 0;
            u32 first = 0;
            bool ok = cmd == ChirpCmd_ListGuilds ? inData(req, &first) : inData(req, &key);
            void* p = recvBuf(&size);
            if (!ok || !p) {
                reply(ERR_BAD_ARGS, nullptr, 0);
                break;
            }
            ChirpListed l = {};
            size_t total = 0, n = 0;
            if (cmd == ChirpCmd_ListGuilds) {
                n = app::listGuilds(first, (ChirpGuild*)p, size / sizeof(ChirpGuild), &total);
            } else if (cmd == ChirpCmd_ListChannels) {
                n = app::listChannels(key, (ChirpChannel*)p, size / sizeof(ChirpChannel));
                total = n;
            } else {
                n = app::listMembers(key, (ChirpMember*)p, size / sizeof(ChirpMember), &total);
            }
            l.written = (u32)n;
            l.total = (u32)total;
            reply(0, &l, sizeof(l));
            break;
        }
        case ChirpCmd_ListLogins: {
            void* p = recvBuf(&size);
            if (!p) {
                reply(ERR_BAD_ARGS, nullptr, 0);
                break;
            }
            ChirpListed l = {};
            l.written = l.total = (u32)app::listLogins((ChirpLogin*)p, size / sizeof(ChirpLogin));
            reply(0, &l, sizeof(l));
            break;
        }
        case ChirpCmd_SetMic: {
            ChirpMic m;
            if (!inData(req, &m)) {
                reply(ERR_BAD_ARGS, nullptr, 0);
                break;
            }
            app::setMic(m.volume, m.sensitivity, m.agc != 0, m.denoise != 0);
            reply(0, nullptr, 0);
            break;
        }
        case ChirpCmd_GetDebug: {
            void* p = recvBuf(&size);
            if (!p || size < sizeof(ChirpDebug)) {
                reply(ERR_BAD_ARGS, nullptr, 0);
                break;
            }
            ChirpDebug dbg;
            app::debug(&dbg);
            memcpy(p, &dbg, sizeof(dbg));
            reply(0, nullptr, 0);
            break;
        }
        case ChirpCmd_SetLogin: {
            ChirpLogin l;
            if (!inData(req, &l)) {
                reply(ERR_BAD_ARGS, nullptr, 0);
                break;
            }
            l.name[sizeof(l.name) - 1] = 0;
            app::setLogin(l.name);
            reply(0, nullptr, 0);
            break;
        }
        case ChirpCmd_Join: {
            ChirpJoin j;
            if (!inData(req, &j)) {
                reply(ERR_BAD_ARGS, nullptr, 0);
                break;
            }
            app::join(j);
            reply(0, nullptr, 0);
            break;
        }
        case ChirpCmd_Leave:
            app::leave();
            reply(0, nullptr, 0);
            break;
        case ChirpCmd_SetSelfMute:
        case ChirpCmd_SetSelfDeaf:
        case ChirpCmd_SetVolume:
        case ChirpCmd_SetScreenShare:
        case ChirpCmd_SetFullRange:
        case ChirpCmd_SetInvisible:
        case ChirpCmd_SetRichPresence:
        case ChirpCmd_SoundCheck: {
            u32 cmd = h->command_id, v = 0;
            if (!inData(req, &v)) {
                reply(ERR_BAD_ARGS, nullptr, 0);
                break;
            }
            if (cmd == ChirpCmd_SetSelfMute) app::setSelfMute(v != 0);
            else if (cmd == ChirpCmd_SetSelfDeaf) app::setSelfDeaf(v != 0);
            else if (cmd == ChirpCmd_SetVolume) app::setVolume((int)v);
            else if (cmd == ChirpCmd_SetScreenShare) app::setScreenShare(v != 0);
            else if (cmd == ChirpCmd_SetFullRange) app::setFullRange(v != 0);
            else if (cmd == ChirpCmd_SetInvisible) app::setInvisible(v != 0);
            else if (cmd == ChirpCmd_SetRichPresence) app::setRichPresence(v != 0);
            else app::soundCheck(v != 0);
            reply(0, nullptr, 0);
            break;
        }
        case ChirpCmd_SetUserMute:
        case ChirpCmd_SetUserVolume: {
            u32 cmd = h->command_id;
            ChirpUserSetting s;
            if (!inData(req, &s)) {
                reply(ERR_BAD_ARGS, nullptr, 0);
                break;
            }
            if (cmd == ChirpCmd_SetUserMute) app::setUserMute(s.user_id, s.value != 0);
            else app::setUserVolume(s.user_id, (int)s.value);
            reply(0, nullptr, 0);
            break;
        }
        case ChirpCmd_Reload:
            app::reload();
            reply(0, nullptr, 0);
            break;
        default:
            reply(ERR_UNKNOWN_COMMAND, nullptr, 0);
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    // the service first: the overlay checks the process is there, then asks sm
    // for it, which waits for it to be registered
    Handle handles[1 + MAX_SESSIONS];
    s32 count = 1;
    if (R_FAILED(smRegisterService(&handles[0], smEncodeName(CHIRP_SERVICE), false, MAX_SESSIONS))) {
        for (;;) svcSleepThread(UINT64_MAX);  // (nothing could reach it; there's nothing else to do)
    }
    mixer::start();
    app::start();
    Handle reply_to = INVALID_HANDLE;
    for (;;) {
        s32 idx = -1;
        Result rc = svcReplyAndReceive(&idx, handles, count, reply_to, UINT64_MAX);
        reply_to = INVALID_HANDLE;
        if (R_FAILED(rc)) {
            // a session went away (or the reply couldn't be sent): close it
            if (idx > 0 && idx < count) {
                svcCloseHandle(handles[idx]);
                handles[idx] = handles[--count];
            }
            continue;
        }
        if (idx == 0) {
            Handle s;
            if (R_SUCCEEDED(svcAcceptSession(&s, handles[0]))) {
                if (count < 1 + MAX_SESSIONS)
                    handles[count++] = s;
                else
                    svcCloseHandle(s);
            }
            continue;
        }
        if (idx < 0 || idx >= count) continue;
        if (handle()) {
            reply_to = handles[idx];
        } else {
            svcCloseHandle(handles[idx]);
            handles[idx] = handles[--count];
        }
    }
    app::stop();  // (never reached: the loop above runs until the Switch turns off)
    mixer::stop();
}
