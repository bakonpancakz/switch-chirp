// The overlay's side of the "chirp" service (common/chirp.h)
#pragma once
#include <switch.h>

#include <vector>

#include "chirp.h"

namespace client {

// Connect if the sysmodule is running (never waits for it): false if it isn't
bool connect();
void close();
bool connected();
bool outdated();  // the sysmodule running is older than this overlay (restart the Switch)

bool status(ChirpStatus* s);
std::vector<ChirpGuild> guilds();
std::vector<ChirpChannel> channels(u64 guild);
std::vector<ChirpMember> members(u64 channel);

void join(u64 guild, u64 channel, bool muted);
void leave();
void setSelfMute(bool on);
void setSelfDeaf(bool on);
void setScreenShare(bool on);  // the game's video into the call
void setFullRange(bool on);    // its video labelled full range
void setInvisible(bool on);    // the bot appears offline while connected
void setRichPresence(bool on); // "Playing <the game>" on the bot
std::vector<ChirpLogin> logins();  // token.ini's bots
void setLogin(const char* name);   // connect as that one
void setUserMute(u64 user, bool on);
void setUserVolume(u64 user, int volume);
void soundCheck(bool on);
void setVolume(int volume);  // everything Chirp plays: 0..200 (percent)
// The mic: input volume (0..200 percent), sensitivity (0 automatic, else dBFS), automatic gain
void setMic(int volume, int sensitivity, bool agc, bool denoise);
bool debug(ChirpDebug* d);  // how things are underneath, for the Debug page
void reload();

}  // namespace client
