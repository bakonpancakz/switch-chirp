#include "store.h"

#include <stdio.h>
#include <string.h>

#include "util.h"

namespace store {
namespace {

struct Pref {
    uint64_t user;
    uint8_t volume;
    bool mute, used;
};

Guild g_guilds[STORE_GUILDS];
Channel g_channels[STORE_CHANNELS];
Member g_members[STORE_MEMBERS];
Pref g_prefs[STORE_PREFS];
uint32_t g_serial = 1;
Dropped g_dropped;

void changed() { g_serial++; }

// Everything pointing at a guild slot goes with it
void freeGuild(int slot) {
    for (Channel& c : g_channels)
        if (c.used && c.guild == slot) c.used = false;
    for (Member& m : g_members)
        if (m.used && m.guild == slot) m.used = false;
    g_guilds[slot].used = false;
    g_guilds[slot].pending = false;
}

bool visible(const Guild& g) { return g.used && !g.pending; }

Pref* prefFind(uint64_t user) {
    for (Pref& p : g_prefs)
        if (p.used && p.user == user) return &p;
    return nullptr;
}

// A pref for user, made when there's none; null when the table is full
Pref* prefGet(uint64_t user) {
    if (Pref* p = prefFind(user)) return p;
    for (Pref& p : g_prefs) {
        if (!p.used) {
            p = Pref{user, 100, false, true};
            return &p;
        }
    }
    return nullptr;
}

void prefTidy(Pref* p) {
    if (p->volume == 100 && !p->mute) p->used = false;
}

// Before b, in Discord's order: by position, then by id (older first)
bool before(const Channel& a, const Channel& b) { return a.position != b.position ? a.position < b.position : a.id < b.id; }

void sortChannels(uint16_t* idx, size_t n) {
    for (size_t i = 1; i < n; i++) {
        uint16_t v = idx[i];
        size_t j = i;
        while (j > 0 && before(g_channels[v], g_channels[idx[j - 1]])) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = v;
    }
}

void toChirp(const Channel& c, ChirpChannel* out) {
    memset(out, 0, sizeof(*out));
    out->id = c.id;
    out->type = c.type;
    out->members = (uint16_t)(c.type == ChirpChannel_Voice ? membersIn(c.id) : 0);
    memcpy(out->name, c.name, sizeof(out->name));
}

}  // namespace

void clear() {
    memset(g_guilds, 0, sizeof(g_guilds));
    memset(g_channels, 0, sizeof(g_channels));
    memset(g_members, 0, sizeof(g_members));
    g_dropped = Dropped{};
    changed();
}

uint32_t serial() { return g_serial; }
const Dropped& dropped() { return g_dropped; }

int guildBegin() {
    for (int i = 0; i < STORE_GUILDS; i++) {
        if (!g_guilds[i].used) {
            g_guilds[i] = Guild{};
            g_guilds[i].used = g_guilds[i].pending = true;
            return i;
        }
    }
    g_dropped.guilds++;
    return -1;
}

void guildCommit(int slot, uint64_t id, const char* name, size_t n, const char* icon) {
    if (slot < 0 || slot >= STORE_GUILDS || !g_guilds[slot].pending) return;
    for (int i = 0; i < STORE_GUILDS; i++)
        if (i != slot && visible(g_guilds[i]) && g_guilds[i].id == id) freeGuild(i);
    Guild& g = g_guilds[slot];
    g.id = id;
    g.pending = false;
    utf8Copy(g.name, sizeof(g.name), name, n);
    snprintf(g.icon, sizeof(g.icon), "%s", icon ? icon : "");
    changed();
}

void guildAbandon(int slot) {
    if (slot >= 0 && slot < STORE_GUILDS && g_guilds[slot].pending) freeGuild(slot);
}

int guildFind(uint64_t id) {
    for (int i = 0; i < STORE_GUILDS; i++)
        if (visible(g_guilds[i]) && g_guilds[i].id == id) return i;
    return -1;
}

void guildRename(uint64_t id, const char* name, size_t n, const char* icon) {
    int i = guildFind(id);
    if (i < 0) return;
    if (n) utf8Copy(g_guilds[i].name, sizeof(g_guilds[i].name), name, n);
    if (icon) snprintf(g_guilds[i].icon, sizeof(g_guilds[i].icon), "%s", icon);
    changed();
}

void guildRemove(uint64_t id) {
    int i = guildFind(id);
    if (i < 0) return;
    freeGuild(i);
    changed();
}

const Guild* guildAt(int slot) { return slot >= 0 && slot < STORE_GUILDS && g_guilds[slot].used ? &g_guilds[slot] : nullptr; }

void channelSet(int guild_slot, uint64_t id, uint64_t parent, int position, int type, const char* name, size_t n) {
    if (guild_slot < 0 || guild_slot >= STORE_GUILDS || !g_guilds[guild_slot].used || id == 0) return;
    if (type != ChirpChannel_Voice && type != ChirpChannel_Category) {
        channelRemove(id);  // (it may have been a voice channel before)
        return;
    }
    Channel* c = nullptr;
    for (Channel& x : g_channels)
        if (x.used && x.id == id) c = &x;
    if (!c) {
        for (Channel& x : g_channels) {
            if (!x.used) {
                c = &x;
                break;
            }
        }
    }
    if (!c) {
        g_dropped.channels++;
        return;
    }
    c->used = true;
    c->id = id;
    c->parent = parent;
    c->guild = (uint16_t)guild_slot;
    c->position = (int16_t)(position < -32768 ? -32768 : position > 32767 ? 32767 : position);
    c->type = (uint8_t)type;
    utf8Copy(c->name, sizeof(c->name), name, n);
    changed();
}

void channelRemove(uint64_t id) {
    for (Channel& c : g_channels) {
        if (c.used && c.id == id) {
            c.used = false;
            changed();
        }
    }
}

const Channel* channelFind(uint64_t id) {
    for (const Channel& c : g_channels)
        if (c.used && c.id == id) return &c;
    return nullptr;
}

void memberSet(int guild_slot, uint64_t user, uint64_t channel, uint8_t flags, const char* name, size_t n, const char* avatar) {
    if (guild_slot < 0 || guild_slot >= STORE_GUILDS || !g_guilds[guild_slot].used || user == 0) return;
    Member* m = nullptr;
    for (Member& x : g_members)
        if (x.used && x.guild == guild_slot && x.user == user) m = &x;
    if (channel == 0) {
        if (m) {
            m->used = false;
            changed();
        }
        return;
    }
    if (!m) {
        for (Member& x : g_members) {
            if (!x.used) {
                m = &x;
                *m = Member{};
                m->flags = ChirpMember_NoName;
                break;
            }
        }
    }
    if (!m) {
        g_dropped.members++;
        return;
    }
    m->used = true;
    m->user = user;
    m->guild = (uint16_t)guild_slot;
    m->channel = channel;
    uint8_t keep = m->flags & ChirpMember_NoName;
    m->flags = (uint8_t)((flags & ~ChirpMember_NoName) | keep);
    if (name && n > 0) {
        utf8Copy(m->name, sizeof(m->name), name, n);
        m->flags &= (uint8_t)~ChirpMember_NoName;
        snprintf(m->avatar, sizeof(m->avatar), "%s", avatar ? avatar : "");
    }
    changed();
}

void memberName(int guild_slot, uint64_t user, const char* name, size_t n, const char* avatar) {
    if (n == 0) return;
    for (Member& m : g_members) {
        if (m.used && m.guild == guild_slot && m.user == user && (m.flags & ChirpMember_NoName)) {
            utf8Copy(m.name, sizeof(m.name), name, n);
            m.flags &= (uint8_t)~ChirpMember_NoName;
            snprintf(m.avatar, sizeof(m.avatar), "%s", avatar ? avatar : "");
            changed();
        }
    }
}

void prefSetVolume(uint64_t user, int volume) {
    Pref* p = prefGet(user);
    if (!p) return;
    p->volume = (uint8_t)(volume < 0 ? 0 : volume > 200 ? 200 : volume);
    prefTidy(p);
    if (user != STORE_EVERYONE) changed();  // (the master volume is in no list: the overlay's pages needn't be made again)
}

void prefSetMute(uint64_t user, bool mute) {
    Pref* p = prefGet(user);
    if (!p) return;
    p->mute = mute;
    prefTidy(p);
    changed();
}

int prefVolume(uint64_t user) {
    const Pref* p = prefFind(user);
    return p ? p->volume : 100;
}

bool prefMute(uint64_t user) {
    const Pref* p = prefFind(user);
    return p && p->mute;
}

size_t prefsWrite(char* out, size_t cap) {
    size_t at = 0;
    auto put = [&](const char* fmt, auto... args) {
        if (at < cap) {
            int n = snprintf(out + at, cap - at, fmt, args...);
            if (n > 0) at = at + (size_t)n < cap ? at + (size_t)n : cap;
        }
    };
    put("# Chirp: your mute and volume (0-200%%) for people in calls, by their Discord id. Only you hear these.\n");
    put("# id=everyone is the volume of everything Chirp plays.\n");
    for (const Pref& p : g_prefs) {
        if (!p.used) continue;
        char id[21];
        if (p.user == STORE_EVERYONE) snprintf(id, sizeof(id), "everyone");
        else idString(p.user, id);
        put("id=%s;volume=%u;muted=%s;\n", id, (unsigned)p.volume, p.mute ? "true" : "false");
    }
    return at < cap ? at : cap;
}

// One line: key=value pairs, each ended by ';' (or the line's end), in any order
static void prefsLine(const char* line, size_t len) {
    uint64_t user = 0;
    int volume = 100;
    bool muted = false;
    size_t k = 0;
    while (k < len) {
        size_t end = k;
        while (end < len && line[end] != ';') end++;
        size_t eq = k;
        while (eq < end && line[eq] != '=') eq++;
        if (eq < end) {
            const char* key = line + k;
            size_t key_n = eq - k;
            const char* val = line + eq + 1;
            size_t val_n = end - eq - 1;
            while (key_n && (key[0] == ' ' || key[0] == '\t')) key++, key_n--;
            while (val_n && (val[val_n - 1] == ' ' || val[val_n - 1] == '\r')) val_n--;
            if (key_n == 2 && memcmp(key, "id", 2) == 0) {
                user = val_n == 8 && memcmp(val, "everyone", 8) == 0 ? STORE_EVERYONE : parseId(val, val_n);
            } else if (key_n == 6 && memcmp(key, "volume", 6) == 0) {
                volume = 0;
                for (size_t i = 0; i < val_n && val[i] >= '0' && val[i] <= '9' && volume < 1000; i++) volume = volume * 10 + (val[i] - '0');
            } else if (key_n == 5 && memcmp(key, "muted", 5) == 0) {
                muted = (val_n == 4 && memcmp(val, "true", 4) == 0) || (val_n == 1 && val[0] == '1');
            }
        }
        k = end + 1;
    }
    if (!user) return;
    Pref* p = prefGet(user);
    if (!p) return;  // (the table is full)
    p->volume = (uint8_t)(volume > 200 ? 200 : volume);
    p->mute = muted;
    prefTidy(p);
}

void prefsRead(const char* text, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t end = i;
        while (end < n && text[end] != '\n') end++;
        if (end > i && text[i] != '#') prefsLine(text + i, end - i);
        i = end + 1;
    }
    changed();
}

size_t guildCount() {
    size_t n = 0;
    for (const Guild& g : g_guilds) n += visible(g);
    return n;
}

size_t listGuilds(size_t first, ChirpGuild* out, size_t cap) {
    size_t seen = 0, written = 0;
    for (int i = 0; i < STORE_GUILDS && written < cap; i++) {
        const Guild& g = g_guilds[i];
        if (!visible(g)) continue;
        if (seen++ < first) continue;
        ChirpGuild& o = out[written++];
        memset(&o, 0, sizeof(o));
        o.id = g.id;
        memcpy(o.name, g.name, sizeof(o.name));
        memcpy(o.icon, g.icon, sizeof(o.icon));
        for (const Channel& c : g_channels) {
            if (!c.used || c.guild != i || c.type != ChirpChannel_Voice) continue;
            o.voice_channels++;
        }
        for (const Member& m : g_members)
            if (m.used && m.guild == i) o.in_voice++;
    }
    return written;
}

size_t listChannels(uint64_t guild, ChirpChannel* out, size_t cap) {
    int slot = guildFind(guild);
    if (slot < 0) return 0;
    static uint16_t cats[256], voices[256];
    size_t nc = 0, nv = 0;
    for (size_t i = 0; i < STORE_CHANNELS; i++) {
        const Channel& c = g_channels[i];
        if (!c.used || c.guild != slot) continue;
        if (c.type == ChirpChannel_Category && nc < 256) cats[nc++] = (uint16_t)i;
        else if (c.type == ChirpChannel_Voice && nv < 256) voices[nv++] = (uint16_t)i;
    }
    sortChannels(cats, nc);
    sortChannels(voices, nv);
    size_t written = 0;
    auto isCategory = [&](uint64_t id) {
        for (size_t k = 0; k < nc; k++)
            if (g_channels[cats[k]].id == id) return true;
        return false;
    };
    // (those in no category, or in one we don't know, come first)
    for (size_t v = 0; v < nv && written < cap; v++)
        if (!isCategory(g_channels[voices[v]].parent)) toChirp(g_channels[voices[v]], &out[written++]);
    for (size_t k = 0; k < nc && written < cap; k++) {
        const Channel& cat = g_channels[cats[k]];
        bool any = false;
        for (size_t v = 0; v < nv; v++) any = any || g_channels[voices[v]].parent == cat.id;
        if (!any) continue;
        toChirp(cat, &out[written++]);
        for (size_t v = 0; v < nv && written < cap; v++)
            if (g_channels[voices[v]].parent == cat.id) toChirp(g_channels[voices[v]], &out[written++]);
    }
    return written;
}

size_t listMembers(uint64_t channel, uint64_t me, ChirpMember* out, size_t cap, size_t* total) {
    size_t written = 0, all = 0;
    for (const Member& m : g_members) {
        if (!m.used || m.channel != channel) continue;
        all++;
        if (written == cap) continue;
        ChirpMember& o = out[written++];
        memset(&o, 0, sizeof(o));
        o.user_id = m.user;
        o.flags = m.flags;
        if (m.user == me) o.flags |= ChirpMember_Me;
        if (prefMute(m.user)) o.flags |= ChirpMember_LocalMute;
        o.volume = (uint16_t)prefVolume(m.user);
        o.level = CHIRP_SILENT;  // (no voice connection yet: nothing comes in)
        if (m.flags & ChirpMember_NoName) {
            char id[21];
            idString(m.user, id);
            snprintf(o.name, sizeof(o.name), "Unknown (%s)", id);
        } else {
            memcpy(o.name, m.name, sizeof(o.name));
        }
        memcpy(o.avatar, m.avatar, sizeof(o.avatar));
    }
    if (total) *total = all;
    return written;
}

size_t membersIn(uint64_t channel) {
    size_t n = 0;
    for (const Member& m : g_members) n += m.used && m.channel == channel;
    return n;
}

}  // namespace store
