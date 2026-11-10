// The network thread: one gateway session after another (a new one from
// scratch whenever the last ended), and what the IPC thread asks of it.
// Commands are queued and run by the network thread, so only it ever talks
// to the TLS connection.
#pragma once
#include <stddef.h>

#include "chirp.h"

namespace app {

void start();
void stop();

void status(ChirpStatus* s);
size_t listGuilds(size_t first, ChirpGuild* out, size_t cap, size_t* total);
size_t listChannels(uint64_t guild, ChirpChannel* out, size_t cap);
size_t listMembers(uint64_t channel, ChirpMember* out, size_t cap, size_t* total);

void join(const ChirpJoin& j);
void leave();
void setSelfMute(bool on);
void setSelfDeaf(bool on);
void setUserMute(uint64_t user, bool on);
void setUserVolume(uint64_t user, int volume);
void setScreenShare(bool on);   // (not kept: off at every start)
void setMic(int volume, int sensitivity, bool agc, bool denoise);  // (the mic's settings: kept in settings.ini)
void debug(ChirpDebug* d);      // (the overlay's Debug page)
void setFullRange(bool on);     // (the screen's video labelled full range: kept in settings.ini)
void setInvisible(bool on);     // (the bot offline to everyone while connected: kept in settings.ini)
void setRichPresence(bool on);  // ("Playing <game>": kept in settings.ini)
size_t listLogins(ChirpLogin* out, size_t cap);  // token.ini's names
void setLogin(const char* name);  // connect as that one (kept in settings.ini)
void setVolume(int volume);     // (everything Chirp plays: 0..200 percent, kept in people.ini)
void soundCheck(bool on);
void reload();

void log(const char* line);  // into CHIRP_LOG, with the time

}  // namespace app
