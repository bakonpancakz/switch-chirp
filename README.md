# Chirp

Discord voice chat on a homebrew Nintendo Switch, through a bot of your own. A
small sysmodule starts with the Switch and stays connected to Discord whatever
runs in front (a game, the HOME menu, Melody playing). An overlay in
Ultrahand's menu shows your servers and their voice channels, who's in them,
and puts the bot in a call: you hear the call, talk through the headset's mic,
and can share the game, picture and sound, as a Go Live stream.

## What it does

- **Home:** the bot's picture and name, and under them whether it's
  connected (or how long until it tries again: 3 s at most). A on it opens
  **Select Login**: **Appear Invisible** (on by default: the bot shows offline
  to everyone while Chirp stays connected, so it isn't online whenever the
  Switch is on), **Rich Presence** ("Playing <game>" and for how long, real
  games only; not shown while invisible), and the bots in `token.ini`. Then your servers, each
  with its icon and how many are in voice.
- **Voice channels:** in Discord's order, under their categories, each with
  how many are in it. A channel: Join Channel, Join Channel Muted, and who's in it.
- **In a call:** "Chirp: <channel>", then **Mute**, **Deafen**, **Screenshare**,
  **Volume** (everyone, 0–200% in steps of 5%), **Settings** (a page of its
  own) and **Leave Channel**. X mutes and Y deafens from anywhere on that page. Then the
  members, the bot first, each with their picture (a green ring while they
  talk) and their mute and deafen state.
- **Someone in the call:** Mute and Volume (0–200%), which only change what
  you hear, saved in `config/sys-chirp/people.ini`.
- **Settings:** the mic's **Input Volume** (0–200%), **Input Sensitivity**
  (Automatic, or -80 to -15 dB, with the mic's level shown live beside the
  line to pick it by), **Automatic Gain Control** and **Noise Suppression**, kept in
  `settings.ini`; then **Debug: Connection** (the voice servers, their pings,
  the call's elapsed time), **Debug: Sound** (people followed, decoders in
  use, the mic's gain) and **Debug: Memory**.
- **Hearing:** each person's Opus packets go through a small jitter buffer
  (20 ms cushion) to the Switch's hardware Opus decoders (eight people
  followed, four heard at once), mixed and played through audout (100 ms
  queued).
- **Talking:** the default mic (a headset's) at 48 kHz, the input volume,
  noise suppression (RNNoise v0.2, its full model, if on; 10 ms more delay), a 100 Hz high-pass for rumble, then the input sensitivity decides if it's
  talk: two 20 ms frames in a row over the line (so a click or a button press
  never opens the mic, and the first frame still goes out). Then automatic
  gain (if on) and Chirp's own Opus encoder (64 kbps). It only sends while
  you talk.
- **Screenshare:** a Go Live stream of what the Switch records (grc:d, the
  service SysDVR uses): 1280x720 H.264 at 30 frames a second, and the game's
  sound (mono Opus, 96 kbps), on its own connection. When there's no picture
  (no game, the HOME menu), viewers see a loading screen (below), or grey.
- **Encryption:** Discord's end-to-end encryption (DAVE, on MLS) for sound
  and video, and the transport's AES-256-GCM.

Names: someone's server nickname, else their display name, else their
username.

## What you need

- Atmosphère, and [Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay)
- The Switch on Wi-Fi
- A Discord bot (below). It's free and takes a few minutes.

## Making the bot

1. Go to the [Discord Developer Portal](https://discord.com/developers/applications)
   and press **New Application**. Its name is the bot's name in calls.
2. **Bot** tab: **Reset Token**, then copy the token. Keep it secret: it's the
   bot's password. No privileged intents are needed (Chirp only asks for
   Guilds and Guild Voice States).
3. **OAuth2** tab, **URL Generator**: tick the `bot` scope, then the permissions
   **View Channels**, **Connect**, **Speak** and **Video** (for Screenshare).
   Open the URL it makes and add the bot to your servers (you need Manage
   Server on each).

## Installing

1. Build it (see below), or take the files from a release.
2. Copy everything in `dist/` to the root of the SD card:

   | On the card | What it is |
   | --- | --- |
   | `atmosphere/contents/420000000000C4A7/` | the sysmodule (`exefs.nsp`, `flags/boot2.flag`, `toolbox.json`) |
   | `switch/.overlays/chirp.ovl` | the overlay |
   | `config/sys-chirp/token.example.txt` | where the token goes |
   | `config/sys-chirp/loading/*.h264` | loading screens for Screenshare (if you made any) |
   | `config/sys-chirp/sounds/*.wav` | the call's sounds (`voice_*`, `screenshare_*`) |

3. In `sdmc:/config/sys-chirp/`, make **`token.ini`** with a line for each bot:
   `My Bot=<token>`. The overlay's Select Login picks between them, and
   `settings.ini` remembers which. (Or **`token.txt`**, with just one token on
   its first line.)
4. Restart the Switch. Open Ultrahand and pick **Chirp**: the top row should
   show your bot, **Connected**.

The token only lives in `token.txt` on your SD card. Chirp's log
(`config/sys-chirp/chirp.log`) never has it. Lines are queued in memory and
written by a thread of their own a moment later, so nothing waits on the SD
card (the call's sound did, once). The file is opened and closed for each
batch, so FTP can copy it while Chirp runs. Settings and volumes are saved a
second after their last change.

## When something's wrong

The overlay's top line says what the sysmodule is doing, and the log says
more.

- **No bot token:** `token.txt` isn't there, or it's empty, or it holds
  something that can't be a token.
- **Discord turned the token down:** Discord closed the session with 4004.
  Copy the token again (Reset Token makes a new one), then **Connect Again**.
- **Offline:** no network, or the connection broke. Chirp starts a new session by
  itself, waiting 1, 2, then 3 seconds between tries.

When a session ends for any reason, everything is dropped and the next one
starts from scratch (no resuming).

## Loading screens

Put PNGs (or JPGs) in `loading/` before building. Each becomes one 1280x720
H.264 keyframe (about 100 KB) in `dist/config/sys-chirp/loading/`, made with
ffmpeg (on the PATH, or `$env:FFMPEG` in `toolchain.local.ps1`). While
sharing and the game shows nothing, Chirp sends one of them at random, again
every 2 s, a new pick each time. They're read from the SD card when needed,
so they cost no memory.

## Sounds

Chirp plays short sounds for what happens in the call. They're WAVs in
`config/sys-chirp/sounds/`:

| File | When |
| --- | --- |
| `voice_join.wav` | you join a call, or someone comes into your channel |
| `voice_leave.wav` | you leave it, or someone goes out of it |
| `screenshare_join.wav` | someone starts watching your screen share |
| `screenshare_leave.wav` | someone stops watching it |

The log also says how many are watching.

Swap any of them for your own: 16-bit PCM WAV, mono or stereo, 8 to 48 kHz.
They're mixed to mono and played at your volume, not while you're deafened.
Each is read from the card when it plays, into one 48 KB buffer: up to 1 s at
24 kHz, 0.5 s at 48 kHz. Anything longer is cut with a short fade. Delete one
to silence it. The ones that come with Chirp are in the spirit of the old
Skype, soft like a bubble bath: deep, watery "bloops" in a small room. They
rise for a join and fall for a leave, and are made by `scripts/chimes.py`.
The call's are two quick bubbles; the screen share's are three, deeper and
slower. `python scripts/chimes.py
bells` makes the plain bell chimes instead.

## How it's made

Everything the sysmodule keeps is in fixed tables and buffers. Nothing is
allocated as it runs, so its memory is known at build time
(`aarch64-none-elf-size chirp.elf`), about 5.8 MB in all. That needs more room than stock
Atmosphère gives sysmodules on firmware 20 to 22 (7 MB extra on 21+, shared by all of them; on
22.5.0, 1 MB was left in a game before v0.2). Firmware 23.0.0+ with Atmosphère 1.12.1 gives 40 MB.
On 22.x, raise `ExtraSystemMemoryForAtmosphere_21_0_0` in mesosphere's
`kern_k_system_control.cpp` (14 MB works; the browser applets get that much less):

| | |
| --- | --- |
| code and constants (RNNoise v0.2's weights are 3.2 MB of it) | ~3.6 MB |
| grc:d's buffer (its biggest keyframe) and the queue of frames being sent (enough for 8 Mbps) | 600 KB |
| two voice connections (the call's and the stream's): DAVE's MLS state and room for a Welcome each | ~380 KB |
| the lists (`store.cpp`), the mixer, the mic | ~200 KB |
| heap, for what libnx hands the system (the sockets' buffers, 87 KB; four hardware Opus decoders, 48 KB each) and the threads' stacks | 608 KB |

- **TLS** is the Switch's own ssl service, so there's no TLS library in here.
- **The websocket** (`ws.cpp`), **JSON** (`json.cpp`, read as it streams in,
  a value at a time with its path) and **the gateway** (`gateway.cpp`) are
  written by hand.
- **The voice connection** (`voice.cpp`): voice gateway v8, UDP with IP
  discovery, RTP with AES-256-GCM (`rtp.cpp`). Go Live is a second one, to the
  stream's own server.
- **Screenshare** comes in three parts.
  - **Capture** (`screen.cpp`) reads grc:d. Video is asked for only while a
    title runs that allows video capture: a game, or sphaira and whatever
    runs in it. It also waits a couple of seconds after the title opens. A
    request made while nothing records is never answered, and was stuck
    until a restart.
  - **The send queue** (`framequeue.cpp`) and **the recorder's clocks**
    (`screenclock.cpp`) sit between capture and sending. The clocks track
    frames missed and holes in the sound.
  - **What's sent** (`share.cpp`) decides frame by frame: the game's frames,
    waits for a keyframe after a loss, and loading screens or grey when
    there's no picture. `voice.cpp` only encrypts and sends what it's given.
    The queue, clocks and decisions are tested on a PC with stand-ins.
- **DAVE** (`dave.cpp`, `mlsgroup.cpp`, `ratchet.cpp`, `p256.cpp`...): MLS
  1.0 with P-256, written out and checked against another MLS library (ts-mls) and the
  MLS test vectors. Video frames are encrypted as libdave does, the H.264
  headers left readable.
- **The Opus encoder** (`opusenc.cpp`): CELT only, mono, fixed size per
  packet, checked against libopus bit for bit.
- **H.264** (`rtp.cpp`): packetized by hand (FU-A); the recorder's SPS and
  PPS put before every keyframe (it only sends them when a game starts), its
  SPS given the bitstream restriction WebRTC would otherwise rewrite (and
  break DAVE's tag over); the grey frame is written macroblock by macroblock.

## Building

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with
`switch-dev` installed. On Windows:

```
./build.ps1           # the sysmodule and the overlay, laid out in dist/
./build.ps1 sysmodule # only the sysmodule
./build.ps1 clean
```

## Tests

`test/run.sh` builds the PC tests with a native gcc and runs them:

```
PATH=/c/msys64/ucrt64/bin:$PATH sh test/run.sh
```

They cover the JSON reader, the websocket and gateway against a pretend
Discord (in random-sized pieces), the store, the mixer and jitter buffer, the
crypto (AES-GCM, SHA-2, HKDF, P-256, HPKE), MLS against its test
vectors and ts-mls, DAVE frames (Opus and H.264), the Opus encoder against
libopus, RNNoise against xiph's, and the H.264 packetizer, SPS fix and grey
frame (`test/check_grey.py` reads the grey frame back as a decoder would).

## What it can't do

- **DM calls.** Bots can't join them.
- **More than four people talking at once** are heard four at a time (the
  hardware decoders, 48 KB each); a ninth person isn't followed until one of
  the eight goes quiet for 5 s.
- **Lost video packets aren't sent again**: one lost on the way breaks the
  picture until the next keyframe (two a second).
- **Go Live as a bot** may need Discord to allow it for your bot.
