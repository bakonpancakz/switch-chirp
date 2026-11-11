// Everything the overlay says, in one place to reword. "%u" is a number and
// "%s" a name, filled in when shown; keep those (and the order they come in).
#pragma once

#include "chirp.h"  // (CHIRP_DIR_SHOWN)

// The Switch's button icons (in its system font), to put in a string
#define BTN_A "\uE0E0"
#define BTN_B "\uE0E1"
#define BTN_X "\uE0E2"
#define BTN_Y "\uE0E3"

namespace txt {

// --- the main menu ---
inline constexpr const char* TITLE = "Chirp";
inline constexpr const char* RETRY = "Retry";  // (A on the status: a new session now)
inline constexpr const char* RPC_IDLE = "(no game open)";  // (Home's Rich Presence row: on, no game running)
inline constexpr const char* RPC_OFF = "(disabled)";
inline constexpr const char* ACTIVE_N = "%u active";  // (a server's or a channel's people in voice)
inline constexpr const char* SERVERS = "Servers";
inline constexpr const char* NO_SERVERS = "The bot isn't in any server yet";
inline constexpr const char* CARET = "\uE14A";  // (on what opens a page: Ultrahand's own, its DROPDOWN_SYMBOL)

// --- Select Login (A on the bot at the top) ---
inline constexpr const char* SELECT_LOGIN = "Select Login";
inline constexpr const char* LOGINS_HINT = "Hint! Tokens are stored in 'sdmc:" CHIRP_DIR_SHOWN "/token.ini' !";
inline constexpr const char* LOGINS_NONE = "No bots in " CHIRP_DIR_SHOWN "/token.ini";
inline constexpr const char* LOGINS_HOW = "One a line: Name=token";
inline constexpr const char* ACTIVE = "Active";
inline constexpr const char* STATUS = "Status";
inline constexpr const char* INVISIBLE = "Appear Invisible";  // (offline to everyone, still connected)
inline constexpr const char* RICH_PRESENCE = "Rich Presence";  // ("Playing <the game>"; not shown while invisible)
inline constexpr const char* LOGINS = "Logins";
inline constexpr const char* ST_INVISIBLE = "Connected, invisible";
inline constexpr const char* ST_PLAYING = "Playing %s";

// the status line at the top (what the sysmodule says)
inline constexpr const char* ST_ONLINE = "Connected";
inline constexpr const char* ST_ONLINE_SUB = "as %s";
inline constexpr const char* ST_CONNECTING = "Connecting...";
inline constexpr const char* ST_OFFLINE = "Offline";
inline constexpr const char* ST_OFFLINE_SUB = "Disconnected, retrying in %u s";
inline constexpr const char* ST_NO_TOKEN = "No bot token";
inline constexpr const char* ST_NO_TOKEN_SUB = "Put it in " CHIRP_DIR_SHOWN "/token.ini";
inline constexpr const char* ST_BAD_TOKEN = "Discord turned the token down";
inline constexpr const char* ST_BAD_TOKEN_SUB = "Check token.txt, then Connect Again";
inline constexpr const char* ST_REFUSED = "Discord closed the session (%u)";
inline constexpr const char* ST_REFUSED_SUB = "See " CHIRP_DIR_SHOWN "/chirp.log";
inline constexpr const char* ST_NOT_RUNNING = "Chirp isn't running";
inline constexpr const char* ST_NOT_RUNNING_SUB = "Restart the Switch after installing it";
inline constexpr const char* ST_OUTDATED = "Restart the Switch to update Chirp";

// --- a server ---
inline constexpr const char* VOICE_CHANNELS = "Voice channels";
inline constexpr const char* NO_VOICE_CHANNELS = "No voice channels the bot can see";

// --- a voice channel ---
inline constexpr const char* JOIN = "Join Channel";
inline constexpr const char* JOIN_AS_MUTED = "Muted";  // (beside Join: the bot was muted last, and joins so)
inline constexpr const char* JOINING = "Joining...";
inline constexpr const char* ACTIVE_MEMBERS = "Active members (%u)";
inline constexpr const char* NOBODY = "Nobody's here";

// what's only ours about someone, beside their name (mute and deafen are icons)
inline constexpr const char* YOU = "You!";

// --- in a call ---
inline constexpr const char* MUTE = "Mute";
inline constexpr const char* MUTE_X = BTN_X " Mute";      // (X mutes from anywhere on the page)
inline constexpr const char* DEAFEN_Y = BTN_Y " Deafen";  // (and Y deafens)
inline constexpr const char* CONNECTED_FOR = "Connected for %s";  // (the call page's subtitle)
inline constexpr const char* DEAFEN = "Deafen";
inline constexpr const char* CALL_TITLE = "Chirp: %s";  // (the call page's title: the channel)
inline constexpr const char* SHARE_SCREEN = "Screenshare";
inline constexpr const char* SETTINGS = "Settings";
inline constexpr const char* LEAVE = "Disconnect";
inline constexpr const char* CONTROLS = "Controls";

// --- someone in the call ---
inline constexpr const char* MANAGE = "Manage: %s";  // (only you hear these; Chirp remembers them)
inline constexpr const char* VOLUME = "Volume";
inline constexpr const char* DEBUG = "Debug";
inline constexpr const char* DEBUG_ID = "User ID";
inline constexpr const char* DEBUG_SPEAKING = "Speaking";
inline constexpr const char* DEBUG_LEVEL = "Volume";  // how loud they are coming in
inline constexpr const char* YES = "True";
inline constexpr const char* NO = "False";

// --- Settings (from the call page) ---
inline constexpr const char* INPUT = "Microphone";
inline constexpr const char* INPUT_VOLUME = "Input Volume";
inline constexpr const char* INPUT_SENSITIVITY = "Input Sensitivity";
inline constexpr const char* AUTOMATIC = "Automatic";
inline constexpr const char* MIC_LEVEL = "Input Level";     // (now, and the line talk must be over)
inline constexpr const char* MIC_LEVEL_VALUE = "%d dB / %d dB";
inline constexpr const char* MIC_CLOSED = "Closed (muted)";
inline constexpr const char* AGC = "Automatic Gain Control";
inline constexpr const char* DENOISE = "Noise Suppression";
inline constexpr const char* HOME_LIGHT = "HOME Input Indicator";  // (lit while you talk; the slider is its brightness)
inline constexpr const char* LIGHT_OFF = "Off";
inline constexpr const char* SCREENSHARE_SECTION = "Screenshare";
inline constexpr const char* FULL_RANGE = "Full Range Color";  // (for when blacks look crushed: grc's own label says limited)

// --- Debug ---
inline constexpr const char* DBG_SCREENSHARE = "Debug: Screenshare";
inline constexpr const char* DBG_RANGE = "Color Range";         // (what the video goes out labelled with)
inline constexpr const char* DBG_FULL = "Full";
inline constexpr const char* DBG_LIMITED = "Limited";
inline constexpr const char* DBG_REC_FPS = "Framerate";         // (the Switch records at: 30, or 60 with the tweaks)
inline constexpr const char* DBG_FPS = "Frames Sent";           // (a second, now)
inline constexpr const char* DBG_BITRATE = "Bitrate";
inline constexpr const char* DBG_LATE = "Late Frames";          // (over a frame and a half after the one before)
inline constexpr const char* DBG_LOST = "Packets Lost";         // (as viewers reported)
inline constexpr const char* DBG_BIGGEST = "Largest Frame";
inline constexpr const char* DBG_NETWORK = "Debug: Network";
inline constexpr const char* DBG_PING_GATEWAY = "Ping: Gateway";
inline constexpr const char* DBG_PING_VOICE = "Ping: Voice";
inline constexpr const char* DBG_PING_STREAM = "Ping: Stream";
inline constexpr const char* DBG_HOST_VOICE = "Host: Voice";
inline constexpr const char* DBG_HOST_STREAM = "Host: Stream";
inline constexpr const char* DBG_NONE = "None";
inline constexpr const char* DBG_NA = "N/A";
inline constexpr const char* DBG_STATES[4] = {"Off", "Connecting", "Connected", "Gave up"};
inline constexpr const char* DBG_AUDIO = "Debug: Audio";
inline constexpr const char* DBG_SPEAKERS = "Speakers";   // (followed now, of how many)
inline constexpr const char* DBG_DECODERS = "Decoders";   // (in use, of how many)
inline constexpr const char* DBG_OF = "%d of %d";
inline constexpr const char* DBG_MIC_GAIN = "Input Gain";
inline constexpr const char* DBG_SYSTEM = "Debug: System";
inline constexpr const char* DBG_SYSTEM_FREE = "Memory Free";  // (what every sysmodule shares, left)
inline constexpr const char* DBG_CHIRP = "Chirp Usage";
inline constexpr const char* DBG_HEAP = "Chirp Heap";
inline constexpr const char* DBG_CORE3_IDLE = "System Idle";   // (core 3: the system's and the sysmodules')
inline constexpr const char* DBG_CHIRP_CPU = "Chirp CPU";

}  // namespace txt
