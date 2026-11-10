// What we know about Discord: guilds, their voice channels (and the
// categories over them), and who's in voice. Fixed tables, nothing
// allocated; full tables drop what doesn't fit (and count it, for the log).
// Not locked: app.cpp holds its lock around every call.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "chirp.h"

#define STORE_GUILDS 200     // (Discord's most, with Nitro)
#define STORE_CHANNELS 512   // voice channels and categories, all guilds together
#define STORE_MEMBERS 256    // people in voice, all guilds together
#define STORE_PREFS 128      // people we've set a volume or local mute for

namespace store {

struct Guild {
    uint64_t id;
    bool used, pending;  // pending: being read (READY, GUILD_CREATE), not shown yet
    char name[CHIRP_NAME];
    char icon[CHIRP_AVATAR];  // ("i" and its hash: a file for avatars; empty: none)
};

struct Channel {
    uint64_t id, parent;  // parent: its category (0: none)
    uint16_t guild;       // slot in the guild table
    int16_t position;
    uint8_t type;         // ChirpChannelType
    bool used;
    char name[CHIRP_NAME];
};

struct Member {
    uint64_t user, channel;
    uint16_t guild;
    uint8_t flags;  // ChirpMemberFlag (SelfMute..ServerDeaf, NoName)
    bool used;
    char name[CHIRP_NAME];
    char avatar[CHIRP_AVATAR];  // its file name in CHIRP_AVATARS (empty: none)
};

struct Dropped {
    uint32_t guilds, channels, members;
};

void clear();
uint32_t serial();  // goes up with every change
const Dropped& dropped();

// A guild being read: a slot for its channels and voice states to point at
// before its id is known (it may come after them). -1: the table is full.
int guildBegin();
// The guild read into `slot` is `id`: it replaces what was known of it
void guildCommit(int slot, uint64_t id, const char* name, size_t n, const char* icon = nullptr);
void guildAbandon(int slot);  // (not a guild after all: unavailable, or no id)
int guildFind(uint64_t id);
void guildRename(uint64_t id, const char* name, size_t n, const char* icon = nullptr);  // (icon null: unchanged)
void guildRemove(uint64_t id);
const Guild* guildAt(int slot);

// Only voice channels and categories are kept: another type removes it
void channelSet(int guild_slot, uint64_t id, uint64_t parent, int position, int type, const char* name, size_t n);
void channelRemove(uint64_t id);
const Channel* channelFind(uint64_t id);

// channel 0: they left voice (in this guild). name null: keep the name and
// avatar known (a voice state without its member). avatar: a file name, "" for none.
void memberSet(int guild_slot, uint64_t user, uint64_t channel, uint8_t flags, const char* name, size_t n, const char* avatar);
// A name and avatar for someone in voice who has none yet (GUILD_CREATE's members)
void memberName(int guild_slot, uint64_t user, const char* name, size_t n, const char* avatar);

// Ours, for the mixer and the lists: percent 0..200 (100 when never set)
// (STORE_EVERYONE: the master volume, "id=everyone" in people.ini; no Discord id is that)
#define STORE_EVERYONE UINT64_MAX
void prefSetVolume(uint64_t user, int volume);
void prefSetMute(uint64_t user, bool mute);
int prefVolume(uint64_t user);
bool prefMute(uint64_t user);
// people.ini: a line a person, "id=<their id>;volume=<0..200>;muted=<true|false>;"
// (read in any order; a missing volume is 100). Write: the bytes written (cut short if cap is small).
size_t prefsWrite(char* out, size_t cap);
void prefsRead(const char* text, size_t n);  // (lines it can't read are skipped)

// For the overlay, in what it shows
size_t guildCount();
size_t listGuilds(size_t first, ChirpGuild* out, size_t cap);
// A guild's voice channels in Discord's order: those without a category
// first, then each category (that has voice channels) and the ones under it
size_t listChannels(uint64_t guild, ChirpChannel* out, size_t cap);
size_t listMembers(uint64_t channel, uint64_t me, ChirpMember* out, size_t cap, size_t* total);
size_t membersIn(uint64_t channel);

}  // namespace store
