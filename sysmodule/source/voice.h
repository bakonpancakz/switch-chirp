// The voice connection (Discord's voice docs, v8): its own thread, its own
// websocket to the voice server Discord gave the bot, and a UDP socket for
// the sound. Hello, Identify, Ready, IP discovery, Select Protocol, Session
// Description; then every packet that comes is opened (rtp.h), then opened
// again with its speaker's DAVE key (dave.h), and handed to the mixer.
// What the mic records (mic.h) goes the other way: DAVE-encrypted with our own
// key, sealed in RTP, with Speaking (op 5) said when talk starts and stops.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace voice {

typedef void (*LogFn)(const char* line);
typedef void (*RandomFn)(void* out, size_t n);

enum State : uint8_t { Voice_Off = 0, Voice_Connecting = 1, Voice_Connected = 2, Voice_Failed = 3 };

void start(LogFn log, RandomFn rnd);  // (once the sockets and ssl are up)
void stop();

// Be in this call: the main gateway's VOICE_STATE_UPDATE (session) and VOICE_SERVER_UPDATE (token,
// endpoint) for the bot. Called again with something different: the connection starts over.
void connect(uint64_t guild, uint64_t channel, uint64_t me, const char* session, const char* token, const char* endpoint);
void disconnect();
void retry();  // (Join pressed again: a connection that was given up on is tried again)
State state();

// Go Live (Share Screen): the stream's own connection, to the voice server STREAM_SERVER_UPDATE named
// (server: STREAM_CREATE's rtc_server_id; session: the call's voice session). It sends the screen
// and the game's sound (screen.h); nothing comes back on it to hear.
void streamConnect(uint64_t server, uint64_t channel, uint64_t me, const char* session, const char* token, const char* endpoint);
void streamDisconnect();
State streamState();

// For the overlay's Debug: each connection's voice server (host:port; empty: none) and heartbeat round trip (ms; 0: none yet)
void info(char voice_server[64], uint16_t* voice_ping, char stream_server[64], uint16_t* stream_ping);

}  // namespace voice
