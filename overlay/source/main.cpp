// Chirp's overlay (libultrahand). Home: the bot (A: Profile, with the game's and the call's volumes, how
// it shows and which bot), how it is (the game it's playing, or how it's connected), and the servers it's in; a server's voice channels; a channel's people, and joining it. While the bot is in
// a call, opening the overlay shows the call instead: Mute, Deafen, Screenshare, the volumes, Settings,
// Disconnect, and the people in it (each with a mute and a volume that are only ours). Everything comes
// from the sysmodule (client.cpp). Lists are sorted by name.
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

#include "avatar.h"
#include "client.h"
#include "icons.h"
#include "texts.h"

namespace {

constexpr u64 POLL_NS = 250'000'000ULL;
constexpr u64 RECONNECT_NS = 1'000'000'000ULL;

// What the sysmodule last said
struct Now {
    bool running = false;
    ChirpStatus st = {};
    u64 polled_ns = 0, tried_ns = 0;
} g_now;

void poll(bool force = false) {
    u64 now = ult::nowNs();
    if (!client::connected()) {
        if (!force && now - g_now.tried_ns < RECONNECT_NS) return;
        g_now.tried_ns = now;
        if (!client::connect()) {
            g_now.running = false;
            return;
        }
    }
    if (!force && now - g_now.polled_ns < POLL_NS) return;
    g_now.polled_ns = now;
    g_now.running = client::status(&g_now.st);
}

bool inCall() { return g_now.running && g_now.st.in_call; }

using tsl::Color;
using tsl::gfx::Renderer;

// Colours from the Ultrahand theme (read when the overlay starts), so this
// looks like the menu around it
#define WHITE tsl::defaultTextColor
#define ACCENT tsl::trackBarFullColor
const Color GREY = Color(0xC, 0xC, 0xC, 0xF);
const Color DIM = Color(0x8, 0x8, 0x8, 0xF);
const Color WARN = Color(0xF, 0x6, 0x5, 0xF);
const Color GOOD = Color(0x5, 0xD, 0x7, 0xF);

std::string fmt(const char* f, unsigned v) {
    char s[128];
    snprintf(s, sizeof(s), f, v);
    return s;
}

std::string fmt(const char* f, const char* v) {
    char s[128];
    snprintf(s, sizeof(s), f, v);
    return s;
}

std::string hms(u32 seconds) {
    char s[16];
    snprintf(s, sizeof(s), "%02u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
    return s;
}

// What's only ours about someone, beside their name (mutes and deafens are icons)
std::string stateOf(const ChirpMember& m) {
    std::string s;
    auto add = [&](const char* t) { s += s.empty() ? t : std::string(", ") + t; };
    if (m.flags & ChirpMember_Me) add(txt::YOU);
    else if (m.volume != 100) add((std::to_string(m.volume) + "%").c_str());
    return s;
}

// Names in order, case aside (then as they are, so the order never wobbles)
bool nameLess(const char* a, const char* b) {
    int c = strcasecmp(a, b);
    return c ? c < 0 : strcmp(a, b) < 0;
}

void byName(std::vector<ChirpMember>& people) {
    std::stable_sort(people.begin(), people.end(), [](const ChirpMember& a, const ChirpMember& b) { return nameLess(a.name, b.name); });
}

void byName(std::vector<ChirpGuild>& guilds) {
    std::stable_sort(guilds.begin(), guilds.end(), [](const ChirpGuild& a, const ChirpGuild& b) { return nameLess(a.name, b.name); });
}

// A server's channels: the categories as Discord has them, the channels under each by name
void byName(std::vector<ChirpChannel>& chans) {
    for (size_t i = 0; i < chans.size();) {
        if (chans[i].type == ChirpChannel_Category) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < chans.size() && chans[j].type != ChirpChannel_Category) j++;
        std::stable_sort(chans.begin() + i, chans.begin() + j,
                         [](const ChirpChannel& a, const ChirpChannel& b) { return nameLess(a.name, b.name); });
        i = j;
    }
}

// A value on the right of a row, in the theme's colour ("2 active"), ending at right and centred
// on cy: its left edge, to place what comes before it
s32 accentValue(Renderer* r, const std::string& text, s32 right, s32 cy) {
    s32 w = r->getTextDimensions(text, false, 19).first;
    r->drawString(text, false, right - w, cy + 7, 19, r->a(ACCENT));
    return right - w;
}

// A mute or deafen icon (icons.h) at (x, y): the shape in one colour, its slash in another
void icon(Renderer* r, icons::Kind k, s32 x, s32 y, Color body, Color slash) {
    r->drawBitmapRGBA4444(x, y, icons::ICON, icons::ICON, icons::get(k, {body.r, body.g, body.b}, {slash.r, slash.g, slash.b}));
}

// The item each page had the cursor on, to put it back there when the page
// is made again (the lists change under it while it's open): the list jumps
// to the item whose key it is, scrolling it into view (tsl's jumpToItem)
u64 g_pick_main = 0, g_pick_guild = 0, g_pick_channel = 0, g_pick_call = 0, g_pick_settings = 0;

std::string jumpKey(u64 key) { return "#" + std::to_string(key); }

// A list item that knows what it stands for (key), and says so when it has the cursor
class PickItem : public tsl::elm::ListItem {
public:
    PickItem(const std::string& text, const std::string& value, u64 key, u64* pick)
        : ListItem(text, value), key_(key), pick_(pick) {}
    // (the list jumps to it, and scrolls to it, when its page is made again: see jumpKey)
    bool matchesJumpCriteria(const std::string& text, const std::string&, bool) const override {
        return text == jumpKey(key_);
    }
    void setFocused(bool f) override {
        ListItem::setFocused(f);
        if (f && pick_) *pick_ = key_;
    }

private:
    u64 key_;
    u64* pick_;
};

// A toggle that does the same (the call's Mute, Deafen and Screenshare, the settings' switches)
class PickToggle : public tsl::elm::ToggleListItem {
public:
    PickToggle(const std::string& text, bool on, u64 key, u64* pick) : ToggleListItem(text, on), key_(key), pick_(pick) {}
    // (the list jumps to it, and scrolls to it, when its page is made again: see jumpKey)
    bool matchesJumpCriteria(const std::string& text, const std::string&, bool) const override {
        return text == jumpKey(key_);
    }
    void setFocused(bool f) override {
        ToggleListItem::setFocused(f);
        if (f) *pick_ = key_;
    }

private:
    u64 key_;
    u64* pick_;
};

// A slider that does the same (the volume's), so the cursor stays on it when its page is made again.
// 41 steps: 0..200% in 5s (tsl's step bar draws them where they are; its plain bar draws any range as 0..100)
constexpr size_t VOLUME_STEPS = 41;
class PickTrackBar : public tsl::elm::StepTrackBar {
public:
    PickTrackBar(u64 key, u64* pick, size_t steps = VOLUME_STEPS) : StepTrackBar("", steps), key_(key), pick_(pick) {}
    bool matchesJumpCriteria(const std::string& text, const std::string&, bool) const override {
        return text == jumpKey(key_);
    }
    void setFocused(bool f) override {
        StepTrackBar::setFocused(f);
        if (f && pick_) *pick_ = key_;
    }

private:
    u64 key_;
    u64* pick_;
};

// A slider's label ("Call Volume 100%"): shown, but the cursor goes past it to the slider under it
// (m_isItem off, as tsl's own headers: the list then never puts the cursor on it, not even first)
class Label : public tsl::elm::ListItem {
public:
    Label(const std::string& text, const std::string& value) : ListItem(text, value) { m_isItem = false; }
    tsl::elm::Element* requestFocus(tsl::elm::Element*, tsl::FocusDirection) override { return nullptr; }
};

// Everything Chirp plays: "Call Volume 100%", and under it a slider, 0..200% in steps of 5%
void addVolume(tsl::elm::List* list, u64 key, u64* pick) {
    u16 v = g_now.st.volume;
    auto* label = new Label(txt::CALL_VOLUME, std::to_string(v) + "%");
    list->addItem(label);
    auto* bar = new PickTrackBar(key, pick);
    bar->setProgress((u16)(v / 5));
    bar->setValueChangedListener([label](u16 step) {
        int volume = step * 5;
        client::setVolume(volume);
        g_now.st.volume = (u16)volume;  // (until the next poll says so too)
        label->setValue(std::to_string(volume) + "%");
    });
    list->addItem(bar);
}

// The game's own volume, apart from the call's: "Game Volume 100%", and a slider, 0..100% in steps of 5%
// (not there when the sysmodule running is older than this overlay: it hasn't the command)
constexpr size_t GAME_VOLUME_STEPS = 21;
void addGameVolume(tsl::elm::List* list, u64 key, u64* pick) {
    if (client::outdated()) return;
    u16 v = g_now.st.game_volume;
    auto* label = new Label(txt::GAME_VOLUME, std::to_string(v) + "%");
    list->addItem(label);
    auto* bar = new PickTrackBar(key, pick, GAME_VOLUME_STEPS);
    bar->setProgress((u16)(v / 5));
    bar->setValueChangedListener([label](u16 step) {
        int volume = step * 5;
        client::setGameVolume(volume);
        g_now.st.game_volume = (uint8_t)volume;  // (until the next poll says so too)
        label->setValue(std::to_string(volume) + "%");
    });
    list->addItem(bar);
}

// A picture, or (none yet) a circle in a colour of theirs with the name's first letter
void picture(Renderer* r, const char* file, u64 id, const char* name, s32 cx, s32 cy) {
    if (const uint8_t* px = file[0] ? avatar::get(file) : nullptr) {
        r->drawBitmapRGBA4444(cx - avatar::SIZE / 2, cy - avatar::SIZE / 2, avatar::SIZE, avatar::SIZE, px);
        return;
    }
    static const Color COLOURS[] = {Color(0x5, 0x6, 0xE, 0xF), Color(0x7, 0x7, 0x8, 0xF), Color(0x3, 0xA, 0x5, 0xF),
                                    Color(0xE, 0xA, 0x2, 0xF), Color(0xE, 0x4, 0x4, 0xF), Color(0xE, 0x4, 0x9, 0xF)};
    r->drawCircle(cx, cy, avatar::SIZE / 2, true, r->a(COLOURS[(id >> 22) % 6]));
    std::string letter = name[0] ? std::string(name, 1) : "?";
    if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 32;
    if ((unsigned char)letter[0] >= 0x80) letter = "?";  // (not half a character)
    s32 lw = r->getTextDimensions(letter, false, 18).first;
    r->drawString(letter, false, cx - lw / 2, cy + 7, 18, r->a(WHITE));
}

// text cut (at a character boundary) until it fits in room at size
std::string fitted(Renderer* r, std::string text, s32 room, u32 size) {
    while (text.size() > 1 && r->getTextDimensions(text, false, size).first > room) {
        size_t cut = text.size() - 1;
        while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) cut--;
        text.resize(cut);
    }
    return text;
}

// Someone in voice: their picture (a ring around it while they talk), their
// name, and how they are on the right
class MemberRow : public tsl::elm::Element {
public:
    static constexpr u16 HEIGHT = 64;
    // open: what A on it does (null: nothing)
    MemberRow(const ChirpMember& m, u64* pick, std::function<void()> open)
        : m_(m), state_(stateOf(m)), pick_(pick), open_(std::move(open)) {}
    void layout(u16, u16, u16, u16) override {}
    u64 user() const { return m_.user_id; }
    // (who's talking changes all the time: the call page sets it from each poll, not by making the page again)
    void setSpeaking(bool on) {
        if (on) m_.flags |= ChirpMember_Speaking;
        else m_.flags &= ~ChirpMember_Speaking;
    }
    // (every row takes the cursor, so a long list scrolls; A only does something where open_ is set.
    // tsl's Element says no by default, which kept the cursor off these rows altogether)
    tsl::elm::Element* requestFocus(tsl::elm::Element*, tsl::FocusDirection) override { return this; }
    // (the list jumps to it, and scrolls to it, when its page is made again: see jumpKey)
    bool matchesJumpCriteria(const std::string& text, const std::string&, bool) const override {
        return text == jumpKey(m_.user_id);
    }
    void setFocused(bool f) override {
        Element::setFocused(f);
        if (f) *pick_ = m_.user_id;
    }
    bool onClick(u64 keys) override {
        if (!(keys & HidNpadButton_A) || !open_) return false;
        open_();
        return true;
    }
    void draw(Renderer* r) override {
        s32 x = getX() + 6, y = getY(), w = getWidth() - 12, cy = y + HEIGHT / 2;
        r->drawRect(x, y + HEIGHT - 1, w, 1, r->a(tsl::separatorColor));
        s32 ax = x + 10, acx = ax + avatar::SIZE / 2;
        if (m_.flags & ChirpMember_Speaking) r->drawRing(acx, cy, avatar::SIZE / 2 + 4, 3, r->a(GOOD));
        picture(r, m_.avatar, m_.user_id, (m_.flags & ChirpMember_NoName) ? "" : m_.name, acx, cy);
        s32 tx = ax + avatar::SIZE + 14;
        // from the right: deafened, then muted: grey when they did it, red when a moderator did,
        // a grey mic with a red slash when only we muted them (as Discord shows them)
        s32 right = x + w - 12;
        if (open_) {  // (A opens their page: a caret says so)
            s32 cw = r->getTextDimensions(txt::CARET, false, 20).first;
            right -= cw;
            r->drawString(txt::CARET, false, right, cy + 7, 20, r->a(WHITE));  // (as Ultrahand draws it)
            right -= 14;
        }
        u8 f = m_.flags;
        bool deaf = (f & (ChirpMember_SelfDeaf | ChirpMember_ServerDeaf)) != 0;
        if (deaf) {
            right -= icons::ICON;
            Color c = (f & ChirpMember_ServerDeaf) ? WARN : GREY;
            icon(r, icons::HeadphonesOff, right, cy - icons::ICON / 2, c, c);
            right -= 8;
        }
        // (deafened is muted too: only the headphones then, unless a moderator or we muted them)
        if ((f & (ChirpMember_ServerMute | ChirpMember_LocalMute)) || (!deaf && (f & ChirpMember_SelfMute))) {
            right -= icons::ICON;
            Color body = (f & ChirpMember_ServerMute) ? WARN : GREY;
            Color slash = (f & (ChirpMember_ServerMute | ChirpMember_LocalMute)) ? WARN : GREY;
            icon(r, icons::MicOff, right, cy - icons::ICON / 2, body, slash);
            right -= 8;
        }
        s32 sw = state_.empty() ? 0 : r->getTextDimensions(state_, false, 15).first;
        if (sw) {
            right -= sw;
            r->drawString(state_, false, right, cy + 6, 15, r->a(GREY));
            right -= 12;
        }
        r->drawString(fitted(r, m_.name, right - tx, 20), false, tx, cy + 7, 20, r->a(WHITE));
    }

private:
    ChirpMember m_;
    std::string state_;
    u64* pick_;
    std::function<void()> open_;
};

// --- the main menu ---

class GuildGui;
class CallGui;
class ProfileGui;

// A row of Home's Profile: drawn by draw(), A does onA() (when there is one); every row takes the
// cursor so the list scrolls. key keeps the cursor on it when the page is made again
class ProfileRow : public tsl::elm::Element {
public:
    static constexpr u16 HEIGHT = 64;
    ProfileRow(u64 key, u64* pick) : key_(key), pick_(pick) {}
    void layout(u16, u16, u16, u16) override {}
    tsl::elm::Element* requestFocus(tsl::elm::Element*, tsl::FocusDirection) override { return this; }
    bool matchesJumpCriteria(const std::string& text, const std::string&, bool) const override { return text == jumpKey(key_); }
    void setFocused(bool f) override {
        Element::setFocused(f);
        if (f) *pick_ = key_;
    }
    bool onClick(u64 keys) override {
        if (!(keys & HidNpadButton_A)) return false;
        return onA();
    }

protected:
    virtual bool onA() { return false; }
    // (the row's line under it; x, w and cy for what's in it)
    void frame(Renderer* r, s32* x, s32* w, s32* cy) {
        *x = getX() + 6, *w = getWidth() - 12, *cy = getY() + HEIGHT / 2;
        r->drawRect(*x, getY() + HEIGHT - 1, *w, 1, r->a(tsl::separatorColor));
    }

private:
    u64 key_;
    u64* pick_;
};

// How the bot is, under it (not a button): a dot (green: online and visible; grey: invisible or not there yet;
// red: something to fix) and the game it's playing (with Game Activity on), else how it's connected
class StatusRow : public ProfileRow {
public:
    using ProfileRow::ProfileRow;
    void draw(Renderer* r) override {
        s32 x, w, cy;
        frame(r, &x, &w, &cy);
        const ChirpStatus& s = g_now.st;
        std::string text;
        Color dot = DIM;
        if (!g_now.running) {
            text = txt::ST_NOT_RUNNING, dot = WARN;
        } else if (client::outdated()) {
            text = txt::ST_OUTDATED, dot = WARN;
        } else if (s.conn == ChirpConn_Online) {
            if (s.invisible) text = txt::ST_INVISIBLE;
            else text = s.rich_presence && s.game[0] ? s.game : txt::ST_ONLINE, dot = GOOD;
        } else if (s.conn == ChirpConn_Connecting || s.conn == ChirpConn_Offline) {
            text = txt::ST_RECONNECTING;
        } else if (s.conn == ChirpConn_BadToken) {
            text = txt::ST_BAD_TOKEN, dot = WARN;
        } else if (s.conn == ChirpConn_Refused) {
            text = fmt(txt::ST_REFUSED, s.last_close), dot = WARN;
        } else {
            text = txt::ST_NO_TOKEN, dot = WARN;
        }
        r->drawCircle(x + 19, cy, 7, true, r->a(dot));
        s32 tx = x + 42, right = x + w - 12;
        r->drawString(fitted(r, text, right - tx, 20), false, tx, cy + 7, 20, r->a(WHITE));
    }
};

// The bot: its picture and name. A opens Profile (the volumes, how it shows, which bot)
class BotRow : public ProfileRow {
public:
    using ProfileRow::ProfileRow;
    void draw(Renderer* r) override {
        s32 x, w, cy;
        frame(r, &x, &w, &cy);
        const ChirpStatus& s = g_now.st;
        std::string name = s.user[0] ? s.user : s.login[0] ? s.login : txt::TITLE;
        picture(r, s.avatar, 0, name.c_str(), x + 10 + avatar::SIZE / 2, cy);
        s32 right = x + w - 12 - r->getTextDimensions(txt::CARET, false, 20).first;
        r->drawString(txt::CARET, false, right, cy + 7, 20, r->a(WHITE));  // (as Ultrahand draws it)
        s32 tx = x + 10 + avatar::SIZE + 14;
        r->drawString(fitted(r, name, right - 14 - tx, 20), false, tx, cy + 7, 20, r->a(WHITE));
    }

protected:
    bool onA() override;
};

// A server: its icon, its name, and how many are active in its voice channels (A opens it)
class GuildRow : public tsl::elm::Element {
public:
    static constexpr u16 HEIGHT = 64;
    GuildRow(const ChirpGuild& g, u64* pick) : g_(g), pick_(pick) {}
    void layout(u16, u16, u16, u16) override {}
    tsl::elm::Element* requestFocus(tsl::elm::Element*, tsl::FocusDirection) override { return this; }
    bool matchesJumpCriteria(const std::string& text, const std::string&, bool) const override { return text == jumpKey(g_.id); }
    void setFocused(bool f) override {
        Element::setFocused(f);
        if (f) *pick_ = g_.id;
    }
    bool onClick(u64 keys) override;
    void draw(Renderer* r) override {
        s32 x = getX() + 6, y = getY(), w = getWidth() - 12, cy = y + HEIGHT / 2;
        r->drawRect(x, y + HEIGHT - 1, w, 1, r->a(tsl::separatorColor));
        picture(r, g_.icon, g_.id, g_.name, x + 10 + avatar::SIZE / 2, cy);
        s32 right = x + w - 12;
        if (g_.in_voice) right = accentValue(r, fmt(txt::ACTIVE_N, g_.in_voice), right, cy) - 12;
        s32 tx = x + 10 + avatar::SIZE + 14;
        r->drawString(fitted(r, g_.name, right - tx, 20), false, tx, cy + 7, 20, r->a(WHITE));
    }

private:
    ChirpGuild g_;
    u64* pick_;
};

class MainGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        poll(true);
        serial_ = g_now.st.serial;
        conn_ = g_now.running ? g_now.st.conn : 0xFF;
        auto* frame = new tsl::elm::OverlayFrame(txt::TITLE, APP_VERSION);
        auto* list = new tsl::elm::List();
        // (the bot, and how it is: keys 1..2, no server's id is that small)
        if (g_now.running) list->addItem(new BotRow(2, &g_pick_main), ProfileRow::HEIGHT);
        list->addItem(new StatusRow(1, &g_pick_main), ProfileRow::HEIGHT);
        if (g_now.running && g_now.st.conn == ChirpConn_Online) {
            list->addItem(new tsl::elm::CategoryHeader(txt::SERVERS));
            auto guilds = client::guilds();
            byName(guilds);
            for (const ChirpGuild& g : guilds) list->addItem(new GuildRow(g, &g_pick_main), GuildRow::HEIGHT);
            if (guilds.empty()) list->addItem(new tsl::elm::ListItem(txt::NO_SERVERS));
        }
        if (g_pick_main) list->jumpToItem(jumpKey(g_pick_main));
        frame->setContent(list);
        return frame;
    }

    void update() override;

private:
    u32 serial_ = 0;
    u8 conn_ = 0;
};

// --- a server: its voice channels ---

// A voice channel: its name, how many are active in it (when anyone is), and a caret (A opens it)
class ChannelRow : public tsl::elm::ListItem {
public:
    ChannelRow(const ChirpChannel& c, u64* pick) : ListItem(c.name, txt::CARET), id_(c.id), members_(c.members), pick_(pick) {}
    // (the list jumps to it, and scrolls to it, when its page is made again: see jumpKey)
    bool matchesJumpCriteria(const std::string& text, const std::string&, bool) const override {
        return text == jumpKey(id_);
    }
    void setFocused(bool f) override {
        ListItem::setFocused(f);
        if (f) *pick_ = id_;
    }
    void draw(Renderer* r) override {
        ListItem::draw(r);
        if (members_) {  // (before the caret: the value Ultrahand draws, its right edge 22 in)
            s32 cw = r->getTextDimensions(txt::CARET, false, 20).first;
            accentValue(r, fmt(txt::ACTIVE_N, members_), getX() + getWidth() - 22 - cw - 12, getY() + getHeight() / 2);
        }
    }

private:
    u64 id_;
    unsigned members_;
    u64* pick_;
};

class ChannelGui;

// Every page under the main menu goes back to it when a call starts (so the
// call is what's shown) or the session ends
class SubGui : public tsl::Gui {
public:
    void update() override {
        poll();
        if (inCall() || !g_now.running || g_now.st.conn != ChirpConn_Online) {
            tsl::goBack();
            return;
        }
        if (g_now.st.serial != serial_) refresh();
    }

protected:
    virtual void refresh() = 0;
    u32 serial_ = 0;
};

class GuildGui : public SubGui {
public:
    GuildGui(u64 id, std::string name) : id_(id), name_(std::move(name)) {}
    tsl::elm::Element* createUI() override {
        serial_ = g_now.st.serial;
        auto* frame = new tsl::elm::OverlayFrame(name_, txt::VOICE_CHANNELS);
        auto* list = new tsl::elm::List();
        auto chans = client::channels(id_);
        byName(chans);
        bool any = false;
        for (const ChirpChannel& c : chans) {
            if (c.type == ChirpChannel_Category) {
                list->addItem(new tsl::elm::CategoryHeader(c.name));
                continue;
            }
            any = true;
            auto* row = new ChannelRow(c, &g_pick_guild);
            u64 guild = id_, chan = c.id;
            std::string gname = name_, cname = c.name;
            row->setClickListener([guild, chan, gname, cname](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;
                g_pick_channel = 0;
                tsl::changeTo<ChannelGui>(guild, chan, gname, cname);
                return true;
            });
            list->addItem(row);
        }
        if (!any) list->addItem(new tsl::elm::ListItem(txt::NO_VOICE_CHANNELS));
        if (g_pick_guild) list->jumpToItem(jumpKey(g_pick_guild));
        frame->setContent(list);
        return frame;
    }

protected:
    void refresh() override { tsl::swapTo<GuildGui>(id_, name_); }

private:
    u64 id_;
    std::string name_;
};

// --- a voice channel: join it, and who's there ---

class ChannelGui : public SubGui {
public:
    ChannelGui(u64 guild, u64 channel, std::string gname, std::string cname)
        : guild_(guild), channel_(channel), gname_(std::move(gname)), cname_(std::move(cname)) {}
    tsl::elm::Element* createUI() override {
        serial_ = g_now.st.serial;
        auto* frame = new tsl::elm::OverlayFrame(cname_, gname_);
        auto* list = new tsl::elm::List();
        // (muted or not as the bot last was: the sysmodule keeps that between calls)
        auto* join = new tsl::elm::ListItem(txt::JOIN, g_now.st.self_mute ? std::string(txt::JOIN_AS_MUTED) + "  " + txt::CARET
                                                                          : std::string(txt::CARET));
        u64 g = guild_, c = channel_;
        join->setClickListener([join, g, c](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            client::join(g, c, g_now.st.self_mute != 0);
            join->setValue(txt::JOINING);  // (the call shows once Discord says the bot's in)
            return true;
        });
        list->addItem(join);
        auto people = client::members(channel_);
        byName(people);
        list->addItem(new tsl::elm::CategoryHeader(fmt(txt::ACTIVE_MEMBERS, (unsigned)people.size())));
        for (const ChirpMember& m : people) list->addItem(new MemberRow(m, &g_pick_channel, nullptr), MemberRow::HEIGHT);
        if (people.empty()) list->addItem(new tsl::elm::ListItem(txt::NOBODY));
        if (g_pick_channel) list->jumpToItem(jumpKey(g_pick_channel));  // (where it was before the list changed)
        frame->setContent(list);
        return frame;
    }

protected:
    void refresh() override { tsl::swapTo<ChannelGui>(guild_, channel_, gname_, cname_); }

private:
    u64 guild_, channel_;
    std::string gname_, cname_;
};

// --- in a call ---

// --- Settings: the mic (input volume, sensitivity, automatic gain; kept in settings.ini), and Debug ---

// The sensitivity slider: step 0 is Automatic, then -80 dB up to -15 in 5s (further right, louder
// to count as talk)
constexpr size_t SENS_STEPS = 15;
int sensitivityOf(u16 step) { return step == 0 ? 0 : -85 + 5 * (int)step; }
u16 stepOf(int sensitivity) {
    if (sensitivity >= 0) return 0;
    int step = (sensitivity + 85) / 5;
    return (u16)(step < 1 ? 1 : step > (int)SENS_STEPS - 1 ? SENS_STEPS - 1 : step);
}
std::string sensitivityText(int sensitivity) {
    return sensitivity >= 0 ? std::string(txt::AUTOMATIC) : std::to_string(sensitivity) + " dB";
}

// A row only to look at: a name and its value, grey
tsl::elm::ListItem* infoRow(tsl::elm::List* list, const char* what, const std::string& value) {
    auto* item = new tsl::elm::ListItem(what, "");
    item->setValue(value, true);
    item->setClickListener([](u64) { return false; });
    list->addItem(item);
    return item;
}

// The call page's Settings section: the mic's input volume, sensitivity (with the mic's level beside the
// line, to pick it by) and automatic gain. Keys 7..10 keep the cursor on them when the page is made again
struct MicSettings {
    int volume = 100, sens = 0, light = 50;
    bool agc = true, denoise = true;
    void send() const { client::setMic(volume, sens, agc, denoise, light); }
} g_mic;

// The HOME button's light: Off, then each of the light's 15 levels (as percent, 7% to 100%)
constexpr size_t LIGHT_STEPS = 16;
int lightOf(u16 step) { return (step * 100 + 7) / 15; }
u16 lightStep(int light) { return (u16)((light * 15 + 50) / 100); }  // (as the sysmodule rounds it)
std::string lightText(int light) { return light <= 0 ? std::string(txt::LIGHT_OFF) : std::to_string(light) + "%"; }

tsl::elm::ListItem* addMicSettings(tsl::elm::List* list, u64* pick) {
    g_mic.volume = g_now.st.mic_volume, g_mic.sens = g_now.st.mic_sensitivity, g_mic.agc = g_now.st.mic_agc != 0;
    g_mic.denoise = g_now.st.mic_denoise != 0;
    g_mic.light = g_now.st.mic_home_light;
    auto* vol = new Label(txt::INPUT_VOLUME, std::to_string(g_mic.volume) + "%");
    list->addItem(vol);
    auto* volBar = new PickTrackBar(7, pick);
    volBar->setProgress((u16)(g_mic.volume / 5));
    volBar->setValueChangedListener([vol](u16 step) {
        g_mic.volume = step * 5;
        vol->setValue(std::to_string(g_mic.volume) + "%");
        g_mic.send();
    });
    list->addItem(volBar);
    auto* sens = new Label(txt::INPUT_SENSITIVITY, sensitivityText(g_mic.sens));
    list->addItem(sens);
    auto* sensBar = new PickTrackBar(8, pick, SENS_STEPS);
    sensBar->setProgress(stepOf(g_mic.sens));
    sensBar->setValueChangedListener([sens](u16 step) {
        g_mic.sens = sensitivityOf(step);
        sens->setValue(sensitivityText(g_mic.sens));
        g_mic.send();
    });
    list->addItem(sensBar);
    auto* level = infoRow(list, txt::MIC_LEVEL, "");  // (Input Level: now, and the line talk must be over)
    auto* agc = new PickToggle(txt::AGC, g_mic.agc, 9, pick);
    agc->setStateChangedListener([](bool on) {
        g_mic.agc = on;
        g_mic.send();
    });
    list->addItem(agc);
    auto* dn = new PickToggle(txt::DENOISE, g_mic.denoise, 11, pick);
    dn->setStateChangedListener([](bool on) {
        g_mic.denoise = on;
        g_mic.send();
    });
    list->addItem(dn);
    auto* light = new Label(txt::HOME_LIGHT, lightText(g_mic.light));
    list->addItem(light);
    auto* lightBar = new PickTrackBar(13, pick, LIGHT_STEPS);
    lightBar->setProgress(lightStep(g_mic.light));
    lightBar->setValueChangedListener([light](u16 step) {
        g_mic.light = lightOf(step);
        light->setValue(lightText(g_mic.light));
        g_mic.send();
    });
    list->addItem(lightBar);
    return level;
}

// The mic's level against the line, for the Settings section (from the Debug numbers)
std::string micLevelText(const ChirpDebug& d) {
    char t[48];
    if (d.mic_open) snprintf(t, sizeof(t), txt::MIC_LEVEL_VALUE, (int)d.mic_level, (int)d.mic_threshold);
    else snprintf(t, sizeof(t), "%s", txt::MIC_CLOSED);
    return t;
}

// How long the bot's been in the call (the seconds counted here between polls, so the clock ticks smoothly)
u32 callElapsed() { return g_now.st.call_seconds + (u32)((ult::nowNs() - g_now.polled_ns) / 1'000'000'000ULL); }
std::string connectedFor() { return fmt(txt::CONNECTED_FOR, hms(callElapsed()).c_str()); }

// Settings (from the call page): the mic, then what's underneath as it is now (Debug: Screenshare,
// Network, Audio, System), all on the one page
class SettingsGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        poll(true);
        auto* frame = new tsl::elm::OverlayFrame(txt::SETTINGS, g_now.st.call_channel_name);
        auto* list = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader(txt::INPUT));
        level_ = addMicSettings(list, &g_pick_settings);
        // (the video's colour range as labelled: grc says limited; full if blacks look crushed. Next keyframe)
        list->addItem(new tsl::elm::CategoryHeader(txt::SCREENSHARE_SECTION));
        auto* range = new PickToggle(txt::FULL_RANGE, g_now.st.full_range != 0, 12, &g_pick_settings);
        range->setStateChangedListener([](bool on) { client::setFullRange(on); });
        list->addItem(range);
        list->addItem(new tsl::elm::CategoryHeader(txt::DBG_SCREENSHARE));
        rows_[R_RANGE] = infoRow(list, txt::DBG_RANGE, "");
        rows_[R_REC_FPS] = infoRow(list, txt::DBG_REC_FPS, "");
        rows_[R_FPS] = infoRow(list, txt::DBG_FPS, "");
        rows_[R_BITRATE] = infoRow(list, txt::DBG_BITRATE, "");
        rows_[R_LATE] = infoRow(list, txt::DBG_LATE, "");
        rows_[R_LOST] = infoRow(list, txt::DBG_LOST, "");
        rows_[R_BIGGEST] = infoRow(list, txt::DBG_BIGGEST, "");
        list->addItem(new tsl::elm::CategoryHeader(txt::DBG_NETWORK));
        rows_[R_PING_GW] = infoRow(list, txt::DBG_PING_GATEWAY, "");
        rows_[R_PING_VOICE] = infoRow(list, txt::DBG_PING_VOICE, "");
        rows_[R_PING_STREAM] = infoRow(list, txt::DBG_PING_STREAM, "");
        rows_[R_HOST_VOICE] = infoRow(list, txt::DBG_HOST_VOICE, "");
        rows_[R_HOST_STREAM] = infoRow(list, txt::DBG_HOST_STREAM, "");
        list->addItem(new tsl::elm::CategoryHeader(txt::DBG_AUDIO));
        rows_[R_SPEAKERS] = infoRow(list, txt::DBG_SPEAKERS, "");
        rows_[R_DECODERS] = infoRow(list, txt::DBG_DECODERS, "");
        rows_[R_GAIN] = infoRow(list, txt::DBG_MIC_GAIN, "");
        list->addItem(new tsl::elm::CategoryHeader(txt::DBG_SYSTEM));
        rows_[R_FREE] = infoRow(list, txt::DBG_SYSTEM_FREE, "");
        rows_[R_CHIRP] = infoRow(list, txt::DBG_CHIRP, "");
        rows_[R_HEAP] = infoRow(list, txt::DBG_HEAP, "");
        rows_[R_CPU] = infoRow(list, txt::DBG_CHIRP_CPU, "");
        rows_[R_IDLE] = infoRow(list, txt::DBG_CORE3_IDLE, "");
        if (g_pick_settings) list->jumpToItem(jumpKey(g_pick_settings));
        frame->setContent(list);
        refresh();
        return frame;
    }

    void update() override {
        poll();
        if (!inCall()) {
            tsl::goBack();
            return;
        }
        if (ult::nowNs() - refreshed_ns_ >= POLL_NS) refresh();
    }

private:
    enum {
        R_RANGE, R_REC_FPS, R_FPS, R_BITRATE, R_LATE, R_LOST, R_BIGGEST,
        R_PING_GW, R_PING_VOICE, R_PING_STREAM, R_HOST_VOICE, R_HOST_STREAM,
        R_SPEAKERS, R_DECODERS, R_GAIN,
        R_FREE, R_CHIRP, R_HEAP, R_IDLE, R_CPU,
        ROWS
    };
    // The server's own name, short: "c-sjc05-b80a87ec" of "c-sjc05-b80a87ec.discord.media:443" (Ultrahand
    // draws a value too wide for its row off the screen: the whole name never showed), and how it's going
    // when it isn't connected
    static std::string server(const char* host, u8 state) {
        if (!host[0]) return txt::DBG_NONE;
        std::string s = host;
        size_t dot = s.find_first_of(".:");
        if (dot != std::string::npos) s.resize(dot);
        if (state != 2) s += std::string(" (") + txt::DBG_STATES[state < 4 ? state : 0] + ")";
        return s;
    }
    static std::string ms(u16 v) { return v ? std::to_string(v) + " ms" : std::string("-"); }
    static std::string kb(u32 v) {
        char t[24];
        if (v >= 1024) snprintf(t, sizeof(t), "%u.%02u MB", v / 1024, v % 1024 * 100 / 1024);
        else snprintf(t, sizeof(t), "%u KB", v);
        return t;
    }
    void set(int i, const std::string& v) {
        if (v != shown_[i]) rows_[i]->setValue(shown_[i] = v, true);
    }

    // (a few times a second: the mic's level, and the rest; rates over the last second or so)
    void refresh() {
        u64 now = ult::nowNs();
        refreshed_ns_ = now;
        ChirpDebug d;
        if (!client::debug(&d)) return;
        std::string m = micLevelText(d);
        if (m != level_text_) level_->setValue(level_text_ = m, true);
        char t[48];
        bool sharing = d.stream_state == 2;
        set(R_RANGE, d.color_range < 0 ? txt::DBG_NA : d.color_range ? txt::DBG_FULL : txt::DBG_LIMITED);
        set(R_REC_FPS, d.record_fps ? std::to_string(d.record_fps) : std::string(txt::DBG_NA));
        // (frames and bytes sent: per second from two readings a second or more apart)
        if (!sharing) {
            rate_ns_ = 0;
            set(R_FPS, "-"), set(R_BITRATE, "-");
        } else if (!rate_ns_ || d.stream_frames < rate_frames_) {
            rate_ns_ = now, rate_frames_ = d.stream_frames, rate_kb_ = d.stream_kb;
        } else if (now - rate_ns_ >= 1'000'000'000ULL) {
            double secs = (double)(now - rate_ns_) / 1e9;
            snprintf(t, sizeof(t), "%.0f fps", (d.stream_frames - rate_frames_) / secs);
            set(R_FPS, t);
            snprintf(t, sizeof(t), "%.1f Mbps", (d.stream_kb - rate_kb_) * 8.0 * 1024 / 1e6 / secs);
            set(R_BITRATE, t);
            rate_ns_ = now, rate_frames_ = d.stream_frames, rate_kb_ = d.stream_kb;
        }
        set(R_LATE, sharing ? std::to_string(d.stream_late) : std::string("-"));
        set(R_LOST, sharing ? std::to_string(d.stream_lost) : std::string("-"));
        set(R_BIGGEST, sharing && d.stream_frame_max_kb ? kb(d.stream_frame_max_kb) : std::string("-"));
        set(R_PING_GW, ms(d.gateway_ping));
        set(R_PING_VOICE, ms(d.voice_ping));
        set(R_PING_STREAM, ms(d.stream_ping));
        set(R_HOST_VOICE, server(d.voice_server, d.voice_state));
        set(R_HOST_STREAM, server(d.stream_server, d.stream_state));
        snprintf(t, sizeof(t), txt::DBG_OF, (int)d.speakers, (int)d.speakers_max);
        set(R_SPEAKERS, t);
        snprintf(t, sizeof(t), txt::DBG_OF, (int)d.decoders, (int)d.decoders_max);
        set(R_DECODERS, t);
        snprintf(t, sizeof(t), "%+d dB", (int)d.mic_gain);
        set(R_GAIN, t);
        set(R_FREE, kb(d.system_free_kb));
        set(R_CHIRP, kb(d.chirp_kb));
        snprintf(t, sizeof(t), "%u / %u KB", d.heap_used_kb, d.heap_kb);
        set(R_HEAP, t);
        set(R_IDLE, std::to_string(d.core3_idle) + "%");
        snprintf(t, sizeof(t), "%u.%u%%", d.chirp_cpu / 10, d.chirp_cpu % 10);
        set(R_CPU, t);
    }

    tsl::elm::ListItem* level_ = nullptr;
    std::string level_text_;
    tsl::elm::ListItem* rows_[ROWS] = {};
    std::string shown_[ROWS];
    u64 refreshed_ns_ = 0;
    u64 rate_ns_ = 0;
    u32 rate_frames_ = 0, rate_kb_ = 0;
};

class UserGui;

// Controls (Disconnect, Deafen, Mute), then the members, us first; picking
// someone else gives our mute and volume for them
class CallGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        poll(true);
        serial_ = g_now.st.serial;
        // (the channel, and under it how long the bot's been in it, ticking: update() sets it)
        frame_ = new tsl::elm::OverlayFrame(fmt(txt::CALL_TITLE, g_now.st.call_channel_name), connectedFor());
        auto* list = new tsl::elm::List();
        // (keys 1..3 for these, members' ids for them: the cursor stays where it
        // was when the page is made again; it starts on Mute, not on Disconnect)
        list->addItem(new tsl::elm::CategoryHeader(txt::CONTROLS));
        // (Mute, Deafen, Screenshare, Call Volume, Game Volume, Settings, Disconnect; X and Y mute and deafen
        // from anywhere on the page)
        mute_ = new PickToggle(txt::MUTE_X, g_now.st.self_mute != 0, 3, &g_pick_call);
        mute_->setStateChangedListener([](bool on) { client::setSelfMute(on); });
        list->addItem(mute_);
        deaf_ = new PickToggle(txt::DEAFEN_Y, g_now.st.self_deaf != 0, 2, &g_pick_call);
        deaf_->setStateChangedListener([](bool on) { client::setSelfDeaf(on); });
        list->addItem(deaf_);
        // (a Go Live stream of the game, picture and sound)
        share_ = new PickToggle(txt::SHARE_SCREEN, g_now.st.screen_share != 0, 5, &g_pick_call);
        share_->setStateChangedListener([](bool on) { client::setScreenShare(on); });
        list->addItem(share_);
        addVolume(list, 4, &g_pick_call);
        addGameVolume(list, 7, &g_pick_call);
        auto* settings = new PickItem(txt::SETTINGS, txt::CARET, 6, &g_pick_call);
        settings->setClickListener([](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            tsl::changeTo<SettingsGui>();
            return true;
        });
        list->addItem(settings);
        auto* leave = new PickItem(txt::LEAVE, "", 1, &g_pick_call);
        leave->setClickListener([](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            client::leave();  // (the page goes back to the main menu once Discord says the bot's out)
            return true;
        });
        list->addItem(leave);
        auto people = client::members(g_now.st.call_channel);
        byName(people);
        seen_ns_ = g_now.polled_ns;
        rows_.clear();
        list->addItem(new tsl::elm::CategoryHeader(fmt(txt::ACTIVE_MEMBERS, (unsigned)people.size())));
        for (const ChirpMember& m : people) {
            // (someone else: their page, with our mute and volume for them)
            std::function<void()> open;
            if (!(m.flags & ChirpMember_Me)) open = [m]() { tsl::changeTo<UserGui>(m); };
            auto* row = new MemberRow(m, &g_pick_call, open);
            rows_.push_back(row);
            list->addItem(row, MemberRow::HEIGHT);
        }
        list->jumpToItem(jumpKey(g_pick_call ? g_pick_call : 3));  // (Mute, the first time)
        frame_->setContent(list);
        return frame_;
    }

    void update() override {
        poll();
        if (!inCall()) {
            tsl::swapTo<MainGui>();
            return;
        }
        if (g_now.st.serial != serial_) {
            tsl::swapTo<CallGui>();
            return;
        }
        std::string sub = connectedFor();  // (every frame: the clock ticks smoothly)
        if (sub != frame_->m_subtitle) frame_->m_subtitle = sub;
        // (Discord has the last word on mute and deafen: the toggles follow it)
        if (mute_->getState() != (g_now.st.self_mute != 0)) mute_->setState(g_now.st.self_mute != 0);
        if (deaf_->getState() != (g_now.st.self_deaf != 0)) deaf_->setState(g_now.st.self_deaf != 0);
        if (share_->getState() != (g_now.st.screen_share != 0)) share_->setState(g_now.st.screen_share != 0);
        // (the rings: who's talking, as often as the status is asked for)
        if (g_now.polled_ns == seen_ns_) return;
        seen_ns_ = g_now.polled_ns;
        auto people = client::members(g_now.st.call_channel);
        for (MemberRow* row : rows_) {
            bool on = false;
            for (const ChirpMember& m : people)
                if (m.user_id == row->user()) on = (m.flags & ChirpMember_Speaking) != 0;
            row->setSpeaking(on);
        }
    }

    // X mutes, Y deafens, wherever the cursor is on the page
    bool handleInput(u64 keysDown, u64, const HidTouchState&, HidAnalogStickState, HidAnalogStickState) override {
        if (keysDown & HidNpadButton_X) {
            bool on = !mute_->getState();
            mute_->setState(on);
            client::setSelfMute(on);
            return true;
        }
        if (keysDown & HidNpadButton_Y) {
            bool on = !deaf_->getState();
            deaf_->setState(on);
            client::setSelfDeaf(on);
            return true;
        }
        return false;
    }

private:
    u32 serial_ = 0;
    u64 seen_ns_ = 0;
    std::vector<MemberRow*> rows_;  // (owned by the list)
    tsl::elm::OverlayFrame* frame_ = nullptr;
    tsl::elm::ToggleListItem* mute_ = nullptr;
    tsl::elm::ToggleListItem* deaf_ = nullptr;
    tsl::elm::ToggleListItem* share_ = nullptr;
};

// --- someone in the call: our mute and volume for them (kept in people.ini), and how they sound ---

// How loud they are, for Debug: level is ChirpMember.level
std::string loudness(u8 level) {
    if (level == CHIRP_SILENT) return "-inf dB";
    return "-" + std::to_string(level) + " dB";
}

class UserGui : public tsl::Gui {
public:
    explicit UserGui(const ChirpMember& m) : m_(m) {}
    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(txt::TITLE, fmt(txt::MANAGE, m_.name));
        auto* list = new tsl::elm::List();
        u64 id = m_.user_id;

        list->addItem(new tsl::elm::CategoryHeader(txt::CONTROLS));
        auto* mute = new tsl::elm::ToggleListItem(txt::MUTE, (m_.flags & ChirpMember_LocalMute) != 0);
        mute->setStateChangedListener([id](bool on) { client::setUserMute(id, on); });
        list->addItem(mute);
        auto* volume = new Label(txt::VOLUME, std::to_string(m_.volume) + "%");
        list->addItem(volume);
        // (0..200% in steps of 5%, as addVolume's)
        auto* bar = new tsl::elm::StepTrackBar("", VOLUME_STEPS);
        bar->setProgress((u16)(m_.volume / 5));
        bar->setValueChangedListener([id, volume](u16 step) {
            int v = step * 5;
            client::setUserVolume(id, v);
            volume->setValue(std::to_string(v) + "%");
        });
        list->addItem(bar);

        list->addItem(new tsl::elm::CategoryHeader(txt::DEBUG));
        char idText[24];
        snprintf(idText, sizeof(idText), "%llu", (unsigned long long)m_.user_id);
        infoRow(list, txt::DEBUG_ID, idText);
        speaking_ = infoRow(list, txt::DEBUG_SPEAKING, (m_.flags & ChirpMember_Speaking) ? txt::YES : txt::NO);
        level_ = infoRow(list, txt::DEBUG_LEVEL, loudness(m_.level));
        frame->setContent(list);
        return frame;
    }

    void update() override {
        poll();
        if (!inCall()) {
            tsl::goBack();
            return;
        }
        // (Debug as it is now, as often as the status is asked for)
        if (g_now.polled_ns == seen_ns_) return;
        seen_ns_ = g_now.polled_ns;
        for (const ChirpMember& m : client::members(g_now.st.call_channel)) {
            if (m.user_id != m_.user_id) continue;
            speaking_->setValue((m.flags & ChirpMember_Speaking) ? txt::YES : txt::NO, true);
            level_->setValue(loudness(m.level), true);
        }
    }

private:
    ChirpMember m_;
    tsl::elm::ListItem* speaking_ = nullptr;
    tsl::elm::ListItem* level_ = nullptr;
    u64 seen_ns_ = 0;
};

bool GuildRow::onClick(u64 keys) {
    if (!(keys & HidNpadButton_A)) return false;
    tsl::changeTo<GuildGui>(g_.id, std::string(g_.name));
    return true;
}

// --- Profile: the volumes, how the bot shows, and which bot (token.ini's; picking one connects as it,
// the one it is too: that's how to try again after fixing token.ini) ---

u64 g_pick_profile = 0;

class ProfileGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(txt::PROFILE, txt::LOGINS_HINT);
        auto* list = new tsl::elm::List();
        addVolume(list, 2, &g_pick_profile);
        addGameVolume(list, 1, &g_pick_profile);
        list->addItem(new tsl::elm::CategoryHeader(txt::STATUS));
        auto* invisible = new PickToggle(txt::INVISIBLE, g_now.st.invisible != 0, 3, &g_pick_profile);
        invisible->setStateChangedListener([](bool on) {
            client::setInvisible(on);
            g_now.st.invisible = on;  // (until the next poll says so too)
        });
        list->addItem(invisible);
        auto* activity = new PickToggle(txt::GAME_ACTIVITY, g_now.st.rich_presence != 0, 4, &g_pick_profile);
        activity->setStateChangedListener([](bool on) {
            client::setRichPresence(on);
            g_now.st.rich_presence = on;
        });
        list->addItem(activity);
        list->addItem(new tsl::elm::CategoryHeader(txt::LOGINS));
        auto logins = client::logins();
        for (const ChirpLogin& l : logins) {
            bool current = !strcmp(l.name, g_now.st.login);
            auto* item = new tsl::elm::ListItem(l.name, current ? txt::ACTIVE : "");
            std::string name = l.name;
            item->setClickListener([name](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;
                client::setLogin(name.c_str());
                tsl::goBack();
                return true;
            });
            list->addItem(item);
        }
        if (logins.empty()) {
            auto* none = new tsl::elm::ListItem(txt::LOGINS_NONE);
            none->setClickListener([](u64) { return false; });
            list->addItem(none);
            auto* how = new tsl::elm::ListItem(txt::LOGINS_HOW);
            how->setClickListener([](u64) { return false; });
            list->addItem(how);
        }
        list->jumpToItem(jumpKey(g_pick_profile ? g_pick_profile : 2));  // (Call Volume, the first time)
        frame->setContent(list);
        return frame;
    }
    void update() override { poll(); }
};

bool BotRow::onA() {
    tsl::changeTo<ProfileGui>();
    return true;
}

void MainGui::update() {
    poll();
    if (inCall()) {
        tsl::swapTo<CallGui>();
        return;
    }
    u8 conn = g_now.running ? g_now.st.conn : 0xFF;
    if (conn != conn_ || (conn == ChirpConn_Online && g_now.st.serial != serial_)) tsl::swapTo<MainGui>();
}

class ChirpOverlay : public tsl::Overlay {
public:
    void initServices() override {}
    void exitServices() override { client::close(); }
    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        poll(true);
        if (inCall()) return initially<CallGui>();
        return initially<MainGui>();
    }
};

}  // namespace

int main(int argc, char** argv) { return tsl::loop<ChirpOverlay>(argc, argv); }
