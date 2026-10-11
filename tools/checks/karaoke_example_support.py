"""What the tests of the karaoke examples (examples/karaoke/*) read a stream and its words with.

test_gen_karaoke_example.py and test_gen_karaoke_duet_example.py both hold a generated stream to
what it should be without a build: the stream's own bytes are read here as the syntax of ETSI
TS 102 366 5.4.1 gives them, and a lyrics row is read for its time stamps.

Standard library only, as the rest of this directory.
"""

import re

# Table 4.13 (frmsizecod) for 48 kHz: the rate each pair of codes gives, in kbit/s.
BITRATES_KBPS = (32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
                 576, 640)
SAMPLES_PER_FRAME = 1536


def frames_of(stream):
    """The syncframes of an AC-3 stream as (fscod, frmsizecod, bsid, bsmod, acmod, lfeon),
    each held to the sync word and to the size its header gives."""
    frames = []
    at = 0
    while at < len(stream):
        assert stream[at : at + 2] == b"\x0b\x77", f"no sync word at {at}"
        fscod = stream[at + 4] >> 6
        frmsizecod = stream[at + 4] & 0x3F
        bsid = stream[at + 5] >> 3
        bsmod = stream[at + 5] & 0x07
        acmod = stream[at + 6] >> 5
        # After acmod: cmixlev when acmod has a centre and is not 1/0, surmixlev when it has a
        # surround, dsurmod at 2/0, then lfeon.
        bits = 3
        value = stream[at + 6]
        if acmod & 1 and acmod != 1:
            bits += 2
        if acmod & 4:
            bits += 2
        if acmod == 2:
            bits += 2
        lfeon = (value >> (7 - bits)) & 1
        assert fscod == 0, "not 48 kHz"
        size = BITRATES_KBPS[frmsizecod >> 1] * 4
        frames.append((fscod, frmsizecod, bsid, bsmod, acmod, lfeon))
        at += size
    assert at == len(stream), "the last frame is cut short"
    return frames


def timestamps(row):
    """Every [mm:ss.xx] and <mm:ss.xx> in a lyrics row, in centiseconds, in order."""
    return [
        (int(m) * 60 + int(s)) * 100 + int(cs)
        for m, s, cs in re.findall(r"[\[<](\d\d):(\d\d)\.(\d\d)[\]>]", row)
    ]
