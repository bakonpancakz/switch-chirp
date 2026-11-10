#include "gateway.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "store.h"
#include "util.h"

namespace {

bool is(const char* a, const char* b) { return strcmp(a, b) == 0; }

// "d.channels[].name" after "d.channels[]." is "name": the rest of path after prefix, or null
const char* under(const char* path, const char* prefix) {
    size_t n = strlen(prefix);
    if (strncmp(path, prefix, n) != 0) return nullptr;
    if (path[n] == '.') return path + n + 1;
    return nullptr;
}

long toLong(const char* s) {
    long v = 0;
    bool neg = *s == '-';
    if (neg) s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

uint64_t idOf(JsonType type, const char* s, size_t n) { return type == Json_String ? parseId(s, n) : 0; }

}  // namespace

void Gateway::start(WebSocket* ws, const char* token, WebSocket::RandomFn rnd, LogFn log, uint64_t now_ms) {
    ws_ = ws;
    rnd_ = rnd;
    log_ = log;
    snprintf(token_, sizeof(token_), "%s", token);
    hello_ = ready_ = over_ = false;
    acked_ = true;
    interval_ms_ = 0;
    next_beat_ms_ = 0;
    now_ms_ = now_ms;
    seq_ = -1;
    first_ack_ = false;
    error_[0] = 0;
    skipped_ = 0;
    me_ = 0;
    my_name_[0] = 0;
    session_id_[0] = 0;
    voice_session_[0] = 0;
    call_guild_ = call_channel_ = call_since_ms_ = 0;
    self_mute_ = self_deaf_ = false;
    streamClear();
    store::clear();
}

void Gateway::streamClear() {
    stream_server_ = stream_channel_ = 0;
    stream_active_[0] = 0;
    memset(stream_token_, 0, sizeof(stream_token_));
    stream_endpoint_[0] = 0;
    stream_serial_++;
}

void Gateway::streamKey(uint64_t guild, uint64_t channel, char out[80]) const {
    char g[21], c[21], u[21];
    idString(guild, g);
    idString(channel, c);
    idString(me_, u);
    snprintf(out, 80, "guild:%s:%s:%s", g, c, u);
}

bool Gateway::streamCreate(uint64_t guild, uint64_t channel) {
    if (!ready_ || !guild || !channel) return false;
    char g[21], c[21], key[80], msg[256];
    idString(guild, g);
    idString(channel, c);
    streamKey(guild, channel, key);
    snprintf(stream_active_, sizeof(stream_active_), "%s", key);
    say("asking Discord for a stream (Go Live)");
    snprintf(msg, sizeof(msg), "{\"op\":18,\"d\":{\"type\":\"guild\",\"guild_id\":\"%s\",\"channel_id\":\"%s\",\"preferred_region\":null}}", g, c);
    if (!send(msg)) return false;
    snprintf(msg, sizeof(msg), "{\"op\":22,\"d\":{\"stream_key\":\"%s\",\"paused\":false}}", key);
    return send(msg);
}

bool Gateway::streamDelete(uint64_t guild, uint64_t channel) {
    if (!ready_ || !guild || !channel) return false;
    char key[80], msg[160];
    streamKey(guild, channel, key);
    say("ending the stream");
    snprintf(msg, sizeof(msg), "{\"op\":19,\"d\":{\"stream_key\":\"%s\"}}", key);
    streamClear();
    return send(msg);
}

void Gateway::say(const char* fmt, ...) {
    if (!log_) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_(line);
}

void Gateway::fail(const char* why) {
    if (over_) return;
    over_ = true;
    snprintf(error_, sizeof(error_), "%s", why);
    if (ws_) ws_->sendClose(1000);
}

void Gateway::close() { fail("closed"); }

bool Gateway::send(const char* json) {
    if (over_ || !ws_->sendText(json, strlen(json))) {
        fail("couldn't send");
        return false;
    }
    return true;
}

void Gateway::identify() {
    char token[160];
    if (!jsonEscape(token, sizeof(token), token_)) {
        fail("the token is too long");
        return;
    }
    char msg[640];
    snprintf(msg, sizeof(msg),
             "{\"op\":2,\"d\":{\"token\":\"%s\",\"intents\":%d,"
             "\"properties\":{\"os\":\"horizon\",\"browser\":\"chirp\",\"device\":\"chirp\"},"
             "\"presence\":{\"since\":null,\"activities\":[],\"status\":\"%s\",\"afk\":false}}}",
             token, GATEWAY_INTENTS, invisible_ ? "invisible" : "online");
    send(msg);
    memset(msg, 0, sizeof(msg));  // (the token doesn't stay around in it)
}

void Gateway::heartbeat() {
    char msg[64];
    if (seq_ < 0) snprintf(msg, sizeof(msg), "{\"op\":1,\"d\":null}");
    else snprintf(msg, sizeof(msg), "{\"op\":1,\"d\":%lld}", (long long)seq_);
    acked_ = false;
    send(msg);
}

bool Gateway::tick(uint64_t now_ms) {
    now_ms_ = now_ms;
    if (over_) return false;
    if (hello_ && now_ms >= next_beat_ms_) {
        if (!acked_) {
            fail("no heartbeat ACK (the connection is dead)");
            return false;
        }
        heartbeat();
        next_beat_ms_ = now_ms + interval_ms_;
    }
    return !over_;
}

int Gateway::waitMs(uint64_t now_ms) const {
    if (!hello_ || next_beat_ms_ <= now_ms) return hello_ ? 0 : 1000;
    uint64_t left = next_beat_ms_ - now_ms;
    return left > 1000 ? 1000 : (int)left;
}

bool Gateway::voiceState(uint64_t guild, uint64_t channel, bool mute, bool deaf, bool video) {
    if (!ready_ || guild == 0) return false;
    say(channel ? "asking Discord to put the bot in voice" : "asking Discord to take the bot out of voice");
    char g[21], c[24];
    idString(guild, g);
    if (channel) {
        c[0] = '"';
        idString(channel, c + 1);
        strcat(c, "\"");
    } else {
        strcpy(c, "null");
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "{\"op\":4,\"d\":{\"guild_id\":\"%s\",\"channel_id\":%s,\"self_mute\":%s,\"self_deaf\":%s,\"self_video\":%s}}",
             g, c, mute ? "true" : "false", deaf ? "true" : "false", video && channel ? "true" : "false");
    return send(msg);
}

// --- reading a message ---

void Gateway::messageStart(bool text) {
    in_text_ = text;
    unreadable_ = false;
    json_.reset(this);
    op_ = -1;
    ev_ = Ev_None;
    d_begun_ = t_late_ = false;
    hello_interval_ = 0;
    if (!text) say("a binary message (we asked for JSON): skipped");
}

void Gateway::messageData(const char* p, size_t n) {
    if (!in_text_ || unreadable_) return;
    if (!json_.feed(p, n)) unreadable_ = true;
}

void Gateway::messageEnd() {
    if (!in_text_) return;
    if (unreadable_ || !json_.done()) {
        say("a message that isn't JSON: skipped");
        return;
    }
    switch (op_) {
        case 10:  // Hello
            if (hello_ || hello_interval_ == 0) break;
            hello_ = true;
            interval_ms_ = hello_interval_;
            {
                // the first heartbeat after interval * a random fraction, as Discord asks
                uint32_t r = 0;
                rnd_(&r, sizeof(r));
                next_beat_ms_ = now_ms_ + (uint64_t)interval_ms_ * (r % 1000) / 1000;
            }
            say("Hello: heartbeat every %u ms", interval_ms_);
            identify();
            break;
        case 11:  // Heartbeat ACK
            if (!acked_ && !first_ack_) say("heartbeats are getting through");
            if (!acked_) first_ack_ = true;
            acked_ = true;
            break;
        case 1:  // Discord wants a heartbeat now
            heartbeat();
            break;
        case 7:
            fail("Discord asked for a reconnect");
            break;
        case 9:
            fail("Discord said the session is invalid");
            break;
        case 0:
            if (t_late_) {
                skipped_++;
                say("an event whose name came after its data: skipped (%u so far)", skipped_);
            } else {
                dispatchEnd();
            }
            break;
        default:
            break;
    }
}

void Gateway::dispatchEnd() {
    if (ev_ == Ev_Ready) {
        ready_ = true;
        const Text* n = my_names_.best();
        utf8Copy(my_name_, sizeof(my_name_), n ? n->s : "", n ? n->n : 0);
        say("READY: signed in as %s, in %u guilds", my_name_, ready_guilds_);
    }
}

// Discord sends a guild either in full inside READY (as it may for a sharded bot), or as an
// "unavailable" stub there and in full in a GUILD_CREATE after. Each of READY's guilds[] is read as a
// GUILD_CREATE: a full one is kept, a stub (only an id) is dropped until its GUILD_CREATE comes
bool Gateway::readyGuild(const char* path, char* out, size_t cap) const {
    if (ev_ != Ev_Ready) return false;
    if (is(path, "d.guilds[]")) {
        snprintf(out, cap, "d");
        return true;
    }
    const char* rest = under(path, "d.guilds[]");
    if (!rest) return false;
    int n = snprintf(out, cap, "d.%s", rest);
    return n > 0 && (size_t)n < cap;
}

void Gateway::begin(const char* path, bool array) {
    char as[160];
    if (readyGuild(path, as, sizeof(as))) {
        ev_ = Ev_GuildCreate;
        begin(as, array);
        ev_ = Ev_Ready;
        return;
    }
    if (is(path, "d")) {
        d_begun_ = true;
        if (op_ == 0 && ev_ == Ev_None) t_late_ = true;
        if (ev_ == Ev_GuildCreate) {
            guild_slot_ = store::guildBegin();
            guild_id_ = 0;
            guild_unavailable_ = false;
            guild_name_.n = 0;
            guild_icon_.n = 0;
            guild_icon_seen_ = false;
            names_n_ = 0;
        } else if (ev_ == Ev_GuildUpdate || ev_ == Ev_GuildDelete) {
            guild_id_ = 0;
            guild_unavailable_ = false;
            guild_name_.n = 0;
            guild_icon_.n = 0;
            guild_icon_seen_ = false;
        } else if (ev_ == Ev_ChannelSet || ev_ == Ev_ChannelDelete) {
            chan_ = ChannelRead{};
        } else if (ev_ == Ev_VoiceState) {
            voice_ = VoiceRead{};
        } else if (ev_ == Ev_VoiceServer) {
            server_token_[0] = server_endpoint_[0] = 0;
            server_guild_ = 0;
        } else if (ev_ == Ev_Ready) {
            my_names_ = Names{};
            ready_guilds_ = 0;
        }
        return;
    }
    if (ev_ == Ev_GuildCreate && !array) {
        if (is(path, "d.channels[]")) chan_ = ChannelRead{};
        else if (is(path, "d.voice_states[]")) voice_ = VoiceRead{};
        else if (is(path, "d.members[]")) member_ = MemberRead{};
    }
}

// An avatar hash as Discord gives it (hex, "a_" in front when it moves): it
// becomes a file name, so nothing else gets through
static bool avatarHash(const char* s, size_t n) {
    if (n < 8 || n > 34) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (i < 2 && (c == 'a' || c == '_')))) return false;
    }
    return true;
}

void Gateway::iconFile(char out[CHIRP_AVATAR]) const {
    out[0] = 0;
    if (avatarHash(guild_icon_.s, guild_icon_.n)) snprintf(out, CHIRP_AVATAR, "i%.*s", (int)guild_icon_.n, guild_icon_.s);
}

void Gateway::myAvatar(char out[CHIRP_AVATAR]) const {
    out[0] = 0;
    if (avatarHash(my_avatar_.s, my_avatar_.n)) snprintf(out, CHIRP_AVATAR, "%.*s", (int)my_avatar_.n, my_avatar_.s);
}

bool Gateway::presence(const char* game, uint64_t start_ms) {
    if (!ready_) return false;
    const char* status = invisible_ ? "invisible" : "online";
    char msg[384];
    if (!game || !game[0]) {
        snprintf(msg, sizeof(msg), "{\"op\":3,\"d\":{\"since\":null,\"activities\":[],\"status\":\"%s\",\"afk\":false}}", status);
    } else {
        char name[CHIRP_NAME * 2];
        if (!jsonEscape(name, sizeof(name), game)) return false;
        // (Discord's docs: a bot may set only name, state, type and url. The start time is sent anyway,
        // for the day it counts: "Playing <game>" is what shows)
        snprintf(msg, sizeof(msg),
                 "{\"op\":3,\"d\":{\"since\":null,\"activities\":[{\"name\":\"%s\",\"type\":0,\"timestamps\":{\"start\":%llu}}],"
                 "\"status\":\"%s\",\"afk\":false}}",
                 name, (unsigned long long)start_ms, status);
    }
    return send(msg);
}

void Gateway::Names::avatarFile(char out[CHIRP_AVATAR]) const {
    out[0] = 0;
    if (avatarHash(member_avatar.s, member_avatar.n)) snprintf(out, CHIRP_AVATAR, "g%.*s", (int)member_avatar.n, member_avatar.s);
    else if (avatarHash(user_avatar.s, user_avatar.n)) snprintf(out, CHIRP_AVATAR, "%.*s", (int)user_avatar.n, user_avatar.s);
}

bool Gateway::namesValue(const char* rest, JsonType type, const char* s, size_t n, Names* names) {
    if (is(rest, "nick")) names->nick.set(type, s, n);
    else if (is(rest, "avatar")) names->member_avatar.set(type, s, n);
    else if (is(rest, "user.avatar")) names->user_avatar.set(type, s, n);
    else if (is(rest, "user.global_name")) names->global.set(type, s, n);
    else if (is(rest, "user.username")) names->user.set(type, s, n);
    else return false;
    return true;
}

bool Gateway::channelValue(const char* rest, JsonType type, const char* s, size_t n) {
    if (is(rest, "id")) chan_.id = idOf(type, s, n);
    else if (is(rest, "guild_id")) chan_.guild = idOf(type, s, n);
    else if (is(rest, "parent_id")) chan_.parent = idOf(type, s, n);
    else if (is(rest, "type") && type == Json_Number) chan_.type = (int)toLong(s);
    else if (is(rest, "position") && type == Json_Number) chan_.position = (int)toLong(s);
    else if (is(rest, "name")) chan_.name.set(type, s, n);
    else return false;
    return true;
}

bool Gateway::voiceValue(const char* rest, JsonType type, const char* s, size_t n) {
    uint8_t flag = 0;
    if (is(rest, "user_id")) voice_.user = idOf(type, s, n);
    else if (is(rest, "channel_id")) voice_.channel = idOf(type, s, n);
    else if (is(rest, "guild_id")) voice_.guild = idOf(type, s, n);
    else if (is(rest, "session_id")) snprintf(voice_.session, sizeof(voice_.session), "%.*s", type == Json_String ? (int)n : 0, s);
    else if (is(rest, "self_mute")) flag = ChirpMember_SelfMute;
    else if (is(rest, "self_deaf")) flag = ChirpMember_SelfDeaf;
    else if (is(rest, "mute")) flag = ChirpMember_ServerMute;
    else if (is(rest, "deaf")) flag = ChirpMember_ServerDeaf;
    else if (const char* m = under(rest, "member")) return namesValue(m, type, s, n, &voice_.names);
    else return false;
    if (flag && type == Json_True) voice_.flags |= flag;
    return true;
}

void Gateway::value(const char* path, JsonType type, const char* s, size_t n, bool cut) {
    char as[160];
    if (readyGuild(path, as, sizeof(as))) {
        if (is(as, "d.id")) ready_guilds_++;
        ev_ = Ev_GuildCreate;
        value(as, type, s, n, cut);
        ev_ = Ev_Ready;
        return;
    }
    if (is(path, "op")) {
        if (type == Json_Number) op_ = (int)toLong(s);
        return;
    }
    if (is(path, "s")) {
        if (type == Json_Number) seq_ = toLong(s);
        return;
    }
    if (is(path, "t")) {
        if (type != Json_String) return;
        if (d_begun_) t_late_ = true;
        if (is(s, "READY")) ev_ = Ev_Ready;
        else if (is(s, "GUILD_CREATE")) ev_ = Ev_GuildCreate;
        else if (is(s, "GUILD_UPDATE")) ev_ = Ev_GuildUpdate;
        else if (is(s, "GUILD_DELETE")) ev_ = Ev_GuildDelete;
        else if (is(s, "CHANNEL_CREATE") || is(s, "CHANNEL_UPDATE")) ev_ = Ev_ChannelSet;
        else if (is(s, "CHANNEL_DELETE")) ev_ = Ev_ChannelDelete;
        else if (is(s, "VOICE_STATE_UPDATE")) ev_ = Ev_VoiceState;
        else if (is(s, "VOICE_SERVER_UPDATE")) ev_ = Ev_VoiceServer;
        else if (is(s, "STREAM_CREATE") || is(s, "STREAM_UPDATE")) ev_ = Ev_StreamCreate;
        else if (is(s, "STREAM_SERVER_UPDATE")) ev_ = Ev_StreamServer;
        else if (is(s, "STREAM_DELETE")) ev_ = Ev_StreamDelete;
        else ev_ = Ev_Other;
        if (ev_ >= Ev_StreamCreate) stream_key_[0] = 0, stream_rtc_ = stream_rtc_channel_ = 0, server_token_[0] = server_endpoint_[0] = 0;
        return;
    }
    const char* d = under(path, "d");
    if (!d) return;
    if (op_ == 10 || ev_ == Ev_None) {
        if (is(d, "heartbeat_interval") && type == Json_Number) hello_interval_ = (uint32_t)toLong(s);
        return;
    }
    switch (ev_) {
        case Ev_Ready:
            if (is(d, "user.id")) me_ = idOf(type, s, n);
            else if (is(d, "session_id")) snprintf(session_id_, sizeof(session_id_), "%.*s", type == Json_String ? (int)n : 0, s);
            else if (is(d, "user.avatar")) my_avatar_.set(type, s, n);
            else if (is(d, "user.global_name")) my_names_.global.set(type, s, n);
            else if (is(d, "user.username")) my_names_.user.set(type, s, n);
            break;
        case Ev_GuildCreate:
        case Ev_GuildUpdate:
        case Ev_GuildDelete:
            if (is(d, "id")) guild_id_ = idOf(type, s, n);
            else if (is(d, "name")) guild_name_.set(type, s, n);
            else if (is(d, "icon")) guild_icon_.set(type, s, n), guild_icon_seen_ = true;
            else if (is(d, "unavailable")) guild_unavailable_ = type == Json_True;
            else if (ev_ == Ev_GuildCreate) {
                if (const char* r = under(path, "d.channels[]")) channelValue(r, type, s, n);
                else if (const char* r2 = under(path, "d.voice_states[]")) voiceValue(r2, type, s, n);
                else if (const char* r3 = under(path, "d.members[]")) {
                    if (is(r3, "user.id")) member_.user = idOf(type, s, n);
                    else namesValue(r3, type, s, n, &member_.names);
                }
            }
            break;
        case Ev_ChannelSet:
        case Ev_ChannelDelete:
            channelValue(d, type, s, n);
            break;
        case Ev_VoiceState:
            voiceValue(d, type, s, n);
            break;
        case Ev_StreamCreate:
        case Ev_StreamServer:
        case Ev_StreamDelete: {
            int len = type == Json_String ? (int)n : 0;
            if (is(d, "stream_key")) snprintf(stream_key_, sizeof(stream_key_), "%.*s", len, s);
            else if (is(d, "rtc_server_id")) stream_rtc_ = idOf(type, s, n);
            else if (is(d, "rtc_channel_id")) stream_rtc_channel_ = idOf(type, s, n);
            else if (is(d, "token")) snprintf(server_token_, sizeof(server_token_), "%.*s", len, s);
            else if (is(d, "endpoint")) snprintf(server_endpoint_, sizeof(server_endpoint_), "%.*s", len, s);
            else if (is(d, "reason") && ev_ == Ev_StreamDelete) say("Discord ended the stream: %.*s", len, s);
            break;
        }
        case Ev_VoiceServer: {
            int len = type == Json_String ? (int)n : 0;
            if (is(d, "token")) snprintf(server_token_, sizeof(server_token_), "%.*s", len, s);
            else if (is(d, "endpoint")) snprintf(server_endpoint_, sizeof(server_endpoint_), "%.*s", len, s);
            else if (is(d, "guild_id")) server_guild_ = idOf(type, s, n);
            break;
        }
        default:
            break;
    }
    (void)cut;
}

void Gateway::commitChannel(int guild_slot) {
    store::channelSet(guild_slot, chan_.id, chan_.parent, chan_.position, chan_.type, chan_.name.s, chan_.name.n);
}

void Gateway::commitVoice(int guild_slot) {
    const Text* name = voice_.names.best();
    char avatar[CHIRP_AVATAR];
    voice_.names.avatarFile(avatar);
    store::memberSet(guild_slot, voice_.user, voice_.channel, voice_.flags, name ? name->s : nullptr, name ? name->n : 0, avatar);
    if (voice_.user != me_ || me_ == 0) return;
    // (inside a GUILD_CREATE the guild's id may not be in its slot yet)
    const store::Guild* g = store::guildAt(guild_slot);
    uint64_t guild = g && g->id ? g->id : voice_.guild ? voice_.guild : guild_id_;
    // The bot's voice state belongs to the gateway session that asked for it. One with another session's
    // id is the same bot somewhere else (another Switch, a PC, or our own last session, before a restart
    // or a dropped connection): it's shown in that channel's list, but it isn't our call. (No session id:
    // taken as ours, there's nothing to tell them apart by.)
    bool ours = !voice_.session[0] || !session_id_[0] || !strcmp(voice_.session, session_id_);
    if (!ours) {
        if (voice_.channel && guild == call_guild_ && call_channel_) {
            // (the call was taken over elsewhere: a bot has one voice state a guild)
            say("the bot's call was taken over by another session");
            call_guild_ = call_channel_ = call_since_ms_ = 0;
            if (stream_server_ || stream_endpoint_[0]) streamClear();
        } else if (voice_.channel) {
            const store::Channel* c = store::channelFind(voice_.channel);
            say("the bot is in voice channel %s from another session (not this one's call)", c ? c->name : "(one not known yet)");
        }
        return;
    }
    if (voice_.channel) {
        if (voice_.channel != call_channel_) {
            call_since_ms_ = now_ms_;
            const store::Channel* c = store::channelFind(voice_.channel);
            say("the bot is in voice channel %s", c ? c->name : "(one not known yet)");
        }
        call_guild_ = guild;  // (0 inside a GUILD_CREATE whose id comes later: filled in when it's read)
        call_channel_ = voice_.channel;
        if (voice_.session[0]) snprintf(voice_session_, sizeof(voice_session_), "%s", voice_.session);
        self_mute_ = (voice_.flags & ChirpMember_SelfMute) != 0;
        self_deaf_ = (voice_.flags & ChirpMember_SelfDeaf) != 0;
    } else if (guild == call_guild_ || call_guild_ == 0) {
        if (call_channel_) say("the bot left voice");
        call_guild_ = call_channel_ = call_since_ms_ = 0;
        if (stream_server_ || stream_endpoint_[0]) streamClear();  // (a stream ends with its call)
    }
}

void Gateway::end(const char* path, bool array) {
    char as[160];
    if (readyGuild(path, as, sizeof(as))) {
        ev_ = Ev_GuildCreate;
        end(as, array);
        ev_ = Ev_Ready;
        return;
    }
    if (array) return;
    if (ev_ == Ev_GuildCreate) {
        if (is(path, "d.channels[]")) {
            commitChannel(guild_slot_);
        } else if (is(path, "d.voice_states[]")) {
            commitVoice(guild_slot_);
        } else if (is(path, "d.members[]")) {
            const Text* best = member_.names.best();
            if (best && member_.user && names_n_ < GATEWAY_NAMES) {
                NameKept& k = names_[names_n_++];
                k.user = member_.user;
                k.name = *best;
                member_.names.avatarFile(k.avatar);
            }
        } else if (is(path, "d")) {
            if (guild_unavailable_ || guild_id_ == 0) {
                store::guildAbandon(guild_slot_);
            } else {
                char icon[CHIRP_AVATAR];
                iconFile(icon);
                store::guildCommit(guild_slot_, guild_id_, guild_name_.s, guild_name_.n, icon);
                const store::Guild* g = store::guildAt(guild_slot_);
                say("server: %s", g ? g->name : "?");
                // (the bot already in one of its voice channels when the session began: now its guild is known)
                if (call_channel_ && call_guild_ == 0) {
                    const store::Channel* c = store::channelFind(call_channel_);
                    if (c && c->guild == guild_slot_) call_guild_ = guild_id_;
                }
                for (size_t i = 0; i < names_n_; i++) store::memberName(guild_slot_, names_[i].user, names_[i].name.s, names_[i].name.n, names_[i].avatar);
            }
            guild_slot_ = -1;
        }
        return;
    }
    if (!is(path, "d")) return;
    switch (ev_) {
        case Ev_GuildUpdate:
            if (guild_name_.n || guild_icon_seen_) {
                char icon[CHIRP_AVATAR];
                iconFile(icon);
                store::guildRename(guild_id_, guild_name_.s, guild_name_.n, guild_icon_seen_ ? icon : nullptr);
            }
            break;
        case Ev_GuildDelete:
            store::guildRemove(guild_id_);
            if (guild_id_ == call_guild_) call_guild_ = call_channel_ = call_since_ms_ = 0;
            break;
        case Ev_ChannelSet:
            commitChannel(store::guildFind(chan_.guild));
            break;
        case Ev_ChannelDelete:
            store::channelRemove(chan_.id);
            break;
        case Ev_VoiceState:
            if (voice_.guild) commitVoice(store::guildFind(voice_.guild));
            break;
        case Ev_VoiceServer:
            // (the token is the voice server's password: it isn't logged)
            memcpy(voice_token_, server_token_, sizeof(voice_token_));
            memcpy(voice_endpoint_, server_endpoint_, sizeof(voice_endpoint_));
            voice_server_guild_ = server_guild_;
            voice_server_serial_++;
            say("voice server: %s", voice_endpoint_[0] ? voice_endpoint_ : "(none yet)");
            break;
        case Ev_StreamCreate:
        case Ev_StreamServer:
        case Ev_StreamDelete: {
            // (only ours: "guild:<guild>:<channel>:<me>")
            char u[21];
            idString(me_, u);
            size_t kn = strlen(stream_key_), un = strlen(u);
            if (kn <= un + 1 || stream_key_[kn - un - 1] != ':' || strcmp(stream_key_ + kn - un, u) != 0) break;
            if (ev_ == Ev_StreamDelete) {
                say("the stream is over");
                // (ended by Discord, not by us: our own streamDelete has already let go of it)
                if (stream_active_[0] && !strcmp(stream_key_, stream_active_)) stream_ended_++;
                streamClear();
            } else if (ev_ == Ev_StreamCreate) {
                if (stream_rtc_ && stream_rtc_ != stream_server_) {
                    stream_server_ = stream_rtc_;
                    stream_channel_ = stream_rtc_channel_;
                    stream_serial_++;
                    char sv[21], ch[21];
                    idString(stream_server_, sv);
                    idString(stream_channel_, ch);
                    say("stream created: its server %s, its channel %s", sv, stream_channel_ ? ch : "(none given)");
                }
            } else {
                memcpy(stream_token_, server_token_, sizeof(stream_token_));
                memcpy(stream_endpoint_, server_endpoint_, sizeof(stream_endpoint_));
                stream_serial_++;
                say("stream server: %s", stream_endpoint_[0] ? stream_endpoint_ : "(none yet)");
            }
            memset(server_token_, 0, sizeof(server_token_));
            break;
        }
        default:
            break;
    }
}
