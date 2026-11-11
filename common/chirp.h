// What the Chirp sysmodule and its overlay say to each other over IPC
// (service "chirp", plain CMIF requests), and where things are on the card.
// Raw data is small and fixed size; lists travel in mapped buffers
// (HipcMapAlias) as arrays of the structs below.
#pragma once
#include <stdint.h>

#define CHIRP_SERVICE "chirp"
#define CHIRP_TITLE_ID 0x420000000000C4A7ULL
#define CHIRP_DIR_SHOWN "/config/sys-chirp"  // (as the overlay names it)
#define CHIRP_DIR "sdmc:" CHIRP_DIR_SHOWN
#define CHIRP_TOKEN_TXT CHIRP_DIR "/token.txt"
#define CHIRP_TOKEN_INI CHIRP_DIR "/token.ini"        // Name=token, one bot a line: the overlay's Login As (token.txt: just one)
#define CHIRP_SETTINGS_INI CHIRP_DIR "/settings.ini"  // which login, the mic's settings
#define CHIRP_LOG CHIRP_DIR "/chirp.log"
#define CHIRP_PEOPLE_INI CHIRP_DIR "/people.ini"  // our mute and volume for people (store.h)
#define CHIRP_LOADING CHIRP_DIR "/loading"  // *.h264: keyframes shown while sharing and the game shows nothing (scripts/loading.ps1)
#define CHIRP_AVATARS CHIRP_DIR "/avatars"
#define CHIRP_SOUNDS CHIRP_DIR "/sounds"  // voice_join.wav, voice_leave.wav, screenshare_join.wav, screenshare_leave.wav, stream_start.wav, stream_stop.wav, mute.wav, unmute.wav, deafen.wav, undeafen.wav (sounds.h)  // {ChirpMember.avatar}.png, fetched by the sysmodule

// A name as it's kept and shown: UTF-8, cut (with "…") to fit, NUL-terminated
#define CHIRP_NAME 48

// The most the overlay asks for at once
#define CHIRP_LIST_MAX 128

enum ChirpCmd {
    ChirpCmd_GetVersion = 0,     // out u32 CHIRP_PROTOCOL
    ChirpCmd_GetStatus = 1,      // out buffer ChirpStatus (a buffer: it's more than an IPC message holds)
    ChirpCmd_ListGuilds = 2,     // in u32 first; out buffer ChirpGuild[]; out ChirpListed
    ChirpCmd_ListChannels = 3,   // in u64 guild id; out buffer ChirpChannel[] (display order); out ChirpListed
    ChirpCmd_ListMembers = 4,    // in u64 channel id; out buffer ChirpMember[]; out ChirpListed
    ChirpCmd_Join = 5,           // in ChirpJoin
    ChirpCmd_Leave = 6,          //
    ChirpCmd_SetSelfMute = 7,    // in u32 0/1
    ChirpCmd_SetSelfDeaf = 8,    // in u32 0/1
    ChirpCmd_SetUserMute = 9,    // in ChirpUserSetting (value 0/1): only for us, nobody else hears it
    ChirpCmd_SetUserVolume = 10, // in ChirpUserSetting (value 0..200, percent)
    ChirpCmd_SoundCheck = 11,    // in u32 0/1: test tones through the mixer
    ChirpCmd_Reload = 12,        // read token.txt again and start a new session
    ChirpCmd_SetVolume = 13,     // in u32 0..200 (percent): everything Chirp plays
    ChirpCmd_SetScreenShare = 14, // in u32 0/1: the game's video into the call (when Discord takes a bot's)
    ChirpCmd_SetRichPresence = 15, // in u32 0/1: "Playing <the game>" on the bot (kept in settings.ini)
    ChirpCmd_ListLogins = 16,     // out buffer ChirpLogin[] (token.ini's names); out ChirpListed
    ChirpCmd_SetLogin = 17,       // in ChirpLogin: connect as that bot from now on
    ChirpCmd_SetMic = 18,         // in ChirpMic: input volume, sensitivity, automatic gain, noise suppression, HOME light (kept in settings.ini)
    ChirpCmd_GetDebug = 19,       // out buffer ChirpDebug
    ChirpCmd_SetFullRange = 20,   // in u32 0/1: label the screen's video full range, whatever grc says (kept in settings.ini)
    ChirpCmd_SetInvisible = 21,   // in u32 0/1: the bot appears offline while connected (kept in settings.ini)
};

// Raised when the commands change. The overlay says so when the sysmodule
// running is older (it's only loaded again when the Switch restarts).
#define CHIRP_PROTOCOL 12

enum ChirpConn {
    ChirpConn_NoToken = 0,     // no token.txt (or it's empty)
    ChirpConn_Offline = 1,     // no network, or it went away: trying again in retry_in_s
    ChirpConn_Connecting = 2,  // TLS, the websocket, Hello, Identify, waiting for READY
    ChirpConn_Online = 3,      // READY came: the lists are filled in
    ChirpConn_BadToken = 4,    // Discord said no (close code 4004): not tried again until Reload
    ChirpConn_Refused = 5,     // another close code that trying again won't fix (last_close says which)
};

enum ChirpMemberFlag {
    ChirpMember_SelfMute = 1 << 0,
    ChirpMember_SelfDeaf = 1 << 1,
    ChirpMember_ServerMute = 1 << 2,
    ChirpMember_ServerDeaf = 1 << 3,
    ChirpMember_LocalMute = 1 << 4,  // we muted them for ourselves
    ChirpMember_Me = 1 << 5,
    ChirpMember_NoName = 1 << 6,     // the name is "Unknown (id)" until a voice state brings theirs
    ChirpMember_Speaking = 1 << 7,   // (from the voice connection, when there is one)
};

// An avatar's file name: its hash, with "g" in front for a server's own
// picture of someone, "i" for a server's icon (empty: none, draw the default)
#define CHIRP_AVATAR 40

typedef struct ChirpStatus {
    uint8_t conn;        // ChirpConn
    uint8_t in_call;     // 0/1: our voice state is in call_channel
    uint8_t self_mute;   // 0/1
    uint8_t self_deaf;   // 0/1
    uint8_t sound_check; // 0/1
    uint8_t pad[1];
    uint16_t last_close; // the websocket's last close code (0: none)
    uint32_t serial;     // goes up whenever a list changes: ask again
    uint32_t retry_in_s; // ChirpConn_Offline: seconds to the next try
    uint32_t call_seconds;  // how long we've been in call_channel
    uint32_t guilds;     // how many ListGuilds has
    uint64_t call_guild, call_channel;
    char user[CHIRP_NAME];          // who we're signed in as
    char call_guild_name[CHIRP_NAME];
    char call_channel_name[CHIRP_NAME];
    uint16_t volume;     // everything Chirp plays: 0..200 (percent)
    uint8_t screen_share;  // wanted (the toggle)
    uint8_t screen_live;   // and capturing, into a call that has a video stream
    uint8_t pad2[4];
    char avatar[CHIRP_AVATAR];  // the bot's picture (a file in CHIRP_AVATARS once fetched; empty: none)
    char login[CHIRP_NAME];     // which of token.ini's bots (empty: token.txt's)
    uint16_t mic_volume;        // input volume: 0..200 (percent)
    int8_t mic_sensitivity;     // input sensitivity: 0 automatic, else the level talk must be over (dBFS, < 0)
    uint8_t mic_agc;            // automatic gain: 0/1
    uint8_t mic_denoise;        // noise suppression: 0/1
    uint8_t full_range;         // the screen's video labelled full range: 0/1
    uint8_t invisible;          // the bot appears offline: 0/1
    uint8_t rich_presence;      // "Playing <the game>": 0/1
    uint8_t mic_home_light;     // the HOME button's light while talking: 0 off, else 10..100 (percent)
    uint8_t pad3[2];
    char game[CHIRP_NAME];      // what Rich Presence says it's playing (empty: nothing)
} ChirpStatus;

typedef struct ChirpMic {
    uint16_t volume;     // 0..200 (percent)
    int8_t sensitivity;  // 0: automatic; else dBFS (< 0)
    uint8_t agc;         // 0/1
    uint8_t denoise;     // 0/1: noise suppression (RNNoise)
    uint8_t home_light;  // the HOME button's light while talking: 0 off, else 10..100 (percent)
    uint8_t pad[2];
} ChirpMic;

// How things are underneath, for the overlay's Debug page
typedef struct ChirpDebug {
    char voice_server[64];   // the call's voice server, host:port (empty: none)
    char stream_server[64];  // Go Live's (empty: not sharing)
    uint16_t voice_ping;     // ms, the voice connection's heartbeat there and back (0: none yet)
    uint16_t stream_ping;
    uint8_t voice_state;     // 0 off, 1 connecting, 2 connected, 3 given up on
    uint8_t stream_state;
    uint8_t mic_open;        // 0/1
    uint8_t decoders;        // hardware Opus decoders lent to speakers now
    int16_t mic_level;       // dBFS, as the sensitivity sees it
    int16_t mic_threshold;   // dBFS, the line talk must be over now
    int16_t mic_gain;        // dB, the automatic gain
    uint8_t speakers;        // people the mixer's following now
    uint8_t decoders_max, speakers_max;
    uint8_t pad[3];
    uint32_t call_seconds;
    uint32_t system_free_kb;  // the memory every sysmodule shares, left
    uint32_t chirp_kb;        // Chirp's own
    uint32_t heap_used_kb, heap_kb;
    // (protocol 12)
    uint16_t gateway_ping;   // ms, Discord's gateway: its heartbeat there and back (0: none yet)
    uint8_t core3_idle;      // percent of the last second core 3 (the system's, and Chirp's) sat idle
    uint8_t record_fps;      // the frame rate the Switch records at, as its frames' timestamps say (0: none yet)
    uint16_t chirp_cpu;      // Chirp's own share of core 3 over that second, per mille
    uint16_t stream_frame_max_kb;  // the biggest video frame since the stream began
    // the stream since it began (the overlay takes rates from two readings): frames and bytes of video
    // sent, frames sent over 25 ms after the one before, packets viewers reported lost
    uint32_t stream_frames, stream_kb, stream_late, stream_lost;
    int8_t color_range;      // the range the video goes out labelled with: 0 limited, 1 full, -1 not sharing
    uint8_t pad3[3];
} ChirpDebug;

typedef struct ChirpLogin {
    char name[CHIRP_NAME];
} ChirpLogin;
#define CHIRP_LOGINS 8

typedef struct ChirpListed {
    uint32_t written;  // into the buffer
    uint32_t total;    // there are (from `first`, for guilds)
} ChirpListed;

typedef struct ChirpGuild {
    uint64_t id;
    uint16_t voice_channels;
    uint16_t in_voice;  // people in its voice channels
    uint8_t pad[4];
    char name[CHIRP_NAME];
    char icon[CHIRP_AVATAR];  // its icon: a file in CHIRP_AVATARS once fetched (empty: none)
} ChirpGuild;

enum ChirpChannelType {
    ChirpChannel_Voice = 2,
    ChirpChannel_Category = 4,  // a heading: the voice channels under it follow
};

typedef struct ChirpChannel {
    uint64_t id;
    uint16_t members;  // people in it now
    uint8_t type;      // ChirpChannelType
    uint8_t pad[5];
    char name[CHIRP_NAME];
} ChirpChannel;

#define CHIRP_SILENT 255  // (ChirpMember.level: nothing coming in)

typedef struct ChirpMember {
    uint64_t user_id;
    uint8_t flags;     // ChirpMemberFlag
    uint8_t pad;
    uint16_t volume;   // percent, 0..200
    uint8_t level;     // how loud they are coming in: -level dBFS (CHIRP_SILENT: nothing)
    uint8_t pad2[3];
    char name[CHIRP_NAME];
    char avatar[CHIRP_AVATAR];  // a file in CHIRP_AVATARS once it's fetched (+ ".png")
} ChirpMember;

typedef struct ChirpJoin {
    uint64_t guild, channel;
    uint32_t muted;  // 0/1: join with self_mute on
    uint32_t pad;
} ChirpJoin;

typedef struct ChirpUserSetting {
    uint64_t user_id;
    uint32_t value;
    uint32_t pad;
} ChirpUserSetting;
