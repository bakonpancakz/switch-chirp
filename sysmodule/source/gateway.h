// One Discord gateway session for Chirp's bot, over a websocket that's
// already open: Hello, Identify, heartbeats, and the events that fill the
// store (the GUILDS and GUILD_VOICE_STATES intents are all it asks for). No
// resuming: when anything goes wrong the session is over and app.cpp starts
// a new one.
//
// A bot's READY only lists its guilds' ids; each guild then comes whole in a
// GUILD_CREATE (name, channels, voice states, and the members it includes,
// whose names the voice states lack).
//
// The events are read as they stream in (json.h). That needs "t" (the
// event's name) to come before "d" (its data), as Discord sends them; an
// event that doesn't is skipped and counted in the log.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "chirp.h"
#include "json.h"
#include "ws.h"

#define GATEWAY_INTENTS ((1 << 0) | (1 << 7))  // GUILDS, GUILD_VOICE_STATES
#define GATEWAY_NAMES 64  // member names one GUILD_CREATE can bring for its voice states
#define GATEWAY_VIEWERS 32  // our stream's viewers followed (each one's coming and going: the call's sounds)

class Gateway : public WsHandler, public JsonHandler {
public:
    typedef void (*LogFn)(const char* line);

    // token: the bot's, from token.txt
    void start(WebSocket* ws, const char* token, WebSocket::RandomFn rnd, LogFn log, uint64_t now_ms);
    // Heartbeats when they're due; false once the session is over (why: in error())
    bool tick(uint64_t now_ms);
    // The longest poll() may wait before tick() is needed again
    int waitMs(uint64_t now_ms) const;

    // The bot's voice state (op 4). Leave: channel 0.
    // (video: self_video, our camera-style video on: Discord's apps show it, and ask for it, only then)
    bool voiceState(uint64_t guild, uint64_t channel, bool mute, bool deaf, bool video = false);
    // Go Live: a stream of the call's channel (op 18, then op 22 unpaused); ended with op 19. Discord
    // answers with STREAM_CREATE (the stream's own voice server id) and STREAM_SERVER_UPDATE (its
    // endpoint and token), and STREAM_DELETE when it's over
    bool streamCreate(uint64_t guild, uint64_t channel);
    // Appear invisible (offline to everyone, still connected): said in Identify, so it never shows online
    // first; changed later with presence()
    void setInvisible(bool on) { invisible_ = on; }
    // The bot's presence (op 3): invisible or online, and Rich Presence's "Playing <game>" (null or empty:
    // nothing) since start_ms (Unix ms; 0: none)
    bool presence(const char* game, uint64_t start_ms);
    bool streamDelete(uint64_t guild, uint64_t channel);
    // Our stream, as Discord last said (no endpoint: none yet; the serial goes up with each change)
    uint64_t streamServer() const { return stream_server_; }
    // (its own channel: the stream's DAVE group, and what its connection names; 0: Discord gave none)
    uint64_t streamChannel() const { return stream_channel_; }
    const char* streamToken() const { return stream_token_; }
    const char* streamEndpoint() const { return stream_endpoint_; }
    uint32_t streamSerial() const { return stream_serial_; }
    // Goes up each time Discord ends our stream itself (not our own streamDelete): refused, or ended for us
    uint32_t streamEnded() const { return stream_ended_; }
    // Our stream's viewers (STREAM_CREATE's and STREAM_UPDATE's viewer_ids): how many watch now, and each
    // one coming and going (each goes up by one each time)
    size_t streamViewers() const { return viewers_n_; }
    uint32_t viewersJoined() const { return viewers_joined_; }
    uint32_t viewersLeft() const { return viewers_left_; }
    void close();  // (ends the session: a new one starts from scratch)

    bool ready() const { return ready_; }
    bool over() const { return over_; }
    const char* error() const { return error_; }

    // From READY, and the bot's VOICE_STATE_UPDATEs. The call is only this session's: the bot in a
    // channel through another session (another device, or ours before a reconnect) isn't in one here
    uint64_t me() const { return me_; }
    const char* myName() const { return my_name_; }
    // The bot's picture: its file name for avatars (empty: none)
    void myAvatar(char out[CHIRP_AVATAR]) const;
    uint64_t callGuild() const { return call_guild_; }
    uint64_t callChannel() const { return call_channel_; }
    uint64_t callSinceMs() const { return call_since_ms_; }
    // Other people coming into our call's channel, and going out of it (as it happens: not the voice
    // states a session starts with); each goes up by one each time (the call's sounds, sounds.h)
    uint32_t othersJoined() const { return others_joined_; }
    uint32_t othersLeft() const { return others_left_; }
    bool selfMute() const { return self_mute_; }
    bool selfDeaf() const { return self_deaf_; }
    // For the voice connection: the bot's voice session (its VOICE_STATE_UPDATE), and the voice
    // server Discord gave it (VOICE_SERVER_UPDATE; the serial goes up with each one; no endpoint: none yet)
    const char* voiceSession() const { return voice_session_; }
    const char* voiceToken() const { return voice_token_; }
    const char* voiceEndpoint() const { return voice_endpoint_; }
    uint64_t voiceServerGuild() const { return voice_server_guild_; }
    uint32_t voiceServerSerial() const { return voice_server_serial_; }

    // WsHandler
    void messageStart(bool text) override;
    void messageData(const char* p, size_t n) override;
    void messageEnd() override;
    // JsonHandler
    void begin(const char* path, bool array) override;
    void value(const char* path, JsonType type, const char* s, size_t n, bool cut) override;
    void end(const char* path, bool array) override;
    // READY's guilds[]: each read as a GUILD_CREATE (out: its path as one, from "d"); false: not one
    bool readyGuild(const char* path, char* out, size_t cap) const;

private:
    enum Event : uint8_t {
        Ev_None, Ev_Other, Ev_Ready, Ev_GuildCreate, Ev_GuildUpdate, Ev_GuildDelete, Ev_ChannelSet, Ev_ChannelDelete,
        Ev_VoiceState, Ev_VoiceServer, Ev_StreamCreate, Ev_StreamServer, Ev_StreamDelete
    };
    struct Text {
        char s[CHIRP_NAME];
        size_t n;
        void set(JsonType type, const char* p, size_t len) {
            if (type != Json_String) len = 0;
            n = len < sizeof(s) ? len : sizeof(s);  // (utf8Copy cuts what's longer)
            for (size_t i = 0; i < n; i++) s[i] = p[i];
        }
    };
    // Who someone is: their server nickname, else their display name, else
    // their username; and their picture, the server's own of them first
    struct Names {
        Text nick, global, user, member_avatar, user_avatar;
        const Text* best() const { return nick.n ? &nick : global.n ? &global : user.n ? &user : nullptr; }
        void avatarFile(char out[CHIRP_AVATAR]) const;
    };
    // A channel, a voice state, a member while they're read (their fields come in any order)
    struct ChannelRead {
        uint64_t id, parent, guild;
        int type, position;
        Text name;
    };
    struct VoiceRead {
        uint64_t user, channel, guild;
        uint8_t flags;
        char session[64];
        Names names;
    };
    struct MemberRead {
        uint64_t user;
        Names names;
    };
    struct NameKept {
        uint64_t user;
        Text name;
        char avatar[CHIRP_AVATAR];
    };

    void fail(const char* why);
    void say(const char* fmt, ...);
    bool send(const char* json);
    void identify();
    void heartbeat();
    void dispatchEnd();
    bool channelValue(const char* rest, JsonType type, const char* s, size_t n);
    bool voiceValue(const char* rest, JsonType type, const char* s, size_t n);
    bool namesValue(const char* rest, JsonType type, const char* s, size_t n, Names* names);
    void commitChannel(int guild_slot);
    void commitVoice(int guild_slot);

    WebSocket* ws_ = nullptr;
    WebSocket::RandomFn rnd_ = nullptr;
    LogFn log_ = nullptr;
    char token_[128];
    Json json_;
    bool in_text_ = false, unreadable_ = false;

    // the session
    bool hello_ = false, ready_ = false, over_ = false, acked_ = true, first_ack_ = false;
    bool invisible_ = true;
    uint32_t interval_ms_ = 0;
    uint64_t next_beat_ms_ = 0, now_ms_ = 0;
    int64_t seq_ = -1;  // the last "s" (-1: none yet)
    char error_[96];
    uint32_t skipped_ = 0;

    // the bot's own
    uint64_t me_ = 0;
    char my_name_[CHIRP_NAME];
    char session_id_[64] = {0};  // (READY's: the bot's voice states with it are this session's call)
    uint64_t call_guild_ = 0, call_channel_ = 0, call_since_ms_ = 0;
    uint32_t others_joined_ = 0, others_left_ = 0;
    bool self_mute_ = false, self_deaf_ = false;
    char voice_session_[64] = {0}, voice_token_[96] = {0}, voice_endpoint_[128] = {0};
    char server_token_[96], server_endpoint_[128];  // (a VOICE_SERVER_UPDATE while it's read)
    uint64_t voice_server_guild_ = 0, server_guild_ = 0;
    uint32_t voice_server_serial_ = 0;
    // Go Live
    uint64_t stream_server_ = 0, stream_channel_ = 0;
    char stream_token_[96] = {0}, stream_endpoint_[128] = {0};
    uint32_t stream_serial_ = 0;
    char stream_key_[80];  // (the one being read: ours is "guild:<guild>:<channel>:<me>")
    char stream_active_[80] = {0};  // (ours, asked for and not ended by us: empty, none)
    uint32_t stream_ended_ = 0;
    uint64_t stream_rtc_ = 0, stream_rtc_channel_ = 0;
    uint64_t viewers_[GATEWAY_VIEWERS];  // (watching our stream now)
    size_t viewers_n_ = 0;
    uint32_t viewers_joined_ = 0, viewers_left_ = 0;
    uint64_t msg_viewers_[GATEWAY_VIEWERS];  // (the message being read's viewer_ids, if it has them)
    size_t msg_viewers_n_ = 0;
    bool msg_viewers_listed_ = false;
    void streamKey(uint64_t guild, uint64_t channel, char out[80]) const;
    void streamClear();

    // the message being read
    int op_ = -1;
    Event ev_ = Ev_None;
    bool d_begun_ = false, t_late_ = false;
    uint32_t hello_interval_ = 0;
    Names my_names_;
    uint32_t ready_guilds_ = 0;
    // GUILD_CREATE / GUILD_UPDATE / GUILD_DELETE
    int guild_slot_ = -1;
    uint64_t guild_id_ = 0;
    bool guild_unavailable_ = false;
    Text guild_name_, guild_icon_, my_avatar_;
    bool guild_icon_seen_ = false;
    void iconFile(char out[CHIRP_AVATAR]) const;
    NameKept names_[GATEWAY_NAMES];
    size_t names_n_ = 0;
    ChannelRead chan_;
    VoiceRead voice_;
    MemberRead member_;
};
