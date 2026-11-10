#include "client.h"

#include <stdio.h>

namespace client {
namespace {

Service g_srv;
bool g_open = false;
u32 g_version = 0;

// A call that failed: the sysmodule went away (or was never there)
bool check(Result rc) {
    if (R_FAILED(rc)) close();
    return R_SUCCEEDED(rc);
}

template <typename T, typename In>
std::vector<T> list(u32 cmd, const In& in) {
    std::vector<T> out(CHIRP_LIST_MAX);
    ChirpListed l = {};
    if (!g_open || !check(serviceDispatchInOut(&g_srv, cmd, in, l, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                               .buffers = {{out.data(), out.size() * sizeof(T)}}, )))
        return {};
    out.resize(l.written < out.size() ? l.written : out.size());
    return out;
}

void setU32(u32 cmd, u32 v) {
    if (g_open) check(serviceDispatchIn(&g_srv, cmd, v));
}

void setUser(u32 cmd, u64 user, u32 v) {
    ChirpUserSetting s = {user, v, 0};
    if (g_open) check(serviceDispatchIn(&g_srv, cmd, s));
}

}  // namespace

bool connect() {
    if (g_open) return true;
    // sm would wait for a service nobody registered: ask pm first whether it runs
    u64 pid = 0;
    if (R_FAILED(pmdmntGetProcessId(&pid, CHIRP_TITLE_ID))) return false;
    g_open = R_SUCCEEDED(smGetService(&g_srv, CHIRP_SERVICE));
    g_version = 0;
    if (g_open && R_FAILED(serviceDispatchOut(&g_srv, ChirpCmd_GetVersion, g_version))) g_version = 0;
    return g_open;
}

void close() {
    if (g_open) serviceClose(&g_srv);
    g_open = false;
}

bool connected() { return g_open; }
bool outdated() { return g_open && g_version < CHIRP_PROTOCOL; }

bool status(ChirpStatus* s) {
    // (into a buffer: ChirpStatus is more than an IPC message holds)
    return g_open && check(serviceDispatch(&g_srv, ChirpCmd_GetStatus, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                           .buffers = {{s, sizeof(*s)}}, ));
}

std::vector<ChirpGuild> guilds() {
    std::vector<ChirpGuild> all;
    // (in pages: the sysmodule says how many there are)
    for (u32 first = 0;;) {
        std::vector<ChirpGuild> page = list<ChirpGuild>(ChirpCmd_ListGuilds, first);
        all.insert(all.end(), page.begin(), page.end());
        if (page.size() < CHIRP_LIST_MAX) break;
        first += (u32)page.size();
    }
    return all;
}

std::vector<ChirpChannel> channels(u64 guild) { return list<ChirpChannel>(ChirpCmd_ListChannels, guild); }
std::vector<ChirpMember> members(u64 channel) { return list<ChirpMember>(ChirpCmd_ListMembers, channel); }

void join(u64 guild, u64 channel, bool muted) {
    ChirpJoin j = {guild, channel, muted ? 1u : 0u, 0};
    if (g_open) check(serviceDispatchIn(&g_srv, ChirpCmd_Join, j));
}

void leave() {
    if (g_open) check(serviceDispatch(&g_srv, ChirpCmd_Leave));
}

void setSelfMute(bool on) { setU32(ChirpCmd_SetSelfMute, on); }
void setSelfDeaf(bool on) { setU32(ChirpCmd_SetSelfDeaf, on); }
void setScreenShare(bool on) { setU32(ChirpCmd_SetScreenShare, on); }
void setFullRange(bool on) { setU32(ChirpCmd_SetFullRange, on); }
void setInvisible(bool on) { setU32(ChirpCmd_SetInvisible, on); }
void setRichPresence(bool on) { setU32(ChirpCmd_SetRichPresence, on); }

std::vector<ChirpLogin> logins() {
    std::vector<ChirpLogin> out(CHIRP_LOGINS);
    ChirpListed l = {};
    if (!g_open || !check(serviceDispatchOut(&g_srv, ChirpCmd_ListLogins, l, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                             .buffers = {{out.data(), out.size() * sizeof(ChirpLogin)}}, )))
        return {};
    out.resize(l.written < out.size() ? l.written : out.size());
    return out;
}

void setLogin(const char* name) {
    ChirpLogin l = {};
    snprintf(l.name, sizeof(l.name), "%s", name);
    if (g_open) check(serviceDispatchIn(&g_srv, ChirpCmd_SetLogin, l));
}
void setUserMute(u64 user, bool on) { setUser(ChirpCmd_SetUserMute, user, on); }
void setUserVolume(u64 user, int volume) { setUser(ChirpCmd_SetUserVolume, user, (u32)volume); }
void soundCheck(bool on) { setU32(ChirpCmd_SoundCheck, on); }
void setVolume(int volume) { setU32(ChirpCmd_SetVolume, (u32)volume); }

void setMic(int volume, int sensitivity, bool agc, bool denoise) {
    ChirpMic m = {(uint16_t)volume, (int8_t)sensitivity, (uint8_t)(agc ? 1 : 0), (uint8_t)(denoise ? 1 : 0), {0, 0, 0}};
    if (g_open) check(serviceDispatchIn(&g_srv, ChirpCmd_SetMic, m));
}

bool debug(ChirpDebug* d) {
    return g_open && check(serviceDispatch(&g_srv, ChirpCmd_GetDebug, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                           .buffers = {{d, sizeof(*d)}}, ));
}

void reload() {
    if (g_open) check(serviceDispatch(&g_srv, ChirpCmd_Reload));
}

}  // namespace client
