"""Generates the Dolby Encoding Engine's AC-3, E-AC-3, E-AC-3 JOC and TrueHD golden masters: a
local set, never committed, beside the AC-4 gold set.

Why. DEE's licence here ends on 2026-11-06 and is not renewed, and after that date no DEE stream
of any kind can be made. What the tree takes from DEE today is small: the eight legs of
gen_external_baseline.py (testdata/external-baseline/, six of them DEE streams at 2.0 and 5.1),
which quality_race.py's trend and verify_gold_reference.sh's third-party legs read, and the one
DD+ JOC fixture of gen_object_fixture.py (testdata/object-fixture/dee_joc_514.ec3), which the
object layer's tests, Hearth and the ESP32 sink runs play. This set makes, while DEE runs, every
stream a later piece of work could want from it: each layout and data rate the AC-3, E-AC-3,
Blu-ray E-AC-3, E-AC-3 JOC and TrueHD encoders allow, their metadata, 60 s and 300 s programmes,
and the refusals, each from sources rebuilt from committed material.

Groups (G2_GROUPS names what each is for):

  group          encoder             what
  eac3-rates     dee_ddp_encoder     DD+ mono (32-1024 kbps), 2.0 (96-1024) and 5.1 (192-1024) at
                                     every rate DEE lists, from music, speech or film, tones, and
                                     2.0 and 5.1 sweeps, noise and transients; 5.1 without its
                                     LFE; the hybrid downmix's 5.1 at 64, 96, 128 and 160
  ac3-rates      dee_ddp_encoder     DD mono, 2.0 and 5.1 at every rate DEE lists, the same sources
  bluray         dee_ddp_encoder     the undocumented 'bluray' mode: 7.1 as an AC-3 core and an
                                     E-AC-3 dependent substream, at 768, 1024, 1280, 1536 and 1664
  seven-one      dee_ddp_encoder     7.1 input to DD+ and DD, which DEE writes as 5.1
  eac3-metadata  dee_ddp_encoder     DD+: stepped tones under each line and RF DRC profile, each
                                     preferred downmix, every mix level on one tone per channel,
                                     each bitstream mode, Dolby Surround and Surround EX, the LFE
                                     off, the surround attenuation, DEE's production defaults,
                                     loudness presets and corrected targets from -31 to -8,
                                     dialogue intelligence and the speech threshold, user data
  ac3-metadata   dee_ddp_encoder     the same for DD, and its embedded timecode at four rates
  joc            dee_ddpjoc_encoder  DD+ JOC from 5.1.4, 7.1.4 and 9.1.6 beds at every rate, every
                                     5.1 height and surround trim, the downmixes and mix levels,
                                     DRC, music mode, loudness presets, gapless parts and cut
                                     points
  truehd         dee_dthd_encoder    TrueHD 2ch, 6ch and 8ch at 48 and 96 kHz, 16 and 24 bits, from
                                     tones, music, film, noise and sweeps; downmix and independent
                                     presentations, each 2ch format, DRC per presentation, the
                                     surround attenuation, dialnorm and loudness presets, embedded
                                     timecode, and --optimize-data-rate
  truehd-open    dee_dthd_encoder    the TrueHD gold set of 2026-10-29 (GOLD_VERSION 2): the rates
                                     above 96 kHz DEE was asked for, downmix presentations with
                                     three substreams, DRC per presentation and dialnorm on 2.0
                                     and 7.1, lengths that are not a whole number of access
                                     units, 20-bit and other bit depths, other channel counts,
                                     --optimize-data-rate, --add-silence and the trim options,
                                     and the Atmos masters named with --atmos-master
  truehd-fixtures dee_dthd_encoder   0.25 s TrueHD streams, committed under testdata/truehd-dee/
                                     (--fixtures-out)
  truehd-long    dee_dthd_encoder    300 s TrueHD programmes (2.0, 5.1, 7.1), made in a second set
                                     directory
  programme      all four            60 s programmes at the common configurations, and 300 s ones
                                     for soak tests
  objects        all four            the Atmos-master inputs with --masters (the ADM BWF masters of
                                     the AC-4 gold set's G1): refused, as there

Every AC-3 and E-AC-3 leg reads its source as a wav_list of mono WAVs, the path
gen_external_baseline.py found keeps every channel (the single multichannel WAV loses Ls on this
DEE build); the JOC legs read cbi_wav and the TrueHD legs a single WAV, both of which keep them. The
scoring legs switch off DEE's audio-altering defaults (loudness measured only, no 90-degree
surround phase shift, no LFE low-pass), as gen_external_baseline.py does; eac3-metadata and
ac3-metadata keep one leg with those defaults on. Legs whose options change the audio say so in
audio_altered.

Sources (g2_source_specs() describes each), rebuilt from the committed programme fixtures and
synthesised: 10 s unless named otherwise, 24-bit, at 48 kHz, and at 96 kHz for TrueHD (tones and
noise made at 96 kHz, music up-sampled by two); _16 copies rounded to 16 bits. One tone per
speaker (SPEAKER_HZ, the AC-4 gold set's frequencies), music beds cut from the music fixture,
film with the speech fixture in C, sweeps, pink noise from a splitmix64 counter, silence then
transients, stepped tones for DRC, and programmes of music with speech ducking it.

Each leg, in DIR/streams/<leg>, keeps DEE's command and log, the stream, MediaInfo's trace (for
legs up to 60 s, --Details=1 --ParseSpeed=1) and its summary, DEE's MP4 of every AC-3 and E-AC-3
stream, and leg.json, which lets a rerun reuse a stream whose command and sources are unchanged.
DIR/dee-gold-manifest.json holds each leg's group, what it is for, the command, the SHA-256s,
DEE's warnings and loudness measurement, forge probe's reading (substreams, metadata, the tools
each stream uses), what that forge's decode and FFmpeg's make of the stream, and for TrueHD
whether FFmpeg's decode equals the source sample for sample. A refused leg keeps DEE's message.

TrueHD. The roadmap keeps "TrueHD interoperability by black-box analysis of Dolby streams" out of
scope until the clean-room rule allows it. On 2026-09-26 the user decided these streams are test
inputs only: decoded, passed through and round-tripped, with no analysis of how Dolby's encoder
made them. Each TrueHD entry says so (TRUEHD_NOTE).

What DEE 6.5.4 could not be made to write is in the manifest's dee_cannot (DEE_CANNOT).

Usage (repo root):
  python tools/generators/gen_dee_gold.py --gold-set D:/ac3bld/dee-gold [--cli ICLFORGE_CLI]
      [--jobs N] [--only NAME_PART ...] [--masters DIR] [--atmos-master NAME=PATH ...]
  python tools/generators/gen_dee_gold.py --gold-set D:/ac3bld/dee-gold
      --fixtures-out testdata/truehd-dee       # no DEE run: the committed fixtures' copy

Never run in CI - see guard_not_ci(), the same rule gen_external_baseline.py states.
"""

import argparse
import getpass
import hashlib
import json
import os
import platform
import re
import struct
import subprocess
import tempfile
import time
import wave
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
AUDIO = REPO / "testdata" / "audio"

DEE_DIR = Path(r"C:\Program Files\Dolby\Dolby Media Encoder\resources\dee-dir")
MEDIAINFO = DEE_DIR / "MediaInfo.exe"
MP4MUXER = DEE_DIR / "dee_mp4muxer.exe"
DDP = "dee_ddp_encoder"
DDPJOC = "dee_ddpjoc_encoder"
DTHD = "dee_dthd_encoder"
ENCODERS = (DDP, DDPJOC, DTHD)

# 1: the first set, 2026-09-26.
# 2: the TrueHD gold set of 2026-10-29: truehd-open, truehd-fixtures and truehd-long beside
# the first set's legs, which are unchanged, and the manifest's strict FFmpeg decode, forge's TrueHD
# probe and its per-presentation decodes.
GOLD_VERSION = 2
MANIFEST = "dee-gold-manifest.json"
DEFAULT_MASTERS = Path("D:/ac3bld/ac4-gold/masters")

RATE = 48000
SECONDS = 10.0
PROGRAMME_SECONDS = 60.0
SOAK_SECONDS = 300.0
MEASURE_ONLY = ["--loudness-management", "measure_only"]
# DEE's audio-altering defaults off, as gen_external_baseline.py's _DEE_ENCODER_OPTS (which also
# sets drc_profile=none; here DRC is left at DEE's default, film_light, which is metadata).
QUIET = "surround_90deg_phase_shift=0:lfe_filter=0"

# One tone per speaker, the AC-4 gold set's frequencies (gen_ac4_baseline.py's TONE_HZ and G1's
# SPEAKER_HZ): primes, so no tone sits on another's harmonic, and the LFE's inside its band.
SPEAKER_HZ = {"L": 331, "R": 457, "C": 613, "LFE": 47, "Ls": 787, "Rs": 953, "Tfl": 1117,
              "Tfr": 1289, "Tbl": 1453, "Tbr": 1621, "Lrs": 1777, "Rrs": 1933, "Lw": 2089,
              "Rw": 2243, "Ltm": 2399, "Rtm": 2551}
TONE_AMPLITUDE = 0.1

# Each layout's speakers in the order DEE's inputs take them: wav_list's L:R:C:LFE:LS:RS:LRS:RRS
# (a mono wav_list is its one channel), and the cbi_wav orders of the Dolby Media Encoder user
# guide (5.1.4's Lfh Rfh Lrh Rrh are Tfl Tfr Tbl Tbr here).
SPEAKERS = {
    "10": ("C",),
    "20": ("L", "R"),
    "51": ("L", "R", "C", "LFE", "Ls", "Rs"),
    "71": ("L", "R", "C", "LFE", "Ls", "Rs", "Lrs", "Rrs"),
    "30": ("L", "R", "C"),
    "31": ("L", "R", "C", "LFE"),
    "40": ("L", "R", "Ls", "Rs"),
    "50": ("L", "R", "C", "Ls", "Rs"),
    "70": ("L", "R", "C", "Ls", "Rs", "Lrs", "Rrs"),
    "514": ("L", "R", "C", "LFE", "Ls", "Rs", "Tfl", "Tfr", "Tbl", "Tbr"),
    "714": ("L", "R", "C", "LFE", "Ls", "Rs", "Lrs", "Rrs", "Tfl", "Tfr", "Tbl", "Tbr"),
    "916": ("L", "R", "C", "LFE", "Ls", "Rs", "Lrs", "Rrs", "Lw", "Rw", "Tfl", "Tfr", "Ltm",
            "Rtm", "Tbl", "Tbr"),
}
ALL_SPEAKERS = SPEAKERS["916"]
NOISE_RMS = 0.05
STEP_HZ = 997
STEP_LEVELS_DB = tuple(range(-70, 0, 3))
STEP_SECONDS = 1.5
PROGRAMME_DIALOGUE = ((20.0, 0.0, 20.0), (45.0, 20.0, 10.0))

# dee_ddp_encoder --morehelp data-rate's lists. DD's names 272, which is not an AC-3 rate; it is
# asked for like the rest, and DEE's answer recorded.
DDP_RATES = (32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 128, 144, 160, 176, 192, 200,
             208, 216, 224, 232, 240, 248, 256, 272, 288, 304, 320, 336, 352, 368, 384, 400, 448,
             512, 576, 640, 704, 768, 832, 896, 960, 1008, 1024)
DD_RATES = (96, 112, 128, 160, 192, 224, 256, 272, 320, 384, 448, 512, 576, 640)
DDP_FLOOR = {"10": 32, "20": 96, "51": 192}
DD_FLOOR = {"10": 96, "20": 96, "51": 224}
HYBRID_RATES = (64, 96, 128, 160)
# The 'bluray' mode's rates, found by asking for every multiple of 64 from 448 to 1792.
BLURAY_RATES = (768, 1024, 1280, 1536, 1664)
JOC_RATES = (384, 448, 576, 640, 768, 1024)
DRC_PROFILES = ("film_light", "film_standard", "music_light", "music_standard", "speech", "none")
DMX_MODES = ("loro", "ltrt", "ltrt-pl2", "not_indicated")
MIX_LEVELS = {"loro_cmix": ("+3", "+1.5", "0", "-1.5", "-4.5", "-6", "-inf"),
              "loro_smix": ("-1.5", "-4.5", "-6", "-inf"),
              "ltrt_cmix": ("+3", "+1.5", "0", "-1.5", "-4.5", "-6", "-inf"),
              "ltrt_smix": ("-1.5", "-4.5", "-6", "-inf")}
BITSTREAM_MODES = ("music_and_effects", "visually_impaired", "hearing_impaired", "dialogue",
                   "commentary", "emergency")
LOUDNESS_PRESETS = ("atsc_a85", "ebu_r128", "freetv_op59", "arib_b32")
DDP_TARGETS = (-31, -27, -24, -20, -17, -14, -10, -8)
HEIGHT_TRIMS = ("-3", "-6", "-9", "-12")
SURROUND_TRIMS = ("0", "-3", "-6", "-9")
THD_PRESETS = ("atsc_a85", "atsc_a85_agile", "ebu_r128", "freetv_op59", "arib_b32")
THD_DIALNORMS = (-31, -27, -24, -20, -17, -10, -1)
TIMECODE_RATES = ("24", "25", "29.97", "30")

# The rates the TrueHD block C probes (tag: Hz); only the ones DEE accepted get the longer legs.
HIGH_RATES = {"44k": 44100, "88k": 88200, "176k": 176400, "192k": 192000}
# All four probes were refused on 2026-10-29, so none of the longer legs at these rates is made;
# a rate DEE accepts later goes here to make them.
HIGH_RATES_ACCEPTED = ()
HOT_HZ = 997
HOT_AMPLITUDE = 0.5
# Sample counts that are not a whole number of access units (40 at 48 kHz): block F.
LENGTH_SAMPLES = (12000, 12001, 12039, 4001, 41)
FIXTURE_SECONDS = 0.25

TRUEHD_NOTE = ("A test input only (the user's decision, 2026-09-26): decode it, pass it through "
               "and round-trip it, with no analysis of how Dolby's encoder made it. ROADMAP.md "
               "keeps TrueHD interoperability by black-box analysis of Dolby streams out of scope.")

G2_GROUPS = {
    "eac3-rates": "E-AC-3 at every layout and rate: the trend and races against DEE "
                  "(quality_race.py, gen_external_baseline.py), the decoder's third-party "
                  "coverage (verify_gold_reference.sh), the fixed-point tiers "
                  "(planning/arithmetic-tiers.md)",
    "ac3-rates": "AC-3 at every layout and rate, for the same",
    "bluray": "7.1 E-AC-3 as Blu-ray carries it (an AC-3 core and a dependent substream): the "
              "decoder's dependent substreams and 7.1 from a third party",
    "seven-one": "what DEE does with 7.1 input to DD and DD+ (5.1), for transcoding",
    "eac3-metadata": "E-AC-3 metadata as DEE writes it: DRC words over stepped levels, downmix, "
                     "service and loudness fields, for the decoder, QC and Hearth's media "
                     "information",
    "ac3-metadata": "AC-3 metadata, the same, and AC-3's timecode",
    "joc": "E-AC-3 JOC from channel beds: the object layer's third-party coverage "
           "(test_dee_joc_fixture.cpp), transcoding to AC-4 (planning/ac4.md I1, I5)",
    "truehd": "TrueHD with a lossless source: the TrueHD module's decode material, scored "
              "against the sources by tools/checks/score_truehd_decode.py (planning/truehd.md), "
              "and MAT passthrough",
    "truehd-open": "TrueHD streams chosen to answer the open questions of libs/truehd/ERRATA.md "
                   "(planning/truehd.md, \"Oracles\"): the rates, downmix presentations and "
                   "three substreams, DRC and dialnorm, lengths that are not a whole number of "
                   "access units, other bit depths and layouts, the encoder's data-rate and "
                   "stream-end options, and the Atmos input",
    "truehd-fixtures": "short TrueHD streams with their sources, committed under "
                       "testdata/truehd-dee/ as the only streams in the tree from an outside "
                       "encoder at 7.1, with three substreams, at 96 kHz and at 16 and 20 bits",
    "truehd-long": "300 s TrueHD programmes for soak tests of the decoder and the network path "
                   "to Hearth sinks",
    "programme": "60 s and 300 s programmes for soak tests and listening: the ESP32 player "
                 "(planning/esp32-stream-set.md), Hearth",
    "objects": "Atmos-master inputs, refused on provenance as in the AC-4 gold set",
}

DEE_CANNOT = [
    ("any stream from objects: dee_ddpjoc_encoder, dee_dthd_encoder (--presentation atmos) and "
     "dee_ddp_encoder take objects only as an Atmos master and refuse every ADM BWF master this "
     "project writes with 'Content was not authored with Dolby tools' (the objects group)"),
    ("48 kHz is the only rate DD, DD+ and DD+ JOC take ('Invalid input. Unsupported sample-rate' "
     "for 32, 44.1 and 96 kHz); TrueHD takes 48 and 96 kHz and refuses 32 and 44.1"),
    ("DD+ above 5.1 other than the 'bluray' mode's 7.1 (an AC-3 core and a dependent "
     "substream), which --help does not list; the online DD+ and DD encoders write 7.1 input as "
     "5.1"),
    ("AC-3 layouts other than 1/0, 2/0 and 3/2 with or without the LFE, dual mono, 3.0 or 5.0 "
     "from a channel list with silent channels (DEE writes the layout it is asked for)"),
    ("E-AC-3 below its per-layout floors (mono 32, 2.0 96, 5.1 192 kbps; 5.1 at 64 to 160 only "
     "with allow_hybrid_downmix), several programmes, associated services with their mixing "
     "metadata (programme scale factors, mixdef, pan), or reduced-rate (fscod2) frames"),
    ("an embedded timecode in DD+ ('Embedding timestamp is supported only by DD and Bluray "
     "encoder modes'); where DD and the 'bluray' mode embed one, it is a 16-byte packet ahead "
     "of each sync frame, so the file is no longer a bare elementary stream (FFmpeg and forge "
     "both stop at byte 0; DEE's MP4 muxer takes the DD ones)"),
    ("an MP4 of a 'bluray' stream: DEE's MP4 muxer refuses the AC-3 core with its E-AC-3 "
     "dependent substream ('Unsupported bitstream id')"),
    ("a Pro Logic II preferred downmix in AC-3 ('Downmix Mode ltrt-pl2 is not supported by the "
     "selected codec'), and TrueHD's atsc_a85_agile loudness preset, which its help lists and "
     "its parser refuses"),
    ("multi-input (gapless) or split (cut-point) E-AC-3 JOC from channel beds ('Multi-input CBI "
     "encoding is not supported', 'Splitting CBI input is not supported')"),
    ("TrueHD options this build lists none of: a restart or random-access interval, a peak data "
     "rate or bit-rate cap, a target disc format, noise shaping or dither, a data check, the "
     "prediction filter order, a DVD-Audio (MLP) mode, signing, the mix levels of the downmix "
     "presentations, a DRC start-up gain, the mix level and the room type; only "
     "--optimize-data-rate, --add-silence and the trim options touch the stream's rate or its "
     "ends"),
    ("TrueHD above 96 kHz (44.1, 88.2, 176.4 and 192 kHz are each refused at 2.0), and a single "
     "WAV of 1, 3, 4, 5 or 7 channels: those layouts are taken only as a channel list with the "
     "missing slots left silent, so the stream has its presentation's full channel count"),
    ("a TrueHD input of more than eight channels (the sixteen-channel legs), or an "
     "evolution-frame file as an input; its Atmos presentation takes only an Atmos master"),
]

WAV_LIST = (DDP,)


# ------------------------------------------------------------------------------------------
# Sources
# ------------------------------------------------------------------------------------------

def read_fixture(flac, scratch_dir):
    """A committed 48 kHz 16-bit stereo FLAC as float64 samples, shape (n, 2)."""
    wav = Path(scratch_dir) / f"{flac.stem}.wav"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(flac), "-c:a", "pcm_s16le",
                    str(wav)], check=True)
    with wave.open(str(wav), "rb") as r:
        if (r.getframerate(), r.getnchannels(), r.getsampwidth()) != (RATE, 2, 2):
            raise SystemExit(f"{flac}: expected 48 kHz 16-bit stereo, got {r.getparams()}")
        pcm = np.frombuffer(r.readframes(r.getnframes()), dtype="<i2")
    return pcm.reshape(-1, 2) / 32768.0


def cut(x, start_s, seconds, rate=RATE):
    first = round(start_s * rate)
    count = round(seconds * rate)
    if first + count > len(x):
        raise SystemExit(f"a {seconds} s cut at {start_s} s runs past the end of the fixture")
    return x[first:first + count]


def lowpass(x, cutoff_hz=120.0, taps=2047, rate=RATE):
    """Linear-phase windowed-sinc low-pass; mode="same" removes its group delay."""
    n = np.arange(taps) - (taps - 1) / 2.0
    kernel = 2.0 * (cutoff_hz / rate) * np.sinc(2.0 * (cutoff_hz / rate) * n) * np.blackman(taps)
    return np.convolve(x, kernel / kernel.sum(), mode="same")


def fade(x, seconds=0.01, rate=RATE):
    n = round(seconds * rate)
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)
    y = np.array(x, dtype=np.float64)
    y[:n] *= ramp
    y[len(y) - n:] *= ramp[::-1]
    return y


def log_sweep(count, f0, f1, amplitude, rate=RATE):
    t = np.arange(count) / rate
    length = count / rate
    k = np.log(f1 / f0)
    return amplitude * np.sin(2.0 * np.pi * f0 * length / k * (np.exp(t / length * k) - 1.0))


def uniform_noise(count, seed):
    """count values uniform in [-1, 1) from splitmix64 over a counter: the same on any machine
    and any numpy."""
    z = (np.arange(1, count + 1, dtype=np.uint64)
         + np.uint64(seed) * np.uint64(1 << 32)) * np.uint64(0x9E3779B97F4A7C15)
    z = (z ^ (z >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
    z = (z ^ (z >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
    z = z ^ (z >> np.uint64(31))
    return (z >> np.uint64(11)).astype(np.float64) * 2.0 ** -52 - 1.0


def pink_noise(count, seed, rms, rate=RATE):
    spectrum = np.fft.rfft(uniform_noise(count, seed))
    bins = np.arange(len(spectrum), dtype=np.float64)
    low = bins < 20.0 * count / rate
    spectrum[low] = 0.0
    spectrum[~low] /= np.sqrt(bins[~low])
    x = np.fft.irfft(spectrum, count)
    return x * (rms / np.sqrt(np.mean(x * x)))


def loop_cut(x, start_s, seconds, crossfade_s=0.05):
    """seconds of x from start_s, x repeated with a raised-cosine crossfade at each join."""
    f = round(crossfade_s * RATE)
    ramp = (0.5 - 0.5 * np.cos(np.pi * np.arange(f) / f)).reshape(-1, *([1] * (x.ndim - 1)))
    body = np.array(x[:len(x) - f], dtype=np.float64)
    body[:f] = x[:f] * ramp + x[len(x) - f:] * (1.0 - ramp)
    first = round(start_s * RATE) % len(body)
    count = round(seconds * RATE)
    return np.concatenate([body] * ((first + count) // len(body) + 1))[first:first + count]


def upsample2(x, taps=255):
    """x at twice its rate: zeros between samples, then a windowed-sinc low-pass at 0.45 of the
    new rate's Nyquist frequency with a gain of two."""
    y = np.zeros(2 * len(x))
    y[::2] = x
    n = np.arange(taps) - (taps - 1) / 2.0
    kernel = 0.45 * np.sinc(0.45 * n) * np.blackman(taps)
    return np.convolve(y, 2.0 * kernel / kernel.sum(), mode="same")


def speaker_tones(speakers, seconds, rate=RATE):
    t = np.arange(round(seconds * rate)) / rate
    return [TONE_AMPLITUDE * np.sin(2.0 * np.pi * SPEAKER_HZ[s] * t) for s in speakers]


def hot_tones(speakers, seconds):
    """Every channel the same HOT_HZ tone at HOT_AMPLITUDE plus its own pink noise, so that a
    downmix of the channels reaches full scale."""
    count = round(seconds * RATE)
    t = np.arange(count) / RATE
    tone = HOT_AMPLITUDE * np.sin(2.0 * np.pi * HOT_HZ * t)
    return [tone + pink_noise(count, ALL_SPEAKERS.index(s) + 1, NOISE_RMS) for s in speakers]


def round20(x):
    """x quantised to 20 bits, as 24-bit samples with the low four bits clear (wav_bytes scales
    by 8388607, so a multiple of 16 over 8388607 comes out as that multiple)."""
    return np.round(x * (8388607.0 / 16.0)) * (16.0 / 8388607.0)


def music_speakers(music, seconds):
    """Every speaker's music: the AC-4 gold set's cuts (music_51, music_514, and G1's back
    surrounds, wides and top middles)."""
    t = max(5.0, seconds)
    mono = music.mean(axis=1)

    def pair(start, gain):
        return tuple(gain * c for c in cut(music, start, seconds).T)

    out = {}
    out["L"], out["R"] = cut(music, 0.0, seconds).T
    out["C"] = 0.6 * cut(mono, t, seconds)
    out["LFE"] = 0.8 * lowpass(cut(mono, 0.0, seconds))
    out["Ls"], out["Rs"] = pair(2 * t, 0.5)
    out["Tfl"], out["Tfr"] = pair(seconds / 2, 0.4)
    out["Tbl"], out["Tbr"] = pair(1.5 * seconds, 0.3)
    out["Lrs"], out["Rrs"] = pair(0.75 * seconds, 0.4)
    out["Lw"], out["Rw"] = pair(1.25 * seconds, 0.35)
    out["Ltm"], out["Rtm"] = pair(1.75 * seconds, 0.3)
    return out


def sweeps(speakers, seconds, rate=RATE, top=20000.0):
    count = round(seconds * rate)
    full = fade(log_sweep(count, 20.0, top, TONE_AMPLITUDE, rate), rate=rate)
    lfe = fade(log_sweep(count, 20.0, 120.0, TONE_AMPLITUDE, rate), rate=rate)
    mains = [s for s in speakers if s != "LFE"]
    return [lfe if s == "LFE" else np.roll(full, round(mains.index(s) * count / len(mains)))
            for s in speakers]


def noises(speakers, seconds, rate=RATE):
    count = round(seconds * rate)
    cols = []
    for s in speakers:
        x = pink_noise(count, ALL_SPEAKERS.index(s) + 1, NOISE_RMS, rate)
        if s == "LFE":
            x = lowpass(x, rate=rate)
            x *= NOISE_RMS / np.sqrt(np.mean(x * x))
        cols.append(fade(x, rate=rate))
    return cols


def transients(speakers, seconds):
    count = round(seconds * RATE)
    t = np.arange(round(0.08 * RATE)) / RATE
    click = uniform_noise(len(t), 99) * np.exp(-t / 0.010)
    click *= 0.5 / np.max(np.abs(click))
    t_lfe = np.arange(round(0.3 * RATE)) / RATE
    thump = np.sin(2.0 * np.pi * 55.0 * t_lfe) * np.exp(-t_lfe / 0.05)
    thump *= 0.5 / np.max(np.abs(thump))
    cols = []
    for i, s in enumerate(speakers):
        shape = thump if s == "LFE" else click
        x = np.zeros(count)
        start = 1.0 + i / len(speakers)
        while start + len(shape) / RATE <= seconds:
            first = round(start * RATE)
            x[first:first + len(shape)] += shape
            start += 1.0
        cols.append(x)
    return cols


def stepped_tones(speakers):
    n = round(STEP_SECONDS * RATE)
    gain = np.repeat(10.0 ** (np.array(STEP_LEVELS_DB, dtype=np.float64) / 20.0), n)
    kernel = np.hanning(round(0.005 * RATE))
    gain = np.convolve(gain, kernel / kernel.sum(), mode="same")
    x = fade(gain * np.sin(2.0 * np.pi * STEP_HZ * np.arange(len(gain)) / RATE))
    return [np.zeros_like(x) if s == "LFE" else x for s in speakers]


def programme(music, speech, speakers, seconds):
    """A programme of music, with the speech fixture's 0-20 s at 20 s and its 20-30 s at 45 s in
    each minute, the music ducked under it through 0.5 s ramps (d, the ducking envelope)."""
    count = round(seconds * RATE)
    t = np.arange(count) / RATE
    duck = np.zeros(count)
    placed = np.zeros((count, 2))

    def rise(u):
        return 0.5 - 0.5 * np.cos(np.pi * np.clip(u, 0.0, 1.0))

    for minute in range(int(np.ceil(seconds / 60.0))):
        for offset, start, length in PROGRAMME_DIALOGUE:
            at = offset + 60.0 * minute
            first = round(at * RATE)
            if first >= count:
                continue
            piece = cut(speech, start, length)[:count - first]
            placed[first:first + len(piece)] = piece
            duck += rise((t - at + 0.25) / 0.5) * rise((at + length + 0.25 - t) / 0.5)
    mono = music.mean(axis=1)
    front = loop_cut(music, 0.0, seconds)
    if speakers == SPEAKERS["20"]:
        return list(((1.0 - 0.8 * duck)[:, None] * front + duck[:, None] * placed).T)
    loops = {"back": loop_cut(music, 20.0, seconds), "rear": loop_cut(music, 7.5, seconds),
             "top_front": loop_cut(music, 5.0, seconds), "top_back": loop_cut(music, 15.0, seconds)}
    cols = {"L": (1.0 - 0.55 * duck) * front[:, 0], "R": (1.0 - 0.55 * duck) * front[:, 1],
            "C": 0.6 * (1.0 - duck) * loop_cut(mono, 10.0, seconds) + duck * placed.mean(axis=1),
            "LFE": 0.8 * lowpass(loop_cut(mono, 0.0, seconds)),
            "Ls": 0.5 * (1.0 - 0.3 * duck) * loops["back"][:, 0],
            "Rs": 0.5 * (1.0 - 0.3 * duck) * loops["back"][:, 1],
            "Lrs": 0.4 * (1.0 - 0.3 * duck) * loops["rear"][:, 0],
            "Rrs": 0.4 * (1.0 - 0.3 * duck) * loops["rear"][:, 1],
            "Tfl": 0.4 * (1.0 - 0.5 * duck) * loops["top_front"][:, 0],
            "Tfr": 0.4 * (1.0 - 0.5 * duck) * loops["top_front"][:, 1],
            "Tbl": 0.3 * (1.0 - 0.5 * duck) * loops["top_back"][:, 0],
            "Tbr": 0.3 * (1.0 - 0.5 * duck) * loops["top_back"][:, 1]}
    return [cols[s] for s in speakers]


def source_plan():
    """{name: (layout, seconds, rate, bits, builder, description)}: every source a leg names."""
    s = SECONDS
    plan = {}

    def add(name, layout, builder, text, seconds=s, rate=RATE, bits=24):
        plan[name] = (layout, seconds, rate, bits, builder, text)

    def tones_text(layout, rate=RATE):
        return (f"one tone per channel, {' '.join(SPEAKERS[layout])}: "
                + ", ".join(str(SPEAKER_HZ[k]) for k in SPEAKERS[layout])
                + f" Hz at -20 dBFS, {rate / 1000:g} kHz")

    bed = ("L R the music from 0 s, C its mono mix from 10 s x 0.6, LFE its mono mix from 0 s "
           "low-passed at 120 Hz x 0.8, Ls Rs from 20 s x 0.5, Lrs Rrs from 7.5 s x 0.4, Tfl "
           "Tfr from 5 s x 0.4, Tbl Tbr from 15 s x 0.3, Lw Rw from 12.5 s x 0.35, Ltm Rtm from "
           "17.5 s x 0.3 (the AC-4 gold set's music cuts)")
    for layout in ("10", "20", "51", "71", "514", "714", "916"):
        add(f"tones_{layout}", layout, ("tones", layout, RATE), tones_text(layout))
    for layout in ("10", "20", "51", "71", "514", "714", "916"):
        add(f"music_{layout}", layout, ("music", layout),
            "music from programme_music_stereo.flac, "
            + ("its mono mix from 0 s" if layout == "10" else "L R from 0 s"
               if layout == "20" else f"{' '.join(SPEAKERS[layout])}: {bed}"))
    add("speech_10", "10", ("speech", "10"), "programme_speech_stereo.flac's mono mix from 0 s")
    add("speech_20", "20", ("speech", "20"), "programme_speech_stereo.flac from 0 s")
    for layout in ("51", "71", "514"):
        add(f"film_{layout}", layout, ("film", layout),
            "film: the speech fixture's mono mix from 0 s in C over the music bed, L R x 0.45, "
            "Ls Rs x 0.35, Lrs Rrs and Tfl Tfr x 0.3, Tbl Tbr x 0.25, the same LFE (the AC-4 "
            "gold set's film_51, film_71 and film_514)")
    for layout in ("10", "20", "51", "71"):
        add(f"sweep_{layout}", layout, ("sweep", layout, RATE),
            "a logarithmic sweep, 20 Hz to 20 kHz over the source at -20 dBFS, in every channel "
            "but the LFE (20 to 120 Hz), started a 1/n of the length later in each of n channels")
        add(f"noise_{layout}", layout, ("noise", layout, RATE),
            "pink noise at -26 dBFS RMS per channel, splitmix64-seeded by speaker, the LFE's "
            "low-passed at 120 Hz")
        add(f"transient_{layout}", layout, ("transient", layout),
            "1 s of silence, then a decaying noise burst each second per channel, the channels "
            "a 1/n of a second apart (the LFE's a 55 Hz thump)")
    for layout in ("20", "51", "71", "514"):
        add(f"steps_{layout}", layout, ("steps", layout),
            f"a {STEP_HZ} Hz tone in every channel but the LFE, from {STEP_LEVELS_DB[0]} to "
            f"{STEP_LEVELS_DB[-1]} dBFS in 3 dB steps of {STEP_SECONDS:g} s",
            seconds=len(STEP_LEVELS_DB) * STEP_SECONDS)
    for layout in ("20", "51", "71", "514", "714"):
        add(f"programme_{layout}", layout, ("programme", layout),
            "a programme: music throughout, with the speech fixture's 0-20 s at 20 s and 20-30 "
            "s at 45 s, the music ducked under it", seconds=PROGRAMME_SECONDS)
    for layout in ("20", "51", "71", "514"):
        add(f"programme_{layout}_300", layout, ("programme", layout),
            "the programme for 300 s, its speech placed each minute", seconds=SOAK_SECONDS)
    for layout in ("20", "51", "71"):
        add(f"tones_{layout}_96k", layout, ("tones", layout, 96000), tones_text(layout, 96000),
            rate=96000)
        add(f"noise_{layout}_96k", layout, ("noise", layout, 96000),
            "pink noise made at 96 kHz, to 48 kHz", rate=96000)
        add(f"music_{layout}_96k", layout, ("music96", layout),
            "the music bed up-sampled by two (upsample2())", rate=96000)
    add("sweep_20_96k", "20", ("sweep", "20", 96000), "a logarithmic sweep, 20 Hz to 40 kHz",
        rate=96000)
    for name in ("tones_20", "tones_51", "tones_71", "music_20", "music_51", "music_71",
                 "tones_71_96k", "music_71_96k"):
        layout, seconds, rate, _bits, _builder, _text = plan[name]
        add(f"{name}_16", layout, ("round16", name), f"{name} rounded to 16 bits",
            seconds=seconds, rate=rate, bits=16)
    thd_open_sources(plan, add, tones_text)
    return plan


def thd_open_sources(plan, add, tones_text):
    """The sources of the TrueHD gold set of 2026-10-29 (GOLD_VERSION 2): other layouts, the
    rates DEE was asked for above 96 kHz, 20-bit and 16-bit copies, lengths that are not a whole
    number of access units, loud downmix sources and the 0.25 s fixtures."""
    for layout in ("30", "31", "40", "50", "70"):
        add(f"tones_{layout}", layout, ("tones", layout, RATE), tones_text(layout))
    for layout in ("51", "71"):
        add(f"hot_{layout}", layout, ("hot", layout),
            f"every channel the same {HOT_HZ} Hz tone at {HOT_AMPLITUDE} plus its own pink noise "
            "at -26 dBFS RMS, so that a downmix passes full scale")
    for tag, rate in HIGH_RATES.items():
        for layout in ("20", "51", "71"):
            add(f"tones_{layout}_{tag}", layout, ("tones", layout, rate),
                tones_text(layout, rate), rate=rate)
        for layout in ("20", "51"):
            add(f"noise_{layout}_{tag}", layout, ("noise", layout, rate),
                f"pink noise made at {rate / 1000:g} kHz", rate=rate)
        add(f"sweep_20_{tag}", "20", ("sweep", "20", rate),
            f"a logarithmic sweep, 20 Hz to {0.45 * rate / 1000:g} kHz", rate=rate)
    add("music_20_192k", "20", ("music192", "20"),
        "the music bed up-sampled by two twice (upsample2())", rate=192000)
    add("tones_20_192k_16", "20", ("round16", "tones_20_192k"),
        "tones_20_192k rounded to 16 bits", rate=192000, bits=16)
    add("tones_20_192k_b20", "20", ("round20", "tones_20_192k"),
        "tones_20_192k rounded to 20 bits (the low four bits of each 24-bit sample clear)",
        rate=192000)
    for name, base in (("music_20_b20", "music_20"), ("music_51_b20", "music_51"),
                       ("tones_71_b20", "tones_71"), ("music_20_96k_b20", "music_20_96k")):
        layout, seconds, rate, _bits, _builder, _text = plan[base]
        add(name, layout, ("round20", base), f"{base} rounded to 20 bits (the low four bits of "
            "each 24-bit sample clear)", seconds=seconds, rate=rate)
    for name, base in (("music_20_96k_16", "music_20_96k"), ("music_51_96k_16", "music_51_96k")):
        layout, seconds, rate, _bits, _builder, _text = plan[base]
        add(name, layout, ("round16", base), f"{base} rounded to 16 bits", seconds=seconds,
            rate=rate, bits=16)
    for n in LENGTH_SAMPLES:
        add(f"tones_20_n{n}", "20", ("tones", "20", RATE), f"{tones_text('20')}, exactly {n} "
            "samples", seconds=n / RATE)
    add("tones_20_96k_n24001", "20", ("tones", "20", 96000), f"{tones_text('20', 96000)}, "
        "exactly 24001 samples", seconds=24001 / 96000, rate=96000)
    add("tones_20_192k_n48001", "20", ("tones", "20", 192000),
        f"{tones_text('20', 192000)}, exactly 48001 samples", seconds=48001 / 192000,
        rate=192000)
    add("tones_51_n12001", "51", ("tones", "51", RATE), f"{tones_text('51')}, exactly 12001 "
        "samples", seconds=12001 / RATE)
    f = FIXTURE_SECONDS
    add("fx_tones_71", "71", ("tones", "71", RATE), tones_text("71"), seconds=f)
    add("fx_tones_71_96k", "71", ("tones", "71", 96000), tones_text("71", 96000),
        seconds=f / 2, rate=96000)
    add("fx_music_51", "51", ("music", "51"), "music_51 for 0.25 s", seconds=f)
    add("fx_music_20", "20", ("music", "20"), "music_20 for 0.25 s", seconds=f)
    add("fx_music_20_96k", "20", ("music96", "20"), "music_20_96k for 0.25 s", seconds=f,
        rate=96000)
    add("fx_tones_20", "20", ("tones", "20", RATE), tones_text("20"), seconds=f)
    add("fx_tones_20_16", "20", ("round16", "fx_tones_20"), "fx_tones_20 rounded to 16 bits",
        seconds=f, bits=16)
    add("fx_tones_20_b20", "20", ("round20", "fx_tones_20"), "fx_tones_20 rounded to 20 bits",
        seconds=f)
    for tag in ("88k", "176k", "192k"):
        add(f"fx_tones_20_{tag}", "20", ("tones", "20", HIGH_RATES[tag]),
            tones_text("20", HIGH_RATES[tag]), seconds=f, rate=HIGH_RATES[tag])


def build_columns(spec, music, speech, plan, cache, seconds=SECONDS):
    kind = spec[0]
    if kind == "tones":
        _, layout, rate = spec
        return speaker_tones(SPEAKERS[layout], seconds, rate)
    if kind == "hot":
        return hot_tones(SPEAKERS[spec[1]], seconds)
    if kind == "music":
        layout = spec[1]
        if layout == "10":
            return [cut(music.mean(axis=1), 0.0, seconds)]
        cols = music_speakers(music, seconds)
        return [cols[k] for k in SPEAKERS[layout]]
    if kind == "music96":
        return [upsample2(c) for c in build_columns(("music", spec[1]), music, speech, plan,
                                                     cache, seconds)]
    if kind == "music192":
        return [upsample2(upsample2(c)) for c in build_columns(("music", spec[1]), music, speech,
                                                                plan, cache, seconds)]
    if kind == "speech":
        x = cut(speech, 0.0, SECONDS)
        return [x.mean(axis=1)] if spec[1] == "10" else list(x.T)
    if kind == "film":
        # The AC-4 gold set's film_51, film_71 and film_514: the bed's L R x 0.45 and Ls Rs x 0.7
        # of their music levels, Lrs Rrs and Tfl Tfr at 0.75 of theirs, Tbl Tbr at 0.25.
        cols = music_speakers(music, SECONDS)
        gains = {"L": 0.45, "R": 0.45, "LFE": 1.0, "Ls": 0.7, "Rs": 0.7, "Lrs": 0.75,
                 "Rrs": 0.75, "Tfl": 0.75, "Tfr": 0.75, "Tbl": 0.25 / 0.3, "Tbr": 0.25 / 0.3}
        return [cut(speech.mean(axis=1), 0.0, SECONDS) if k == "C" else gains[k] * cols[k]
                for k in SPEAKERS[spec[1]]]
    if kind == "sweep":
        _, layout, rate = spec
        top = 20000.0 if rate == RATE else 40000.0 if rate == 96000 else 0.45 * rate
        return sweeps(SPEAKERS[layout], seconds, rate, top=top)
    if kind == "noise":
        _, layout, rate = spec
        return noises(SPEAKERS[layout], seconds, rate)
    if kind == "transient":
        return transients(SPEAKERS[spec[1]], SECONDS)
    if kind == "steps":
        return stepped_tones(SPEAKERS[spec[1]])
    if kind == "programme":
        return None  # built by build_sources(), which knows the length
    if kind == "round16":
        base = cache[spec[1]]
        return [np.round(c * 32767.0) / 32767.0 for c in base]
    if kind == "round20":
        return [round20(c) for c in cache[spec[1]]]
    raise ValueError(spec)


def wav_bytes(columns, rate, bits):
    """A PCM WAV of the columns; a mix peaking above 0.98 is scaled to 0.98."""
    pcm = np.stack(columns, axis=1)
    peak = float(np.max(np.abs(pcm)))
    if peak > 0.98:
        pcm = pcm * (0.98 / peak)
    if bits == 16:
        frames = np.round(pcm * 32767.0).astype("<i2").tobytes()
    else:
        samples = np.round(pcm * 8388607.0).astype("<i4")
        frames = samples.view(np.uint8).reshape(len(samples), -1, 4)[:, :, :3].tobytes()
    with tempfile.SpooledTemporaryFile() as f:
        with wave.open(f, "wb") as w:
            w.setnchannels(pcm.shape[1])
            w.setsampwidth(bits // 8)
            w.setframerate(rate)
            w.writeframes(frames)
        f.seek(0)
        return f.read()


def replace_file(path, data):
    """Writes data to path through a temporary file renamed over it, so a reader sees the old
    file or the new one, never a partial one."""
    path = Path(path)
    temporary = path.with_name(path.name + f".tmp{os.getpid()}")
    temporary.write_bytes(data)
    for attempt in range(60):
        try:
            os.replace(temporary, path)
            return
        except PermissionError:
            if attempt == 59:
                raise
            time.sleep(1.0)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 22), b""):
            h.update(block)
    return h.hexdigest()


def build_sources(root, names):
    """Writes the named sources under DIR/sources where missing or different; {name: entry}."""
    plan = source_plan()
    with tempfile.TemporaryDirectory() as temporary:
        music = read_fixture(AUDIO / "programme_music_stereo.flac", temporary)
        speech = read_fixture(AUDIO / "programme_speech_stereo.flac", temporary)
    rounded = ("round16", "round20")
    bases = {plan[n][4][1] for n in names if plan[n][4][0] in rounded}
    cache, entries = {}, {}
    for name in sorted(names, key=lambda n: plan[n][4][0] in rounded):
        layout, seconds, rate, bits, builder, text = plan[name]
        if builder[0] in rounded and builder[1] not in cache:
            cache[builder[1]] = build_columns(plan[builder[1]][4], music, speech, plan, cache,
                                              plan[builder[1]][1])
        if builder[0] == "programme":
            cols = programme(music, speech, SPEAKERS[layout], seconds)
        else:
            cols = build_columns(builder, music, speech, plan, cache, seconds)
        if name in bases:
            cache[name] = cols
        path = root / "sources" / f"{name}.wav"
        data = wav_bytes(cols, rate, bits)
        del cols
        if not (path.is_file() and path.stat().st_size == len(data)
                and path.read_bytes() == data):
            path.parent.mkdir(parents=True, exist_ok=True)
            replace_file(path, data)
            print(f"source {name}: written", flush=True)
        entries[name] = {"layout": layout, "speakers": list(SPEAKERS[layout]),
                         "seconds": seconds, "sample_rate": rate, "bits": bits,
                         "description": text, "size_bytes": len(data),
                         "sha256": hashlib.sha256(data).hexdigest()}
    return entries


# ------------------------------------------------------------------------------------------
# Legs
# ------------------------------------------------------------------------------------------

def layout_of(tag):
    return {"10": "mono", "20": "stereo", "51": "5.1", "71": "auto"}[tag]


def g2_legs():
    """Every leg, by group; the module docstring says what each group is."""
    legs = []

    def add(group, name, encoder, source, options, codec, **extra):
        legs.append({"name": name, "group": group, "encoder": encoder, "source": source,
                     "options": list(options), "codec": codec, **extra})

    def ddp_leg(group, name, codec_mode, source, layout_tag, kbps, settings="",
                loudness=None, quiet=QUIET, **extra):
        encoder_opt = f"{codec_mode}:{quiet}" + (f":{settings}" if settings else "")
        opts = ["--output-channel-layout", layout_of(layout_tag), "--data-rate", str(kbps),
                "--encoder", encoder_opt, *(loudness or MEASURE_ONLY)]
        codec = {"dd": "ac3", "ddp": "eac3", "bluray": "eac3-bluray"}[codec_mode]
        add(group, name, DDP, source, opts, codec, kbps=kbps, layout=layout_tag, **extra)

    # E-AC-3 and AC-3 at every rate.
    for layout, sources in (("10", ("music", "speech", "tones")),
                            ("20", ("music", "speech", "tones", "sweep", "noise", "transient")),
                            ("51", ("music", "film", "tones", "sweep", "noise", "transient"))):
        for kbps in DDP_RATES:
            if kbps < DDP_FLOOR[layout]:
                continue
            for source in sources:
                ddp_leg("eac3-rates", f"ddp-{layout}-{source}-{kbps}", "ddp",
                        f"{source}_{layout}", layout, kbps, refusal_ok=True)
        for kbps in DD_RATES:
            if kbps < DD_FLOOR[layout]:
                continue
            for source in sources:
                ddp_leg("ac3-rates", f"dd-{layout}-{source}-{kbps}", "dd", f"{source}_{layout}",
                        layout, kbps, refusal_ok=True)
    for kbps in HYBRID_RATES:
        for source in ("music", "film", "tones"):
            ddp_leg("eac3-rates", f"ddp-51-{source}-{kbps}-hybrid", "ddp", f"{source}_51", "51",
                    kbps, "allow_hybrid_downmix=1")
    for kbps in (192, 256, 384, 640):
        ddp_leg("eac3-rates", f"ddp-50-music-{kbps}", "ddp", "music_51", "51", kbps, "lfe_on=0")
    for kbps in (224, 384, 640):
        ddp_leg("ac3-rates", f"dd-50-music-{kbps}", "dd", "music_51", "51", kbps, "lfe_on=0")

    # Blu-ray 7.1.
    for kbps in BLURAY_RATES:
        for source in ("tones", "music", "film", "sweep", "noise"):
            ddp_leg("bluray", f"bd-71-{source}-{kbps}", "bluray", f"{source}_71", "71", kbps)
    for tag, setting in (("drc-film_standard-speech", "drc_profile_line=film_standard:"
                          "drc_profile_rf=speech"),
                         ("ltrt", "preferred_downmix_mode=ltrt:ltrt_cmix=-1.5:ltrt_smix=-4.5"),
                         ("surround-ex", "dolby_surround_ex_mode=yes"),
                         ("atten", "surround_3dB_attenuation=1")):
        ddp_leg("bluray", f"bd-71-tones-1024-{tag}", "bluray", "tones_71", "71", 1024, setting)
    for fps in TIMECODE_RATES:
        ddp_leg("bluray", f"bd-71-music-1024-timecode-{fps.replace('.', '')}", "bluray",
                "music_71", "71", 1024, extra_args=["--embed-timecode", "01:00:00:00",
                                                    "--timecode-frame-rate", fps])

    # 7.1 input to the online encoders.
    for codec_mode, rates in (("ddp", (448, 768, 1024)), ("dd", (448, 640))):
        for kbps in rates:
            for source in ("tones", "music"):
                ddp_leg("seven-one", f"{codec_mode}-71in-{source}-{kbps}", codec_mode,
                        f"{source}_71", "71", kbps)

    # Metadata, E-AC-3 and AC-3 alike.
    for codec_mode, group, kbps in (("ddp", "eac3-metadata", 384), ("dd", "ac3-metadata", 448)):
        tag = codec_mode
        for profile in DRC_PROFILES:
            ddp_leg(group, f"{tag}-51-steps-{kbps}-drc-{profile}", codec_mode, "steps_51", "51",
                    kbps, f"drc_profile={profile}")
            ddp_leg(group, f"{tag}-20-steps-192-drc-{profile}", codec_mode, "steps_20", "20",
                    192, f"drc_profile={profile}")
        ddp_leg(group, f"{tag}-51-steps-{kbps}-drc-line-film_standard-rf-speech", codec_mode,
                "steps_51", "51", kbps, "drc_profile_line=film_standard:drc_profile_rf=speech")
        ddp_leg(group, f"{tag}-51-steps-{kbps}-drc-line-music_light-rf-film_light", codec_mode,
                "steps_51", "51", kbps, "drc_profile_line=music_light:drc_profile_rf=film_light")
        for mode in DMX_MODES:
            # DD refuses ltrt-pl2 ("not supported by the selected codec"): AC-3 has no code
            # for it. The refusal is kept.
            ddp_leg(group, f"{tag}-51-tones-{kbps}-dmx-{mode}", codec_mode, "tones_51", "51",
                    kbps, f"preferred_downmix_mode={mode}", refusal_ok=True)
        for key, values in MIX_LEVELS.items():
            for value in values:
                label = value.replace("+", "p").replace("-", "m")
                ddp_leg(group, f"{tag}-51-tones-{kbps}-mix-{key}-{label}", codec_mode,
                        "tones_51", "51", kbps, f"{key}={value}")
        for mode in BITSTREAM_MODES:
            ddp_leg(group, f"{tag}-51-film-{kbps}-bsmod-{mode}", codec_mode, "film_51", "51",
                    kbps, f"bitstream_mode={mode}")
        ddp_leg(group, f"{tag}-10-speech-192-bsmod-voice_over", codec_mode, "speech_10", "10",
                192, "bitstream_mode=voice_over")
        for value in ("yes", "no", "not_indicated"):
            ddp_leg(group, f"{tag}-20-music-192-dsurmod-{value}", codec_mode, "music_20", "20",
                    192, f"dolby_surround_mode={value}")
            ddp_leg(group, f"{tag}-51-music-{kbps}-dsurexmod-{value}", codec_mode, "music_51",
                    "51", kbps, f"dolby_surround_ex_mode={value}")
        ddp_leg(group, f"{tag}-51-tones-{kbps}-atten", codec_mode, "tones_51", "51", kbps,
                "surround_3dB_attenuation=1",
                audio_altered="surround channels attenuated by 3 dB")
        opts = ["--output-channel-layout", "5.1", "--data-rate", str(kbps), "--encoder",
                codec_mode, *MEASURE_ONLY]
        add(group, f"{tag}-51-tones-{kbps}-production-defaults", DDP, "tones_51", opts,
            {"dd": "ac3", "ddp": "eac3"}[codec_mode], kbps=kbps, layout="51",
            audio_altered="DEE's defaults: 90-degree surround phase shift and LFE low-pass")
        for preset in LOUDNESS_PRESETS:
            ddp_leg(group, f"{tag}-51-music-{kbps}-loudness-{preset}", codec_mode, "music_51",
                    "51", kbps, loudness=["--loudness-management",
                                          f"measure_only:preset={preset}"])
        for target in DDP_TARGETS:
            ddp_leg(group, f"{tag}-51-music-{kbps}-target-{-target}", codec_mode, "music_51",
                    "51", kbps, loudness=["--loudness-management",
                                          f"measure_and_correct:loudness_target={target}"],
                    audio_altered=f"loudness corrected to {target} LKFS, -2 dBTP limiter",
                    refusal_ok=True)
        ddp_leg(group, f"{tag}-51-film-{kbps}-di-0", codec_mode, "film_51", "51", kbps,
                loudness=["--loudness-management", "measure_only:dialogue_intelligence=0"])
        for threshold in (0, 50, 100):
            ddp_leg(group, f"{tag}-51-film-{kbps}-speech-threshold-{threshold}", codec_mode,
                    "film_51", "51", kbps, loudness=[
                        "--loudness-management", f"measure_only:speech_threshold={threshold}"])
        ddp_leg(group, f"{tag}-51-music-{kbps}-lfe-lowpass", codec_mode, "music_51", "51",
                kbps, "lfe_filter=1", quiet="surround_90deg_phase_shift=0",
                audio_altered="the LFE low-pass on")
    for value in (0, 4660, 65535):
        ddp_leg("eac3-metadata", f"ddp-51-music-384-user-data-{value}", "ddp", "music_51", "51",
                384, f"user_data={value}")
    for fps in TIMECODE_RATES:
        ddp_leg("ac3-metadata", f"dd-51-music-448-timecode-{fps.replace('.', '')}", "dd",
                "music_51", "51", 448, extra_args=["--embed-timecode", "01:00:00:00",
                                                   "--timecode-frame-rate", fps])

    # E-AC-3 JOC from channel beds.
    def joc(name, source, kbps, cbi="cbi_wav", settings=None, loudness=None, **extra):
        opts = ["--input-format", cbi, "--data-rate", str(kbps),
                *(["--encoder", settings] if settings else []), *(loudness or MEASURE_ONLY)]
        add("joc", name, DDPJOC, source, opts, "eac3-joc", kbps=kbps, **extra)

    for layout in ("514", "714", "916"):
        for kbps in JOC_RATES:
            for source in ("tones", "music"):
                joc(f"joc-{layout}-{source}-{kbps}", f"{source}_{layout}", kbps)
    for layout in ("514",):
        for kbps in (448, 768):
            joc(f"joc-{layout}-film-{kbps}", f"film_{layout}", kbps)
    for height in HEIGHT_TRIMS:
        for surround in SURROUND_TRIMS:
            joc(f"joc-514-tones-768-trim-h{height[1:]}-s{surround.lstrip('-')}", "tones_514",
                768, f"cbi_wav:height_trim_5_1={height}:surround_trim_5_1={surround}")
    for mode in DMX_MODES:
        joc(f"joc-514-tones-768-dmx-{mode}", "tones_514", 768,
            settings=f"preferred_downmix_mode={mode}")
    for key, values in MIX_LEVELS.items():
        for value in values:
            label = value.replace("+", "p").replace("-", "m")
            joc(f"joc-514-tones-768-mix-{key}-{label}", "tones_514", 768,
                settings=f"{key}={value}")
    for profile in DRC_PROFILES:
        joc(f"joc-514-steps-768-drc-{profile}", "steps_514", 768,
            settings=f"drc_profile={profile}")
    joc("joc-514-music-768-musicmode", "music_514", 768,
        settings="mode=music:drc_profile=music_light",
        loudness=["--loudness-management", "measure_only:dialogue_intelligence=0"])
    for preset in LOUDNESS_PRESETS:
        joc(f"joc-514-music-768-loudness-{preset}", "music_514", 768,
            loudness=["--loudness-management", f"measure_only:preset={preset}"])
    # The user guide says channel-based immersive input is not taken in gapless encoding; these
    # two ask, and keep DEE's answer.
    add("joc", "joc-514-programme-768-gapless", DDPJOC, "programme_514",
        ["--input-format", "cbi_wav", "--data-rate", "768", *MEASURE_ONLY], "eac3-joc",
        kbps=768, parts=3, refusal_ok=True)
    add("joc", "joc-514-programme-768-cut", DDPJOC, "programme_514",
        ["--input-format", "cbi_wav", "--data-rate", "768", *MEASURE_ONLY, "--cut-point", "20",
         "--cut-point", "45"], "eac3-joc", kbps=768, outputs=3, refusal_ok=True)

    # TrueHD.
    def thd(name, presentations, loudness=None, extra_args=(), group="truehd", **extra):
        opts = []
        slots = {}
        for presentation, given in presentations:
            opts += ["--presentation", presentation]
            if isinstance(given, tuple):
                # (name, slots): the source's channels as mono WAVs in the slots of a wav_list,
                # None leaving a slot silent.
                slots[given[0]] = given[1]
                opts += ["--input-format", "wav_list", "-i", "{wavlist:" + given[0] + "}"]
            elif given:
                opts += ["--input-format", "wav", "-i", "{" + given + "}"]
        opts += [*(loudness or []), *extra_args]
        sources = [s[0] if isinstance(s, tuple) else s for _, s in presentations if s]
        add(group, name, DTHD, sources[0], opts, "truehd", sources=sources,
            use_note=TRUEHD_NOTE, **({"wav_slots": slots} if slots else {}), **extra)

    for pres, layout in (("2ch", "20"), ("6ch", "51"), ("8ch", "71")):
        for source in ("tones", "music", "noise", "sweep", *(("film",) if layout != "20"
                                                             else ())):
            thd(f"thd-{pres}-{source}-48k24", [(pres, f"{source}_{layout}")])
        for source in ("tones", "noise", "music"):
            thd(f"thd-{pres}-{source}-96k24", [(pres, f"{source}_{layout}_96k")])
        for source in ("tones", "music"):
            thd(f"thd-{pres}-{source}-48k16", [(pres, f"{source}_{layout}_16")])
    thd("thd-2ch-sweep-96k24", [("2ch", "sweep_20_96k")])
    thd("thd-8ch-tones-96k16", [("8ch", "tones_71_96k_16")])
    thd("thd-8ch-music-96k16", [("8ch", "music_71_96k_16")])
    thd("thd-8ch-music-48k24-dmx6-dmx2", [("8ch", "music_71"), ("6ch", None), ("2ch", None)])
    thd("thd-8ch-tones-48k24-dmx2", [("8ch", "tones_71"), ("2ch", None)])
    thd("thd-6ch-tones-48k24-dmx2", [("6ch", "tones_51"), ("2ch", None)])
    thd("thd-8ch-music-48k24-indep2", [("8ch", "music_71"), ("2ch", "speech_20")])
    thd("thd-6ch-music-48k24-indep2", [("6ch", "music_51"), ("2ch", "music_20")])
    for fmt in ("stereo", "headphone_encoded", "surround_encoded"):
        thd(f"thd-2ch-music-48k24-format-{fmt}", [(f"2ch:format={fmt}", "music_20")])
    thd("thd-2ch-music-48k24-drc-off", [("2ch:drc_default_on=0", "music_20")])
    for profile in ("film_light", "film_standard", "music_light", "music_standard", "speech",
                    "not_indicated"):
        thd(f"thd-6ch-steps-48k24-drc-{profile}", [(f"6ch:drc_profile={profile}", "steps_51")])
    thd("thd-8ch-music-48k24-dmx6-drc-music_light", [("8ch", "music_71"),
                                                     ("6ch:drc_profile=music_light", None)])
    for pres, source in (("8ch", "tones_71"), ("6ch", "tones_51")):
        thd(f"thd-{pres}-tones-48k24-atten", [(f"{pres}:surround_3dB_attenuation=1", source)],
            audio_altered="surround channels attenuated by 3 dB")
    for dialnorm in THD_DIALNORMS:
        thd(f"thd-6ch-music-48k24-dialnorm-{-dialnorm}", [("6ch", "music_51")],
            loudness=["--loudness-management", f"skip:dialnorm={dialnorm}"])
    # --morehelp loudness-management lists atsc_a85_agile, which the encoder refuses; the
    # refusal is kept.
    for preset in THD_PRESETS:
        thd(f"thd-6ch-film-48k24-loudness-{preset}", [("6ch", "film_51")],
            loudness=["--loudness-management", f"measure_only:preset={preset}"],
            refusal_ok=True)
    for fps in TIMECODE_RATES:
        thd(f"thd-6ch-music-48k24-timecode-{fps.replace('.', '')}", [("6ch", "music_51")],
            extra_args=["--embed-timecode", "01:00:00:00", "--timecode-frame-rate", fps])
    for pres, source in (("8ch", "music_71"), ("6ch", "music_51"), ("2ch", "music_20")):
        thd(f"thd-{pres}-music-48k24-optimize", [(pres, source)],
            extra_args=["--optimize-data-rate", "1"],
            audio_altered="--optimize-data-rate requantises the samples before coding")

    thd_open_legs(thd)

    # Programmes.
    for kbps in (96, 128, 192, 256):
        ddp_leg("programme", f"ddp-20-programme-{kbps}", "ddp", "programme_20", "20", kbps)
    for kbps in (256, 384, 448, 640):
        ddp_leg("programme", f"ddp-51-programme-{kbps}", "ddp", "programme_51", "51", kbps)
    for kbps in (192, 448, 640):
        layout = "20" if kbps == 192 else "51"
        ddp_leg("programme", f"dd-{layout}-programme-{kbps}", "dd", f"programme_{layout}",
                layout, kbps)
    ddp_leg("programme", "bd-71-programme-1024", "bluray", "programme_71", "71", 1024)
    add("programme", "joc-514-programme-768", DDPJOC, "programme_514",
        ["--input-format", "cbi_wav", "--data-rate", "768", *MEASURE_ONLY], "eac3-joc",
        kbps=768)
    add("programme", "joc-714-programme-1024", DDPJOC, "programme_714",
        ["--input-format", "cbi_wav", "--data-rate", "1024", *MEASURE_ONLY], "eac3-joc",
        kbps=1024)
    for pres, layout in (("2ch", "20"), ("6ch", "51"), ("8ch", "71")):
        thd(f"thd-{pres}-programme-48k24", [(pres, f"programme_{layout}")], group="programme")
    ddp_leg("programme", "ddp-51-programme_300-448", "ddp", "programme_51_300", "51", 448)
    add("programme", "joc-514-programme_300-768", DDPJOC, "programme_514_300",
        ["--input-format", "cbi_wav", "--data-rate", "768", *MEASURE_ONLY], "eac3-joc",
        kbps=768)
    return legs


def thd_open_legs(thd):
    """The TrueHD gold set of 2026-10-29, blocks C to L of the work order (block B's Atmos legs
    are object_legs(), whose masters are named on the command line). Every exploratory leg is
    allowed to be refused, and its refusal kept. What DEE has no option for is in DEE_CANNOT."""
    def op(name, presentations, group="truehd-open", **extra):
        thd(name, presentations, group=group, refusal_ok=True, **extra)

    # C: the sample rates. First the probes; then, for each rate DEE accepted, the longer legs.
    for tag in HIGH_RATES:
        op(f"thd-2ch-tones-{tag}24", [("2ch", f"tones_20_{tag}")])
    for tag in HIGH_RATES_ACCEPTED:
        legs = [("2ch", "noise", "20"), ("6ch", "tones", "51"), ("8ch", "tones", "71")]
        if tag != "44k":
            legs += [("2ch", "sweep", "20"), ("6ch", "noise", "51")]
        for pres, kind, layout in legs:
            op(f"thd-{pres}-{kind}-{tag}24", [(pres, f"{kind}_{layout}_{tag}")])
    if "192k" in HIGH_RATES_ACCEPTED:
        op("thd-2ch-music-192k24", [("2ch", "music_20_192k")])
        op("thd-2ch-tones-192k16", [("2ch", "tones_20_192k_16")])
        op("thd-2ch-tones-192k20", [("2ch", "tones_20_192k_b20")])
        op("thd-len-2ch-192k-48001", [("2ch", "tones_20_192k_n48001")])

    # D: the downmix presentations, which are also the streams with three substreams.
    op("thd-dmx-8ch-hot-48k24-dmx6-dmx2", [("8ch", "hot_71"), ("6ch", None), ("2ch", None)])
    op("thd-dmx-8ch-hot-48k24-dmx2", [("8ch", "hot_71"), ("2ch", None)])
    op("thd-dmx-6ch-hot-48k24-dmx2", [("6ch", "hot_51"), ("2ch", None)])
    op("thd-dmx-8ch-music-48k24-dmx6", [("8ch", "music_71"), ("6ch", None)])
    op("thd-dmx-8ch-tones-96k24-dmx6-dmx2", [("8ch", "tones_71_96k"), ("6ch", None),
                                             ("2ch", None)])

    # E: DRC and loudness metadata.
    for profile in ("film_light", "film_standard", "music_light", "music_standard", "speech",
                    "not_indicated"):
        op(f"thd-drc-2ch-steps-48k24-{profile}", [(f"2ch:drc_profile={profile}", "steps_20")])
        op(f"thd-drc-8ch-steps-48k24-{profile}", [(f"8ch:drc_profile={profile}", "steps_71")])
    for value in (31, 24, 1):
        for pres, source in (("2ch", "music_20"), ("8ch", "music_71")):
            op(f"thd-dialnorm-{pres}-{value}", [(pres, source)],
               loudness=["--loudness-management", f"skip:dialnorm={-value}"])

    # F: stream ends and lengths that are not a whole number of access units.
    for n in LENGTH_SAMPLES:
        op(f"thd-len-2ch-48k-{n}", [("2ch", f"tones_20_n{n}")])
    op("thd-len-2ch-96k-24001", [("2ch", "tones_20_96k_n24001")])
    op("thd-len-6ch-48k-12001", [("6ch", "tones_51_n12001")])

    # G: the encoder's data-rate and stream-end options: --optimize-data-rate, --add-silence and
    # the trim options are all this build lists (dee-options.md).
    op("thd-opt-rate-off-48k24", [("2ch", "music_20")], extra_args=["--optimize-data-rate", "0"])
    op("thd-opt-rate-off-96k24", [("2ch", "music_20_96k")],
       extra_args=["--optimize-data-rate", "0"])
    op("thd-opt-rate-on-96k24", [("2ch", "music_20_96k")],
       extra_args=["--optimize-data-rate", "1"],
       audio_altered="--optimize-data-rate requantises the samples before coding")
    op("thd-opt-end-silence", [("2ch", "music_20")], extra_args=["--add-silence", "0.25:0.5"],
       audio_altered="--add-silence pads the source with silence at both ends")
    op("thd-opt-end-silence-frames", [("2ch", "music_20")],
       extra_args=["--timecode-frame-rate", "25", "--add-silence", "3f:5f"],
       audio_altered="--add-silence pads the source with silence at both ends")
    op("thd-opt-end-trim", [("2ch", "music_20")],
       extra_args=["--start", "1.5", "--end", "7.25"],
       audio_altered="--start and --end cut the source")

    # H: other bit depths.
    for name, pres, source in (("thd-2ch-music-48k20", "2ch", "music_20_b20"),
                               ("thd-6ch-music-48k20", "6ch", "music_51_b20"),
                               ("thd-8ch-tones-48k20", "8ch", "tones_71_b20"),
                               ("thd-2ch-music-96k20", "2ch", "music_20_96k_b20"),
                               ("thd-2ch-music-96k16", "2ch", "music_20_96k_16"),
                               ("thd-6ch-music-96k16", "6ch", "music_51_96k_16")):
        op(name, [(pres, source)])

    # I: channel counts, each into the smallest presentation that can hold it.
    for layout, pres in (("10", "2ch"), ("30", "6ch"), ("31", "6ch"),
                                ("40", "6ch"), ("50", "6ch"), ("70", "8ch")):
        op(f"thd-chn-{layout}-{pres}-48k24", [(pres, f"tones_{layout}")])

    # The same sources as channel lists, the missing channels left silent (DEE's wav_list takes
    # '-' for a channel to replace with silence).
    for layout, pres, slots in (("10", "2ch", (0, None)),
                                ("30", "6ch", (0, 1, 2, None, None, None)),
                                ("31", "6ch", (0, 1, 2, 3, None, None)),
                                ("40", "6ch", (0, 1, None, None, 2, 3)),
                                ("50", "6ch", (0, 1, 2, None, 3, 4)),
                                ("70", "8ch", (0, 1, 2, None, 3, 4, 5, None))):
        op(f"thd-chn-{layout}-{pres}-48k24-wavlist", [(pres, (f"tones_{layout}", slots))])

    # J: the fixtures, 0.25 s each.
    def fx(name, presentations, **extra):
        op(name, presentations, group="truehd-fixtures", **extra)

    fx("thd-fx-8ch-tones-48k24-dmx6-dmx2", [("8ch", "fx_tones_71"), ("6ch", None), ("2ch", None)])
    fx("thd-fx-6ch-music-48k24", [("6ch", "fx_music_51")])
    fx("thd-fx-2ch-music-48k24", [("2ch", "fx_music_20")])
    fx("thd-fx-2ch-music-96k24", [("2ch", "fx_music_20_96k")])
    fx("thd-fx-8ch-tones-96k24", [("8ch", "fx_tones_71_96k")])
    for tag in ("88k", "176k", "192k"):
        if tag in HIGH_RATES_ACCEPTED:
            fx(f"thd-fx-2ch-tones-{tag}24", [("2ch", f"fx_tones_20_{tag}")])
    fx("thd-fx-2ch-tones-48k16", [("2ch", "fx_tones_20_16")])
    fx("thd-fx-2ch-tones-48k20", [("2ch", "fx_tones_20_b20")])
    fx("thd-fx-2ch-music-48k24-drc-film_standard",
       [("2ch:drc_profile=film_standard", "fx_music_20")])
    fx("thd-fx-2ch-music-48k24-timecode", [("2ch", "fx_music_20")],
       extra_args=["--embed-timecode", "01:00:00:00", "--timecode-frame-rate", "25"])

    # L: the 300 s programmes, made in their own set directory.
    for pres, layout in (("2ch", "20"), ("6ch", "51"), ("8ch", "71")):
        op(f"thd-long-{pres}-programme_300-48k24", [(pres, f"programme_{layout}_300")],
           group="truehd-long")


def object_legs(masters, atmos_masters=None):
    """The objects group: each Atmos master given to each encoder that takes one."""
    legs = []
    for name, path in masters.items():
        common = {"group": "objects", "source": str(path), "refusal_ok": True, "master": True}
        legs += [
            {**common, "name": f"obj-ddpjoc-768-{name}", "encoder": DDPJOC, "codec": "eac3-joc",
             "options": ["--input-format", "atmos_mezz", "--data-rate", "768"]},
            {**common, "name": f"obj-ddp-448-{name}", "encoder": DDP, "codec": "eac3",
             "options": ["--input-format", "atmos_mezz", "--data-rate", "448", "--encoder",
                         "ddp"]},
            {**common, "name": f"obj-thd-atmos-{name}", "encoder": DTHD, "codec": "truehd",
             "options": ["--presentation", "atmos", "-i", "{master}"], "use_note": TRUEHD_NOTE},
        ]
    # Masters the owner names (--atmos-master), read in place: the Atmos presentation alone, and
    # with the lower presentations beside it.
    for name, path in (atmos_masters or {}).items():
        common = {"group": "truehd-open", "source": str(path), "refusal_ok": True,
                  "master": True, "encoder": DTHD, "codec": "truehd", "use_note": TRUEHD_NOTE}
        legs += [
            {**common, "name": f"obj-thd-atmos-{name}",
             "options": ["--presentation", "atmos", "-i", "{master}"]},
            {**common, "name": f"obj-thd-atmos-{name}-dmx",
             "options": ["--presentation", "atmos", "-i", "{master}", "--presentation", "8ch",
                         "--presentation", "6ch", "--presentation", "2ch"]},
        ]
    return legs


# ------------------------------------------------------------------------------------------
# One leg, end to end (worker processes)
# ------------------------------------------------------------------------------------------

def guard_not_ci():
    if os.environ.get("GITHUB_ACTIONS"):
        raise SystemExit(
            "gen_dee_gold.py invokes licensed, non-CI-safe tooling (Dolby DEE) and must never "
            "run in a CI job - refusing because GITHUB_ACTIONS is set.")


def dee_exe(encoder):
    return DEE_DIR / f"{encoder}.exe"


def dee_version(encoder):
    result = subprocess.run([str(dee_exe(encoder)), "-h"], capture_output=True, text=True,
                            check=False)
    for line in result.stdout.splitlines():
        if "Dolby Encoding Engine version" in line:
            return line.strip()
    return "unknown"


def tool_version(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    lines = [line.strip() for line in (result.stdout or result.stderr).splitlines()
             if line.strip()]
    return " ".join(lines[:3]) if lines else "unknown"


def run_logged(cmd, cwd, log, timeout=3600):
    try:
        result = subprocess.run(cmd, cwd=cwd, capture_output=True, check=False, timeout=timeout)
        output = (result.stdout + result.stderr).decode("utf-8", "replace")
        code = result.returncode
    except subprocess.TimeoutExpired as exc:
        output = ((exc.stdout or b"") + (exc.stderr or b"")).decode("utf-8", "replace")
        output += f"\n(timed out after {timeout} s)"
        code = -1
    Path(log).write_text(subprocess.list2cmdline(cmd) + "\n\n" + output, encoding="utf-8")
    return code, output


def dee_log_facts(text):
    warnings = sorted({line.split("WARNING: ", 1)[1].strip() for line in text.splitlines()
                       if "WARNING: " in line})
    loudness = {}
    for match in re.finditer(r"\[([A-Za-z ]*loudness)\] ([A-Za-z_]+)=(\S+)", text):
        loudness.setdefault(match.group(1), {})[match.group(2)] = match.group(3)
    return warnings, loudness


def dee_errors(text):
    return list(dict.fromkeys(line.split("ERROR: ", 1)[1].strip()
                              for line in text.splitlines() if "ERROR: " in line))


def split_wav_list(source, work):
    """The source's channels as mono WAVs named by speaker in the leg's directory; DEE's -i
    argument names them from there, since a drive letter's colon would split the list."""
    with wave.open(str(source), "rb") as r:
        channels, width, rate = r.getnchannels(), r.getsampwidth(), r.getframerate()
        raw = np.frombuffer(r.readframes(r.getnframes()), dtype=np.uint8)
    frames = raw.reshape(-1, channels, width)
    speakers = {1: SPEAKERS["10"], 2: SPEAKERS["20"], 6: SPEAKERS["51"],
                8: SPEAKERS["71"]}[channels]
    files = {}
    for i, speaker in enumerate(speakers):
        with tempfile.SpooledTemporaryFile() as f:
            with wave.open(f, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(width)
                w.setframerate(rate)
                w.writeframes(np.ascontiguousarray(frames[:, i, :]).tobytes())
            f.seek(0)
            files[work / f"in_{speaker}.wav"] = f.read()
    return ":".join(p.name for p in files), files


def wav_list_argument(source, work, slots):
    """DEE's wav_list argument for the source's channels in the given slots ('-' for a silent
    one), and the mono WAVs it names, written to the leg's directory before the run."""
    with wave.open(str(source), "rb") as r:
        channels, width, rate = r.getnchannels(), r.getsampwidth(), r.getframerate()
        raw = np.frombuffer(r.readframes(r.getnframes()), dtype=np.uint8)
    frames = raw.reshape(-1, channels, width)
    files, names = {}, []
    for slot in slots:
        if slot is None:
            names.append("-")
            continue
        with tempfile.SpooledTemporaryFile() as f:
            with wave.open(f, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(width)
                w.setframerate(rate)
                w.writeframes(np.ascontiguousarray(frames[:, slot, :]).tobytes())
            f.seek(0)
            files[work / f"in_{slot}.wav"] = f.read()
        names.append(f"in_{slot}.wav")
    return ":".join(names), files


def leg_command(leg, sources, outs, work):
    """DEE's command line for a leg, and the files to write before it runs."""
    exe = str(dee_exe(leg["encoder"]))
    files = {}
    if leg["encoder"] == DDP:
        if leg.get("master"):
            inputs = ["-i", str(sources[0])]
            options = list(leg["options"])
        else:
            argument, files = split_wav_list(sources[0], work)
            inputs = ["--input-format", "wav_list", "-i", argument]
            options = list(leg["options"])
        return [exe, *inputs, "-o", str(outs[0]), "--overwrite", "1", "--temp-dir", str(work),
                *options, *leg.get("extra_args", [])], files
    if leg["encoder"] == DDPJOC:
        options = list(leg["options"])
        pairs = []
        for source in sources:
            pairs += ["-i", str(source)]
        for out in outs:
            pairs += ["-o", str(out)]
        return [exe, *options[:2], *pairs, "--overwrite", "1", *options[2:]], files
    # TrueHD: each "{name}" placeholder is that presentation's source path.
    by_name = {Path(s).stem: s for s in sources}
    options = []
    for o in leg["options"]:
        if o.startswith("{wavlist:"):
            argument, listed = wav_list_argument(by_name[o[9:-1]], work, leg["wav_slots"][o[9:-1]])
            files.update(listed)
            options.append(argument)
        elif o.startswith("{") and o[1:-1] in by_name:
            options.append(by_name[o[1:-1]])
        else:
            options.append(str(sources[0]) if o == "{master}" else o)
    return [exe, *options, "-o", str(outs[0]), "--overwrite", "1", "--temp-dir", str(work)], files


def mediainfo(stream, work, suffix, details):
    """MediaInfo's summary with --ParseSpeed=1, and its --Details=1 trace: of every frame for
    AC-3 and E-AC-3 up to 60 s, at MediaInfo's default parse speed for TrueHD (a trace of every
    40-sample access unit runs to 16 MB for 10 s of 7.1), none beyond 60 s."""
    files = {}
    runs = [(["--Output=JSON", "--ParseSpeed=1"], f"mediainfo{suffix}.json")]
    if details:
        speed = [] if stream.suffix == ".mlp" else ["--ParseSpeed=1"]
        runs.insert(0, (["--Details=1", *speed], f"mediainfo-details{suffix}.txt"))
        files["details_parse_speed"] = "default" if stream.suffix == ".mlp" else 1
    for args, name in runs:
        result = subprocess.run([str(MEDIAINFO), *args, str(stream)], capture_output=True,
                                check=False, timeout=3600)
        (work / name).write_bytes(result.stdout)
        files[name] = hashlib.sha256(result.stdout).hexdigest()
    return files


def mux_mp4(stream, work, fmt, suffix):
    mp4 = work / f"dee{suffix}.mp4"
    mp4.unlink(missing_ok=True)
    cmd = [str(MP4MUXER), "--overwrite", "1", "--temp-dir", str(work), "--track", str(stream),
           "--track-options", f"input_format={fmt}", "--output", str(mp4)]
    code, output = run_logged(cmd, work, work / f"dee{suffix}_mp4muxer_output.txt")
    if code != 0 or not mp4.is_file():
        return {"refused": dee_errors(output) or output.strip().splitlines()[-3:]}
    result = subprocess.run([str(MEDIAINFO), "--Output=JSON", str(mp4)], capture_output=True,
                            check=False, timeout=600)
    (work / f"mediainfo-mp4{suffix}.json").write_bytes(result.stdout)
    return {"file": mp4.name, "sha256": sha256(mp4), "size_bytes": mp4.stat().st_size,
            "muxer_warnings": [line.strip() for line in output.splitlines()
                               if line.strip().startswith("Warning")]}


def forge_probe(cli, stream):
    result = subprocess.run([str(cli), "probe", str(stream), "json=1"], capture_output=True,
                            check=False, timeout=600)
    if result.returncode != 0:
        text = (result.stderr + result.stdout).decode("utf-8", "replace").strip().splitlines()
        return {"error": f"exit {result.returncode}: {(text or [''])[0][:300]}"}
    try:
        info = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        return {"error": f"unreadable JSON: {exc}"}
    # A TrueHD or MLP stream's facts are under "truehd", not "stream".
    if "truehd" in info:
        return info["truehd"]
    try:
        stream_info = info["stream"]
    except KeyError as exc:
        return {"error": f"unreadable JSON: {exc}"}
    keep = ("codec", "bsid", "bsmod", "acmod", "lfeon", "layout", "coded_channels",
            "rendered_channels", "substreams", "access_units", "syncframes", "bitrate_kbps",
            "nominal_bitrate_kbps", "variable_bitrate", "metadata", "objects", "integrity",
            "tools")
    return {k: stream_info[k] for k in keep if k in stream_info}


def wav_shape(path):
    with open(path, "rb") as f:
        head = f.read(65536)
    if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
        return None
    pos, channels, block = 12, None, None
    while pos + 8 <= len(head):
        chunk, size = head[pos:pos + 4], struct.unpack_from("<I", head, pos + 4)[0]
        if chunk == b"fmt ":
            channels = struct.unpack_from("<H", head, pos + 10)[0]
            block = struct.unpack_from("<H", head, pos + 20)[0]
        elif chunk == b"data" and channels and block:
            return channels, size // block
        pos += 8 + size + (size & 1)
    return None


def forge_decode(cli, stream, scratch, extra=(), tag=""):
    scratch.mkdir(parents=True, exist_ok=True)
    wav = scratch / f"{stream.parent.name}-{stream.stem}{tag}.wav"
    wav.unlink(missing_ok=True)
    result = subprocess.run([str(cli), "decode", str(stream), str(wav), *extra],
                            capture_output=True, check=False, timeout=3600)
    shape = wav_shape(wav) if wav.is_file() else None
    wav.unlink(missing_ok=True)
    if result.returncode == 0 and shape:
        text = (result.stdout + result.stderr).decode("utf-8", "replace")
        objects = [line.strip() for line in text.splitlines() if "objects" in line]
        return f"ok: {shape[0]} channels, {shape[1]} samples" + (
            f"; {objects[0]}" if objects else "")
    text = (result.stderr + result.stdout).decode("utf-8", "replace").strip().splitlines()
    picked = [line for line in text if "error" in line.lower() or "unsupported" in line.lower()]
    return f"exit {result.returncode}: {(picked or text or [''])[0].strip()[:300]}"


def read_pcm(path):
    """(integer samples scaled to 24 bits, shape (n, channels), rate) of a PCM WAV."""
    with wave.open(str(path), "rb") as r:
        n, channels, width, rate = (r.getnframes(), r.getnchannels(), r.getsampwidth(),
                                    r.getframerate())
        raw = np.frombuffer(r.readframes(n), dtype=np.uint8)
    if width == 3:
        b = raw.reshape(-1, 3).astype(np.int32)
        x = (b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)) << 8 >> 8
    elif width == 2:
        x = np.frombuffer(raw.tobytes(), dtype="<i2").astype(np.int32) << 8
    else:
        x = np.frombuffer(raw.tobytes(), dtype="<i4") >> 8
    return x.reshape(-1, channels), rate


def ffmpeg_check(stream, scratch, source=None):
    """FFmpeg's decode of the stream: "ok" or its first errors; with a source (TrueHD), whether
    the decode equals it sample for sample, each source channel matched to a decoded one."""
    out = {}
    # FFmpeg's probe can take a TrueHD stream for DVD-Audio MLP ("MLP only supports up to 2
    # substreams"), so the TrueHD legs name the demuxer.
    demuxer = ["-f", "truehd"] if stream.suffix == ".mlp" else []
    if demuxer:
        out["demuxer"] = "truehd"
    strict = subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-err_detect",
                             "crccheck+bitstream+buffer+explode", "-f", "truehd", "-i",
                             str(stream), "-f", "null", "-"], capture_output=True, check=False,
                            timeout=3600)
    strict_errors = (strict.stderr or b"").decode("utf-8", "replace").strip().splitlines()
    out["strict"] = ("ok" if strict.returncode == 0 and not strict_errors
                     else f"exit {strict.returncode}: " + " | ".join(dict.fromkeys(
                         strict_errors))[:400])
    result = subprocess.run(["ffmpeg", "-v", "error", *demuxer, "-i", str(stream), "-f", "null",
                             "-"], capture_output=True, check=False, timeout=3600)
    errors = (result.stderr or b"").decode("utf-8", "replace").strip().splitlines()
    out["decode"] = ("ok" if result.returncode == 0 and not errors
                     else f"exit {result.returncode}: " + " | ".join(dict.fromkeys(errors))[:400])
    if source is None:
        return out
    scratch.mkdir(parents=True, exist_ok=True)
    wav = scratch / f"{stream.parent.name}-{stream.stem}-ffmpeg.wav"
    subprocess.run(["ffmpeg", "-v", "error", "-y", *demuxer, "-i", str(stream), "-c:a",
                    "pcm_s24le", str(wav)], capture_output=True, check=False, timeout=3600)
    if not wav.is_file():
        out["lossless"] = None
        return out
    dec, drate = read_pcm(wav)
    ref, _ = read_pcm(source)
    wav.unlink(missing_ok=True)
    out.update({"decoded_channels": int(dec.shape[1]), "decoded_rate": int(drate),
                "decoded_samples": len(dec), "source_samples": len(ref)})
    if len(dec) == 0:
        out["lossless"] = None
        return out
    probe = min(1000, len(ref))
    for offset in range(0, 8192):
        m = min(len(dec) - offset, len(ref))
        if m <= probe:
            break
        # Each source channel takes the first unused decoded channel equal to it over the first
        # thousand samples (a source can carry one signal in several channels).
        mapping, used = [], set()
        for c in range(ref.shape[1]):
            hit = [d for d in range(dec.shape[1]) if d not in used
                   and np.array_equal(dec[offset:offset + probe, d], ref[:probe, c])]
            mapping.append(hit[0] if hit else None)
            used.update(hit[:1])
        if all(h is not None for h in mapping):
            diff = dec[offset:offset + m][:, mapping] - ref[:m]
            where = np.nonzero(np.any(diff != 0, axis=1))[0]
            out.update({"lossless": len(where) == 0, "offset": offset, "channel_map": mapping,
                        "mismatched_samples": int(np.count_nonzero(diff))})
            if len(where):
                out.update({"max_abs_error_24bit": int(np.max(np.abs(diff))),
                            "mismatch_seconds": [round(float(where[0]) / drate, 3),
                                                 round(float(where[-1]) / drate, 3)]})
            return out
    # Not sample-exact at any offset: each source channel against the decoded channel it
    # correlates with most (normalised), the error's level against the source's, and the
    # largest difference in 24-bit units.
    m = min(len(dec), len(ref))
    decoded = dec[:m].astype(np.float64)
    norms = np.sqrt(np.sum(decoded * decoded, axis=0)) + 1e-30
    errors, largest, mapping = [], 0, []
    for c in range(ref.shape[1]):
        r = ref[:m, c].astype(np.float64)
        best = int(np.argmax(np.abs(r @ decoded) / norms))
        mapping.append(best)
        err = decoded[:, best] - r
        power = float(np.dot(r, r)) or 1.0
        errors.append(round(10.0 * np.log10(max(float(np.dot(err, err)), 1e-30) / power), 1))
        largest = max(largest, int(np.max(np.abs(err))))
    out.update({"lossless": False, "channel_map": mapping, "error_db_per_channel": errors,
                "max_abs_error_24bit": largest})
    return out


def presentations_wanted(leg):
    """Whether each presentation of the stream is decoded in turn: legs with more than one
    presentation, and the Atmos and sixteen-channel ones."""
    return (leg["options"].count("--presentation") > 1
            or leg["name"].startswith(("obj-thd-atmos", "thd-16ch")))


def run_leg(task):
    """Makes one leg in DIR/streams/<name> (reusing streams whose command and sources are
    unchanged) and describes it: (name, manifest entry)."""
    leg, root, old = task["leg"], Path(task["root"]), task["old"] or {}
    cli = task["cli"]
    scratch = Path(task["scratch"])
    sources = [Path(p) for p, _ in task["sources"]]
    shas = [sha for _, sha in task["sources"]]
    name = leg["name"]
    work = root / "streams" / name
    work.mkdir(parents=True, exist_ok=True)
    ext = {"ac3": "ac3", "eac3": "ec3", "eac3-bluray": "ec3", "eac3-joc": "ec3",
           "truehd": "mlp"}[leg["codec"]]
    count = leg.get("parts") or leg.get("outputs") or 1
    streams = [work / (f"dee.{ext}" if i == 0 else f"dee-{i + 1}.{ext}") for i in range(count)]
    if leg.get("parts"):
        part_paths = [Path(task["root"]) / "sources" / f"{sources[0].stem}_part{i + 1}.wav"
                      for i in range(count)]
        command_sources = part_paths
    else:
        command_sources = sources
    cmd, files = leg_command(leg, command_sources, streams, work)
    stamp = {"command": subprocess.list2cmdline(cmd), "source_sha256": shas}
    leg_json = work / "leg.json"
    reusable = all(s.is_file() for s in streams) and leg_json.is_file()
    if reusable:
        reusable = json.loads(leg_json.read_text(encoding="utf-8")) == stamp
    fresh = not reusable
    entry = {"group": leg["group"], "for": G2_GROUPS[leg["group"]], "encoder": leg["encoder"],
             "codec": leg["codec"], "source": leg["source"] if not leg.get("master")
             else str(leg["source"]), "sources": [s.name for s in sources],
             "source_sha256": shas, "options": leg["options"] + leg.get("extra_args", []),
             "command": stamp["command"]}
    for key in ("kbps", "layout", "audio_altered", "use_note", "parts", "outputs",
                "wav_slots"):
        if key in leg:
            entry[key] = leg[key]
    if fresh:
        for stream in streams:
            stream.unlink(missing_ok=True)
        for path, data in files.items():
            path.write_bytes(data)
        code, output = run_logged(cmd, work, work / "dee_output.txt")
        for path in files:
            path.unlink(missing_ok=True)
        if code != 0 or not all(s.is_file() for s in streams):
            entry["refused" if leg.get("refusal_ok") else "failed"] = (
                dee_errors(output) or output.strip().splitlines()[-5:])
            return name, entry
        leg_json.write_text(json.dumps(stamp, indent=2) + "\n", encoding="utf-8")
    output = (work / "dee_output.txt").read_text(encoding="utf-8", errors="replace")
    entry["dee_warnings"], entry["dee_loudness"] = dee_log_facts(output)
    long_leg = task["seconds"] > PROGRAMME_SECONDS
    lossless_source = (sources[0] if leg["codec"] == "truehd" and not leg.get("master")
                       else None)
    olds = old.get("streams") or [old]
    described = []
    for i, stream in enumerate(streams):
        suffix = "" if i == 0 else f"-{i + 1}"
        previous = olds[i] if i < len(olds) else {}
        d = {"stream": stream.name, "size_bytes": stream.stat().st_size,
             "sha256": sha256(stream)}
        if fresh or not (work / f"mediainfo{suffix}.json").is_file():
            d["mediainfo"] = mediainfo(stream, work, suffix, details=not long_leg)
        else:
            d["mediainfo"] = previous.get("mediainfo") or mediainfo(stream, work, suffix,
                                                                    details=not long_leg)
        if ext != "mlp":
            if fresh or not (work / f"dee{suffix}.mp4").is_file() or "mp4" not in previous:
                d["mp4"] = mux_mp4(stream, work, "ac3" if ext == "ac3" else "eac3", suffix)
            else:
                d["mp4"] = previous["mp4"]
        if ext == "mlp":
            # DEE's own log and encode-request files, hashed and not read.
            reports = {r.name: sha256(r) for r in sorted(work.glob(f"{stream.name}.*"))}
            if reports:
                d["reports"] = reports
        if cli:
            d["forge_probe"] = forge_probe(cli, stream)
            d["forge_decode"] = forge_decode(cli, stream, scratch / "decode")
            if ext == "mlp" and presentations_wanted(leg):
                d["forge_presentations"] = {
                    p: forge_decode(cli, stream, scratch / "decode",
                                    [f"truehd-presentation={p}"], f"-{p}")
                    for p in ("2ch", "6ch", "8ch", "all")}
        else:
            for key in ("forge_probe", "forge_decode", "forge_presentations"):
                if key in previous:
                    d[key] = previous[key]
        d["ffmpeg"] = ffmpeg_check(stream, scratch / "decode", lossless_source)
        described.append(d)
    entry.update(described[0])
    if len(described) > 1:
        entry["streams"] = described
    return name, entry


# ------------------------------------------------------------------------------------------
# The set
# ------------------------------------------------------------------------------------------

def gold_run(args, version):
    root = args.gold_set.resolve()
    root.mkdir(parents=True, exist_ok=True)
    manifest_path = root / MANIFEST
    old = (json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.is_file()
           else {})
    legs = g2_legs()
    masters = {}
    if args.masters and args.masters.is_dir():
        for child in sorted(args.masters.iterdir()):
            master = child / "adm-master.wav"
            if master.is_file():
                masters[child.name] = master
    legs += object_legs(masters, args.atmos_master)
    names = [leg["name"] for leg in legs]
    dupes = sorted({n for n in names if names.count(n) > 1})
    if dupes:
        raise SystemExit(f"leg names repeat: {dupes}")
    chosen = [leg for leg in legs if not args.only or any(p in leg["name"] for p in args.only)]
    plan = source_plan()
    needed = sorted({s for leg in chosen if not leg.get("master")
                     for s in leg.get("sources", [leg["source"]])})
    unknown = [n for n in needed if n not in plan]
    if unknown:
        raise SystemExit(f"legs name sources no plan builds: {unknown}")
    sources = dict(old.get("sources", {}))
    sources.update(build_sources(root, needed))
    for leg in chosen:
        if leg.get("parts"):
            # A gapless leg's source cut in equal parts, each its own WAV beside the source.
            path = root / "sources" / f"{leg['source']}.wav"
            with wave.open(str(path), "rb") as r:
                params, frames = r.getparams(), r.readframes(r.getnframes())
            block = params.nchannels * params.sampwidth
            n = len(frames) // block
            for i in range(leg["parts"]):
                a, b = i * n // leg["parts"], (i + 1) * n // leg["parts"]
                part = root / "sources" / f"{leg['source']}_part{i + 1}.wav"
                with tempfile.SpooledTemporaryFile() as f:
                    with wave.open(f, "wb") as w:
                        w.setparams(params)
                        w.writeframes(frames[a * block:b * block])
                    f.seek(0)
                    data = f.read()
                if not (part.is_file() and part.read_bytes() == data):
                    replace_file(part, data)
    scratch = args.scratch_dir.resolve()
    tasks = []
    for leg in chosen:
        if leg.get("master"):
            paths = [Path(leg["source"])]
        else:
            paths = [root / "sources" / f"{s}.wav" for s in leg.get("sources", [leg["source"]])]
        shas = [sources[p.stem]["sha256"] if p.stem in sources else sha256(p) for p in paths]
        seconds = max(sources.get(p.stem, {}).get("seconds", SECONDS) for p in paths)
        pairs = [(str(p), s) for p, s in zip(paths, shas, strict=True)]
        tasks.append({"leg": leg, "root": str(root), "sources": pairs,
                      "cli": str(args.cli.resolve()) if args.cli else None,
                      "scratch": str(scratch), "old": old.get("legs", {}).get(leg["name"]),
                      "seconds": seconds})
    made = {}
    started = time.monotonic()
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_leg, task): task["leg"]["name"] for task in tasks}
        for number, future in enumerate(as_completed(futures), 1):
            try:
                name, entry = future.result()
            except Exception as exc:
                # One leg's checks failing is recorded against it; it does not stop the set.
                name = futures[future]
                entry = {"failed": [f"the generator's checks raised {exc!r}"]}
            made[name] = entry
            state = ("refused" if "refused" in entry else "FAILED" if "failed" in entry
                     else entry.get("forge_decode", "made")[:60])
            extra = ""
            if entry.get("ffmpeg", {}).get("lossless") is not None:
                extra = f"; lossless {entry['ffmpeg']['lossless']}"
            print(f"[{number}/{len(tasks)} {time.monotonic() - started:6.0f} s] {name}: "
                  f"{state}{extra}", flush=True)
    entries = {}
    for leg in legs:
        if leg["name"] in made:
            entries[leg["name"]] = made[leg["name"]]
        elif leg["name"] in old.get("legs", {}):
            entries[leg["name"]] = old["legs"][leg["name"]]
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    manifest = {
        "gold_version": GOLD_VERSION, "made": old.get("made", now), "updated": now,
        "dee_version": version, "mediainfo_version": tool_version([str(MEDIAINFO),
                                                                   "--Version"]),
        "ffmpeg_version": tool_version(["ffmpeg", "-version"]).split(" Copyright")[0],
        "forge": tool_version([str(args.cli.resolve()), "--version"]) if args.cli
        else old.get("forge"),
        "groups": G2_GROUPS, "dee_cannot": DEE_CANNOT, "sources": sources, "legs": entries,
    }
    replace_file(manifest_path, (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
    failed = sorted(n for n, e in entries.items() if "failed" in e)
    refused = sorted(n for n, e in entries.items() if "refused" in e)
    print(f"wrote {manifest_path}: {len(entries)} legs, {len(refused)} refused by DEE, "
          f"{len(failed)} failed")
    if failed:
        print(f"  failed: {failed}")
        raise SystemExit(1)


# The legs committed under testdata/truehd-dee/, in the order the budget is spent: block J of the
# work order, then the short block F legs.
FIXTURE_LEGS = (
    "thd-fx-8ch-tones-48k24-dmx6-dmx2", "thd-fx-6ch-music-48k24", "thd-fx-2ch-music-48k24",
    "thd-fx-2ch-music-96k24", "thd-fx-8ch-tones-96k24", "thd-fx-2ch-tones-88k24",
    "thd-fx-2ch-tones-176k24", "thd-fx-2ch-tones-192k24", "thd-fx-2ch-tones-48k16",
    "thd-fx-2ch-tones-48k20", "thd-fx-2ch-music-48k24-drc-film_standard",
    "thd-fx-2ch-music-48k24-timecode", "thd-len-2ch-48k-12000", "thd-len-2ch-48k-12001",
    "thd-len-2ch-48k-12039", "thd-len-2ch-48k-4001")
FIXTURE_LEG_BYTES = 768 * 1024
FIXTURE_TOTAL_BYTES = 4 * 1024 * 1024
# Lines of DEE's log that are dropped from the committed copy: anything naming the entitlement, a
# key, a host, a user or a machine, or holding a MAC address.
LOG_DROP = re.compile(r"licen[sc]e|seria[l]|key|host|user|machine|computer|"
                      r"([0-9a-f]{2}[:-]){5}[0-9a-f]{2}", re.IGNORECASE)


def sanitised(text, root):
    """(text with its absolute paths made relative to the set, the lines dropped)."""
    own = [v for v in (getpass.getuser(), platform.node()) if v]
    kept, dropped = [], 0
    for line in text.splitlines():
        if LOG_DROP.search(line) or any(v.lower() in line.lower() for v in own):
            dropped += 1
            continue
        kept.append(line)
    out = "\n".join(kept) + "\n"
    return relative_paths(out, root), dropped


def relative_paths(text, root):
    for form in (str(root), root.as_posix()):
        text = text.replace(form, ".")
    return re.sub(r'"[^"]*dee_[a-z0-9]+_encoder\.exe"|[^\s"]*dee_[a-z0-9]+_encoder\.exe',
                  "dee_dthd_encoder", text)


def write_fixture_copy(root, dest):
    """DIR/dee-gold-manifest.json cut to FIXTURE_LEGS and their sources, and the streams, logs and
    sources themselves, laid out as a gold set (score_truehd_decode.py --set reads it as it is).
    Legs that are not in the manifest, were refused, or do not fit the budget are named and left
    out."""
    manifest = json.loads((root / MANIFEST).read_text(encoding="utf-8"))
    dest.mkdir(parents=True, exist_ok=True)
    legs, sources, total, dropped, counted = {}, {}, 0, 0, set()
    for name in FIXTURE_LEGS:
        entry = manifest["legs"].get(name)
        if not entry or "refused" in entry or "failed" in entry:
            print(f"{name}: not in the set or not made; left out")
            continue
        stream = root / "streams" / name / "dee.mlp"
        log, lost = sanitised((root / "streams" / name / "dee_output.txt").read_text(
            encoding="utf-8", errors="replace"), root)
        files = [(f"streams/{name}/dee.mlp", stream.read_bytes()),
                 (f"streams/{name}/dee_output.txt", log.encode("utf-8"))]
        for source in entry["sources"]:
            files.append((f"sources/{source}", (root / "sources" / source).read_bytes()))
        cost = sum(len(d) for p, d in files if p.startswith("streams")
                   or p not in counted)
        if cost > FIXTURE_LEG_BYTES or total + cost > FIXTURE_TOTAL_BYTES:
            print(f"{name}: {cost} bytes over the budget; left out")
            continue
        total += cost
        counted.update(p for p, _ in files)
        dropped += lost
        for path, data in files:
            (dest / path).parent.mkdir(parents=True, exist_ok=True)
            (dest / path).write_bytes(data)
        copy = {k: v for k, v in entry.items() if k not in ("mediainfo", "reports")}
        copy["command"] = relative_paths(entry["command"], root).replace("\\", "/")
        copy["options"] = [relative_paths(o, root) for o in entry["options"]]
        copy["source"] = relative_paths(entry["source"], root)
        copy["sources"] = [Path(s).stem + ".wav" for s in entry["sources"]]
        copy["files"] = {"dee.mlp": entry["sha256"],
                         "dee_output.txt": hashlib.sha256(log.encode("utf-8")).hexdigest()}
        legs[name] = copy
        for source in entry["sources"]:
            sources[Path(source).stem] = manifest["sources"][Path(source).stem]
    out = {"_comment": ("The TrueHD fixtures of tools/generators/gen_dee_gold.py (GOLD_VERSION 2), "
                        "cut from the 2026-10-29 gold set to the legs below and their sources: "
                        "DEE's streams, each with its source and DEE's log. The set's root is "
                        "'.', the logs lose the lines that name the machine, its user or its "
                        "entitlement, and MediaInfo's and DEE's own report digests "
                        "are left out. They are test inputs, decoded and compared with their "
                        "sources and not analysed (docs/verification.md, \"Scoring DEE's TrueHD "
                        "streams\")."),
           **{k: manifest[k] for k in ("gold_version", "made", "updated", "dee_version",
                                       "mediainfo_version", "ffmpeg_version", "forge")},
           "groups": {g: manifest["groups"][g] for g in {e["group"] for e in legs.values()}},
           "sources": sources, "legs": legs}
    (dest / MANIFEST).write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {dest}: {len(legs)} legs, {total} bytes of streams, sources and logs, "
          f"{dropped} log lines dropped")


def main():
    guard_not_ci()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gold-set", type=Path, required=True, metavar="DIR",
                        help="the set's directory, e.g. D:/ac3bld/dee-gold")
    parser.add_argument("--scratch-dir", type=Path, default=REPO / "build" / "dee_gold_scratch",
                        help="where decodes go while they are checked")
    parser.add_argument("--cli", type=Path, metavar="ICLFORGE_CLI",
                        help="a forge whose probe and decode are recorded for each stream")
    parser.add_argument("--jobs", type=int, default=4, help="legs made at once (default 4)")
    parser.add_argument("--only", nargs="+", metavar="NAME_PART",
                        help="make only the legs whose names contain one of these; the others "
                             "keep their manifest entries")
    parser.add_argument("--atmos-master", action="append", default=[], metavar="NAME=PATH",
                        help="an Atmos master (DAMF, BWF ADM or MXF IAB) for the TrueHD Atmos "
                             "legs obj-thd-atmos-NAME and -NAME-dmx, read in place")
    parser.add_argument("--fixtures-out", type=Path, metavar="DIR",
                        help="write the committed copy of the TrueHD fixtures (FIXTURE_LEGS) "
                             "from --gold-set to DIR and stop; no DEE is run")
    parser.add_argument("--masters", type=Path, default=DEFAULT_MASTERS,
                        help="a directory of <name>/adm-master.wav Atmos masters for the "
                             f"objects group (default {DEFAULT_MASTERS.as_posix()})")
    args = parser.parse_args()
    args.atmos_master = {n: Path(p) for n, _, p in (a.partition("=") for a in args.atmos_master)}
    if args.fixtures_out:
        write_fixture_copy(args.gold_set.resolve(), args.fixtures_out.resolve())
        return
    for encoder in ENCODERS:
        if not dee_exe(encoder).exists():
            raise SystemExit(f"{dee_exe(encoder)} not found - this generator only runs on a "
                             "machine with Dolby Media Encoder installed.")
    versions = {dee_version(encoder) for encoder in ENCODERS}
    if len(versions) != 1:
        raise SystemExit(f"{ENCODERS} report different DEE versions: {sorted(versions)}")
    gold_run(args, versions.pop())


if __name__ == "__main__":
    main()
