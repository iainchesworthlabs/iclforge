"""What the karaoke examples' generators share: arithmetic, voices and WAV files.

tools/generators/gen_karaoke_example.py ("Auld Lang Syne") and gen_karaoke_duet_example.py
("Leave Her, Johnny") each hold a score and a way of laying it out; the synthesis under both is
here, so that the two examples sound as one family and a fix to a voice is made once.

Reproducible to the byte, on any platform: the sample path uses only + - * / and comparisons on
IEEE doubles, which Python does the same everywhere. The C library's sin, cos, exp and pow are not
used, because their last bit is not the same on every C library. Moving code here changed none of
it: the first example's committed files are held to its generator by a unit test.

Stdlib only.
"""

from __future__ import annotations

import array
import struct
import sys

RATE = 48_000
FRAME = 1536  # samples in one AC-3 frame


# ---  -------------------------------------------------------------------------------------------
def sin_turns(x: float) -> float:
    """sin(2 pi x), by Taylor series after reducing to a quarter turn. + - * / only."""
    x -= int(x)  # (-1, 1)
    if x > 0.5:
        x -= 1.0
    elif x < -0.5:
        x += 1.0
    if x > 0.25:
        x = 0.5 - x
    elif x < -0.25:
        x = -0.5 - x
    t = x * 6.283185307179586
    t2 = t * t
    term = t
    total = t
    for k in range(1, 12):
        term = -term * t2 / ((2 * k) * (2 * k + 1))
        total += term
    return total


def cos_turns(x: float) -> float:
    return sin_turns(x + 0.25)


def exp_neg(x: float) -> float:
    """exp(-x) for x >= 0, by halving until the series is short, then squaring back."""
    halvings = 0
    while x > 0.0625:
        x *= 0.5
        halvings += 1
    term = 1.0
    total = 1.0
    for k in range(1, 14):
        term = term * -x / k
        total += term
    for _ in range(halvings):
        total *= total
    return total


def _ipow(base: float, exponent: int) -> float:
    result = 1.0
    while exponent > 0:
        if exponent & 1:
            result *= base
        base *= base
        exponent >>= 1
    return result


def _root(target: float, n: int) -> float:
    """The n-th root of target (target in (0, 2]), by bisection on exact multiplications."""
    lo, hi = 0.0, 2.0
    for _ in range(100):
        mid = (lo + hi) * 0.5
        if _ipow(mid, n) < target:
            lo = mid
        else:
            hi = mid
    return (lo + hi) * 0.5


_SEMITONE = _root(2.0, 12)


def hz(midi: int) -> float:
    """Equal temperament, A4 = 440 Hz = midi 69."""
    f = 440.0
    for _ in range(abs(midi - 69)):
        f = f * _SEMITONE if midi > 69 else f / _SEMITONE
    return f


def half_life(samples: int) -> float:
    """The per-sample factor that halves a level every `samples` samples."""
    return _root(0.5, samples)


# ---  -------------------------------------------------------------------------------------------
def add_partial(buf, start, count, freq, amp, decay=1.0, attack=0, release=0):
    """Add amp * sin(2 pi freq t) * decay**t to buf[start:start + count].

    The sine is the two-term recurrence y[n + 1] = 2 cos(w) y[n] - y[n - 1], three operations a
    sample. The note starts from a zero crossing and rises over `attack` samples; its last
    `release` samples fall linearly to nothing."""
    count = min(count, len(buf) - start)
    if count <= 0:
        return
    w = freq / RATE
    k = 2.0 * cos_turns(w)
    y0 = 0.0
    y1 = sin_turns(w)
    level = amp
    attack = min(attack, count)
    release = min(release, count - attack)
    i = start
    for j in range(attack):
        buf[i] += level * y0 * ((j + 1) / attack)
        y2 = k * y1 - y0
        y0 = y1
        y1 = y2
        level *= decay
        i += 1
    for _ in range(count - attack - release):
        buf[i] += level * y0
        y2 = k * y1 - y0
        y0 = y1
        y1 = y2
        level *= decay
        i += 1
    for j in range(release):
        buf[i] += level * y0 * (1.0 - (j + 1) / release)
        y2 = k * y1 - y0
        y0 = y1
        y1 = y2
        level *= decay
        i += 1


# centre Hz, bandwidth Hz, weight: the three formants of each vowel.
VOWELS = {
    "ah": ((700.0, 80.0, 1.0), (1150.0, 100.0, 0.55), (2650.0, 140.0, 0.25)),
    "oo": ((320.0, 70.0, 1.0), (800.0, 90.0, 0.18), (2250.0, 130.0, 0.05)),
}


def resonator(centre, bandwidth, weight):
    """(gain, a1, a2) of y[n] = gain x[n] + a1 y[n-1] + a2 y[n-2], a two-pole resonance."""
    r = exp_neg(3.141592653589793 * bandwidth / RATE)
    theta = centre / RATE
    # The gain that makes the peak of the response about `weight`, whatever the centre.
    return weight * (1.0 - r) * 2.0 * sin_turns(theta), 2.0 * r * cos_turns(theta), -r * r


def render_voice(syllables, total, vowel, vibrato, breath, seed):
    """A sung voice: a smooth pulse train through three formant resonators in parallel.

    `syllables` holds, for each syllable, its notes as (midi, samples); a note whose midi is None
    is a rest, which makes no sound and leaves the voice where it was. The gate rises over 25 ms
    at each syllable and falls over the 60 ms before the next, so the words are separate; a second
    note on a syllable only moves the pitch. The pitch glides to each note, the vibrato is a slow
    sine on it, and the breath is integer pseudo-random noise added to the pulses. `total` is the
    length of the part in samples, which the syllables may not reach the end of."""
    n = total
    env = [0.0] * n
    target = [0.0] * n
    rise = 1200
    fall = 2880
    at = 0
    held = 0.0
    for notes in syllables:
        length = sum(count for _, count in notes)
        if notes[0][0] is not None:
            env[at:at + rise] = [j / rise for j in range(rise)]
            env[at + rise:at + length - fall] = [1.0] * (length - fall - rise)
            env[at + length - fall:at + length] = [1.0 - j / fall for j in range(fall)]
        position = at
        for midi, count in notes:
            if midi is not None:
                held = hz(midi)
            target[position:position + count] = [held] * count
            position += count
        at += length

    (g1, a11, a12), (g2, a21, a22), (g3, a31, a32) = (resonator(*v) for v in VOWELS[vowel])
    y11 = y12 = y21 = y22 = y31 = y32 = 0.0
    out = [0.0] * n
    inv_rate = 1.0 / RATE
    glide = 0.004
    lfo_k = 2.0 * cos_turns(5.4 / RATE)
    lfo0, lfo1 = 0.0, sin_turns(5.4 / RATE)
    phase = 0.0
    freq = 0.0
    state = seed
    mean = 1.0 / 6.0
    for i in range(at):
        lfo2 = lfo_k * lfo1 - lfo0
        lfo0 = lfo1
        lfo1 = lfo2
        t = target[i]
        freq = t if freq == 0.0 else freq + (t - freq) * glide
        phase += freq * (1.0 + vibrato * lfo1) * inv_rate
        if phase >= 1.0:
            phase -= 1.0
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        x = phase * (1.0 - phase) - mean + breath * ((state >> 8) / 4194304.0 - 1.0) * 0.25
        a = x * g1 + a11 * y11 + a12 * y12
        y12 = y11
        y11 = a
        b = x * g2 + a21 * y21 + a22 * y22
        y22 = y21
        y21 = b
        c = x * g3 + a31 * y31 + a32 * y32
        y32 = y31
        y31 = c
        out[i] = (a + b + c) * env[i]
    return [out]


# ---  -------------------------------------------------------------------------------------------
def to_int16(x):
    v = int(x * 32767.0 + 0.5) if x >= 0.0 else int(x * 32767.0 - 0.5)
    return max(-32767, min(32767, v))


def scaled(parts, peaks):
    """Each part, {name: [channel, ...]}, brought to its own peak (of full scale), 16-bit."""
    out = {}
    for name, channels in parts.items():
        peak = max(abs(s) for channel in channels for s in channel)
        gain = peaks[name] / peak
        out[name] = [array.array("h", (to_int16(s * gain) for s in c)) for c in channels]
    return out


def wav_bytes(channels):
    count = len(channels)
    frames = len(channels[0])
    interleaved = array.array("h", bytes(2 * count * frames))
    for c, samples in enumerate(channels):
        interleaved[c::count] = samples
    if sys.byteorder == "big":
        interleaved.byteswap()
    data = interleaved.tobytes()
    block = 2 * count
    fmt = struct.pack("<HHIIHH", 1, count, RATE, RATE * block, block, 16)
    return (
        b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE"
        + b"fmt " + struct.pack("<I", len(fmt)) + fmt
        + b"data" + struct.pack("<I", len(data)) + data
    )


# ---  -------------------------------------------------------------------------------------------
def stamp(centiseconds, tag):
    """[mm:ss.xx] for a line, <mm:ss.xx> for a word (Hearth's enhanced LRC)."""
    minutes, rest = divmod(centiseconds, 6000)
    seconds, hundredths = divmod(rest, 100)
    text = f"{minutes:02d}:{seconds:02d}.{hundredths:02d}"
    return f"<{text}>" if tag == "word" else f"[{text}]"
