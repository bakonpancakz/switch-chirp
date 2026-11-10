# The call's default sounds (sounds/*.wav), in the spirit of the old Skype: soft, like a bubble bath.
#   blub  - a round bubble: a pure sine that lifts a little in pitch as it pops, with a gentle attack
#   glow  - a soft note at the end, with a slightly detuned twin (a slow shimmer)
# then a small, quiet room and a low-pass, so nothing is sharp. Rising for a join, falling for a leave.
# (glow isn't used by the defaults now; it's there to add a note back.)
# voice_*: two watery bubbles (people joining the call, you too); screenshare_*: three, deeper and
# slower (people starting to watch your screen share, and stopping).
# 24 kHz mono 16-bit, as the sysmodule plays them (sounds.h).
#   python scripts/chimes.py          the bubbly ones
#   python scripts/chimes.py bells    the plain bell chimes from before
# Run from the repository's root.
import math
import struct
import sys
import wave

RATE = 24000


def write(name, buf, peak=0.5):
    top = max(abs(x) for x in buf) or 1.0
    scale = peak / top
    k = int(0.02 * RATE)  # (the last 20 ms faded out, so a tail never ends in a click)
    for i in range(k):
        buf[-k + i] *= (k - 1 - i) / k
    with wave.open(f"sounds/{name}.wav", "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, x * scale)) * 32767)) for x in buf))


# --- bubbly ---
def blub(buf, start, f, amp=1.0, lift=1.25, decay=28.0, swoop=0.009, wobble=0.0, wood=0.0):
    """A soft bubble: a sine from f to f*lift (below 1 drops instead), most of the way in `swoop`
    seconds, faded in over 8 ms (no click) and away over about a tenth of a second. wobble: the pitch
    trembles a little as it rings, like water settling; wood: a faint knock of a partial 3.9x up that
    dies at once (a marimba bar's)."""
    a0 = int(start * RATE)
    phase = phase_w = 0.0
    for i in range(min(int(0.25 * RATE), len(buf) - a0)):
        t = i / RATE
        env = math.sin(min(1.0, t / 0.008) * math.pi / 2) ** 2 * math.exp(-t * decay)
        fr = f * (1 + (lift - 1) * (1 - math.exp(-t / swoop)))
        fr *= 1 + wobble * math.sin(2 * math.pi * 22 * t) * min(1.0, t / 0.03)
        phase += 2 * math.pi * fr / RATE
        phase_w += 2 * math.pi * 3.9 * fr / RATE
        buf[a0 + i] += amp * env * (math.sin(phase) + wood * math.exp(-t * 90) * math.sin(phase_w))


def glow(buf, start, freqs, amp=0.4, decay=7.0):
    """A soft chord: each note and a twin 4 cents up (a slow shimmer), faded in over 15 ms."""
    a0 = int(start * RATE)
    for i in range(len(buf) - a0):
        t = i / RATE
        env = math.sin(min(1.0, t / 0.015) * math.pi / 2) ** 2 * math.exp(-t * decay)
        v = sum(math.sin(2 * math.pi * f * t) + 0.7 * math.sin(2 * math.pi * f * 1.0023 * t + 1.0) for f in freqs)
        buf[a0 + i] += amp * env * v / len(freqs)


def room(buf, mix=0.15):
    """A small, quiet Schroeder reverb: four combs into two all-passes."""
    def comb(x, ms, g):
        d = int(ms * RATE / 1000)
        y = [0.0] * len(x)
        for i in range(len(x)):
            y[i] = x[i] + (g * y[i - d] if i >= d else 0.0)
        return y

    def allpass(x, ms, g):
        d = int(ms * RATE / 1000)
        y = [0.0] * len(x)
        for i in range(len(x)):
            xd = x[i - d] if i >= d else 0.0
            yd = y[i - d] if i >= d else 0.0
            y[i] = -g * x[i] + xd + g * yd
        return y

    wet = [0.0] * len(buf)
    for ms, g in ((29.7, 0.7), (37.1, 0.68), (41.1, 0.66), (43.7, 0.64)):
        for i, v in enumerate(comb(buf, ms, g)):
            wet[i] += v / 4
    wet = allpass(allpass(wet, 5.0, 0.6), 1.7, 0.6)
    return [d + mix * w for d, w in zip(buf, wet)]


def soften(buf, hz=2500):
    """Two one-pole low-passes: takes any edge off."""
    a = 1 - math.exp(-2 * math.pi * hz / RATE)
    for _ in range(2):
        y = 0.0
        out = []
        for x in buf:
            y += a * (x - y)
            out.append(y)
        buf = out
    return buf


def bubbly():
    def make(name, total, peak, parts):
        buf = [0.0] * int(total * RATE)
        parts(buf)
        write(name, soften(room(buf)), peak)

    # Water, not a chime: each bubble swoops fast and far (a drop's "bloop"), trembles a touch and has
    # a whisper of wood; no held note at the end.
    wet = dict(swoop=0.005, wobble=0.025, wood=0.12)

    def share_join(b):
        blub(b, 0.00, 250, 0.85, lift=1.55, decay=26, **wet)
        blub(b, 0.08, 330, 0.9, lift=1.55, decay=24, **wet)
        blub(b, 0.16, 440, 0.95, lift=1.45, decay=16, **wet)

    def share_leave(b):
        blub(b, 0.00, 680, 0.95, lift=0.68, decay=26, **wet)
        blub(b, 0.08, 520, 0.9, lift=0.68, decay=24, **wet)
        blub(b, 0.16, 390, 0.85, lift=0.7, decay=16, **wet)

    def voice_join(b):
        blub(b, 0.00, 480, 0.8, lift=1.5, decay=32, **wet)
        blub(b, 0.07, 640, 0.9, lift=1.5, decay=26, **wet)

    def voice_leave(b):
        blub(b, 0.00, 960, 0.9, lift=0.68, decay=32, **wet)
        blub(b, 0.07, 720, 0.8, lift=0.68, decay=26, **wet)

    make("voice_join", 0.5, 0.45, voice_join)
    make("voice_leave", 0.5, 0.45, voice_leave)
    make("screenshare_join", 0.75, 0.45, share_join)
    make("screenshare_leave", 0.75, 0.45, share_leave)


# --- bells (the first defaults) ---
def bells():
    def note(freq, start, length, amp, buf):
        a0 = int(start * RATE)
        for i in range(int(length * RATE)):
            t = i / RATE
            env = min(1.0, t / 0.006) * math.exp(-t * 9.0)
            v = math.sin(2 * math.pi * freq * t) + 0.25 * math.sin(4 * math.pi * freq * t) + 0.08 * math.sin(6 * math.pi * freq * t)
            if a0 + i < len(buf):
                buf[a0 + i] += amp * env * v

    def chime(name, notes, total, peak):
        buf = [0.0] * int(total * RATE)
        for freq, start in notes:
            note(freq, start, total - start, 1.0, buf)
        write(name, buf, peak)

    E5, A5 = 659.25, 880.0
    C6, E6, G6 = 1046.5, 1318.51, 1567.98
    chime("voice_join", [(E5, 0.0), (A5, 0.11)], 0.55, 0.45)
    chime("voice_leave", [(A5, 0.0), (E5, 0.11)], 0.55, 0.45)
    chime("screenshare_join", [(C6, 0.0), (E6, 0.06), (G6, 0.12)], 0.50, 0.30)
    chime("screenshare_leave", [(G6, 0.0), (E6, 0.06), (C6, 0.12)], 0.50, 0.30)


(bells if sys.argv[1:] == ["bells"] else bubbly)()
