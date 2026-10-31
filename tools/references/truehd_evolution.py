#!/usr/bin/env python3
"""An independent transcription of the TrueHD extra data and of the evolution frame inside it.

PROVISIONAL, OWNER TO CONFIRM (libs/truehd/ERRATA.md, "Atmos, spec v3.6", question 22). It is
written from the owner's TrueHD (MLP) Reference Specification v3.6, sections 16.1, 16.2 and 16.4
(the extra data region, its check nibble and its parity) and section 15.4 (evo_frame()), in plain
Python with no code shared with libs/truehd. No stream this project has carries an evolution
frame, so the vectors are built here, by a writer that is also a transcription, and the C++ reader
(libs/truehd/include/iclforge/truehd/io/evolution.hpp) is held to what this module reads from them.

Two readings of section 15.4 (ERRATA questions 23 and 24):

  emdf    the EMDF container of ETSI TS 102 366 Annex H that section 15.4 renames: variable_bits
          are groups of n bits each followed by a read-more flag (ETSI TS 103 420 5.5.1), payload
          id 31 escapes to a longer id, smploffst is 11 bits and a reserved bit, and a primary
          protection length code of 0 is reserved.
  spec36  section 15.4 to the letter: variable_bits(n, max_ext) is an escape value (n bits and,
          when all are ones, n more, up to max_ext times), no payload id escape, smploffst is
          variable_bits(11, 2) with no reserved bit, a primary protection length code of 0 means
          no bytes.

    truehd_evolution.py write [PATH]    # regenerate testdata/truehd/evolution/vectors.json
    truehd_evolution.py check [PATH]    # regenerate in memory and compare with the file
    truehd_evolution.py parse HEX [--reading emdf|spec36]   # read one extra data region
"""

import argparse
import json
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
VECTORS = REPO / "testdata" / "truehd" / "evolution" / "vectors.json"

READINGS = ("emdf", "spec36")
SIZE_TABLE = (0, 1, 4, 16)  # protection bytes for the length codes 0 to 3 (section 15.4)
# section 15.4: the maximum extension reads of each variable_bits(n, max_ext)
EXT = {"version": 16, "key_id": 10, "smploffst": 2, "duration": 2, "groupid": 16, "size": 4}


# ---------------------------------------------------------------------------------------------
# bits


class Reader:
    """MSB first. Reading past the end gives zeros and sets `overflow`, as the C++ reader does."""

    def __init__(self, data):
        self.data = bytes(data)
        self.pos = 0
        self.overflow = False

    def total(self):
        return len(self.data) * 8

    def remaining(self):
        return max(0, self.total() - self.pos)

    def read(self, n):
        value = 0
        for _ in range(n):
            if self.pos >= self.total():
                self.overflow = True
                bit = 0
            else:
                bit = (self.data[self.pos >> 3] >> (7 - (self.pos & 7))) & 1
            value = (value << 1) | bit
            self.pos += 1
        return value


class Writer:
    def __init__(self):
        self.bits = []

    def put(self, value, n):
        assert 0 <= value < (1 << n), (value, n)
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def tobytes(self):
        bits = self.bits + [0] * (-len(self.bits) % 8)
        out = bytearray()
        for i in range(0, len(bits), 8):
            byte = 0
            for b in bits[i : i + 8]:
                byte = (byte << 1) | b
            out.append(byte)
        return bytes(out)


# ---------------------------------------------------------------------------------------------
# variable_bits, in both codings


def vb_read(r, reading, n, max_ext):
    if reading == "emdf":
        value = 0
        while True:
            value += r.read(n)
            more = r.read(1)
            if r.overflow or not more:
                return value
            value = (value << n) + (1 << n)
    ones = (1 << n) - 1
    group = r.read(n)
    value = group
    ext = 0
    while group == ones and ext < max_ext and not r.overflow:
        group = r.read(n)
        value += group
        ext += 1
    return value


def vb_write(w, reading, n, max_ext, value):
    if reading == "emdf":
        # k groups carry value - (2^n + 2^2n + ... + 2^((k-1)n)) in k*n bits
        k = 1
        while True:
            offset = sum(1 << (n * j) for j in range(1, k))
            if value - offset < (1 << (n * k)):
                break
            k += 1
        rest = value - sum(1 << (n * j) for j in range(1, k))
        for i in range(k):
            w.put((rest >> (n * (k - 1 - i))) & ((1 << n) - 1), n)
            w.put(1 if i < k - 1 else 0, 1)
        return
    ones = (1 << n) - 1
    assert value <= ones * (max_ext + 1), "beyond what variable_bits(n, max_ext) can say"
    remaining = value
    written = 0
    while True:
        if remaining < ones:
            w.put(remaining, n)
            return
        w.put(ones, n)
        remaining -= ones
        written += 1
        if written == max_ext + 1:  # the first group and max_ext extensions: the reader stops here
            assert remaining == 0
            return


# ---------------------------------------------------------------------------------------------
# evo_frame() (section 15.4): the writer


def default_config():
    return {
        "sample_offset": None,
        "duration": None,
        "group_id": None,
        "codec_data": None,
        "discard_unknown": False,
        "frame_aligned": False,
        "create_duplicate": False,
        "remove_duplicate": False,
        "priority": 0,
        "proc_allowed": 0,
    }


def write_config(w, reading, cfg):
    so = cfg["sample_offset"] is not None
    w.put(1 if so else 0, 1)
    if so:
        if reading == "emdf":
            w.put(cfg["sample_offset"], 11)
            w.put(0, 1)  # reserved
        else:
            vb_write(w, reading, 11, EXT["smploffst"], cfg["sample_offset"])
    w.put(1 if cfg["duration"] is not None else 0, 1)
    if cfg["duration"] is not None:
        vb_write(w, reading, 11, EXT["duration"], cfg["duration"])
    w.put(1 if cfg["group_id"] is not None else 0, 1)
    if cfg["group_id"] is not None:
        vb_write(w, reading, 2, EXT["groupid"], cfg["group_id"])
    w.put(1 if cfg["codec_data"] is not None else 0, 1)
    if cfg["codec_data"] is not None:
        w.put(cfg["codec_data"], 8)
    w.put(1 if cfg["discard_unknown"] else 0, 1)
    if not cfg["discard_unknown"]:
        aligned = False
        if not so:
            aligned = cfg["frame_aligned"]
            w.put(1 if aligned else 0, 1)
            if aligned:
                w.put(1 if cfg["create_duplicate"] else 0, 1)
                w.put(1 if cfg["remove_duplicate"] else 0, 1)
        if so or aligned:
            w.put(cfg["priority"], 5)
            w.put(cfg["proc_allowed"], 2)


def table56_config(**over):
    cfg = default_config()
    cfg["group_id"] = 0
    cfg["frame_aligned"] = True
    cfg.update(over)
    return cfg


def build_evo_frame(
    reading,
    version=0,
    key_id=0,
    payloads=(),
    primary=1,
    secondary=0,
    size_override=None,
    pad_bits=0,
):
    """payloads: [(id, config, bytes)]; primary/secondary: the protection length codes."""
    w = Writer()
    w.put(min(version, 3), 2)
    if version >= 3:
        vb_write(w, reading, 2, EXT["version"], version - 3)
    w.put(min(key_id, 7), 3)
    if key_id >= 7:
        vb_write(w, reading, 3, EXT["key_id"], key_id - 7)
    for pid, cfg, data in payloads:
        if reading == "emdf" and pid >= 31:
            w.put(31, 5)
            vb_write(w, reading, 5, 0, pid - 31)
        else:
            assert 0 < pid < 31 or (reading == "spec36" and pid < 32)
            w.put(pid, 5)
        write_config(w, reading, cfg)
        size = len(data) if size_override is None else size_override
        vb_write(w, reading, 8, EXT["size"], size)
        for b in data:
            w.put(b, 8)
    w.put(0, 5)
    w.put(primary, 2)
    w.put(secondary, 2)
    for code, tag in ((primary, 0xA0), (secondary, 0xB0)):
        for i in range(SIZE_TABLE[code]):
            w.put((tag + i) & 0xFF, 8)
    for _ in range(pad_bits):
        w.put(0, 1)
    return w.tobytes()


# ---------------------------------------------------------------------------------------------
# evo_frame() (section 15.4): the reader


def error(kind, at):
    return {"ok": False, "kind": kind, "bit_offset": at}


def read_config(r, reading):
    cfg = default_config()
    so = r.read(1)
    if so:
        if reading == "emdf":
            cfg["sample_offset"] = r.read(11)
            r.read(1)
        else:
            cfg["sample_offset"] = vb_read(r, reading, 11, EXT["smploffst"])
    if r.read(1):
        cfg["duration"] = vb_read(r, reading, 11, EXT["duration"])
    if r.read(1):
        cfg["group_id"] = vb_read(r, reading, 2, EXT["groupid"])
    if r.read(1):
        cfg["codec_data"] = r.read(8)
    cfg["discard_unknown"] = bool(r.read(1))
    if not cfg["discard_unknown"]:
        if not so:
            cfg["frame_aligned"] = bool(r.read(1))
            if cfg["frame_aligned"]:
                cfg["create_duplicate"] = bool(r.read(1))
                cfg["remove_duplicate"] = bool(r.read(1))
        if so or cfg["frame_aligned"]:
            cfg["priority"] = r.read(5)
            cfg["proc_allowed"] = r.read(2)
    return cfg


def parse_evo_frame(data, reading):
    if len(data) == 0:
        return error("empty", 0)
    r = Reader(data)
    version = r.read(2)
    if version == 3:
        version += vb_read(r, reading, 2, EXT["version"])
    key_id = r.read(3)
    if key_id == 7:
        key_id += vb_read(r, reading, 3, EXT["key_id"])
    if r.overflow:
        return error("truncated_header", 0)
    payloads = []
    while True:
        id_at = r.pos
        if r.remaining() < 5:
            return error("missing_terminator", id_at)
        pid = r.read(5)
        if pid == 0:
            break
        if reading == "emdf" and pid == 31:
            pid += vb_read(r, reading, 5, 0)
            if r.overflow:
                return error("truncated_payload_id", id_at)
        config_at = r.pos
        cfg = read_config(r, reading)
        if r.overflow:
            return error("truncated_config", config_at)
        size_at = r.pos
        size = vb_read(r, reading, 8, EXT["size"])
        if r.overflow:
            return error("truncated_size", size_at)
        if size * 8 > r.remaining():
            return error("truncated_payload", r.pos)
        body = bytes(r.read(8) for _ in range(size))
        payloads.append({"id": pid, "config": cfg, "bytes": body.hex()})
    prot_at = r.pos
    primary = r.read(2)
    secondary = r.read(2)
    if r.overflow:
        return error("truncated_protection", prot_at)
    if reading == "emdf" and primary == 0:
        return error("reserved_protection_length", prot_at)
    need = (SIZE_TABLE[primary] + SIZE_TABLE[secondary]) * 8
    if need > r.remaining():
        return error("truncated_protection", prot_at + 4)
    pb = bytes(r.read(8) for _ in range(SIZE_TABLE[primary]))
    sb = bytes(r.read(8) for _ in range(SIZE_TABLE[secondary]))
    used = r.pos
    trailing_zero = True
    while r.remaining():
        if r.read(1):
            trailing_zero = False
            break
    return {
        "ok": True,
        "version": version,
        "key_id": key_id,
        "payloads": payloads,
        "protection": {
            "primary_code": primary,
            "secondary_code": secondary,
            "primary": pb.hex(),
            "secondary": sb.hex(),
        },
        "bits_used": used,
        "trailing_zero": trailing_zero,
    }


# ---------------------------------------------------------------------------------------------
# the extra data region (section 16.1, 16.2, 16.4)


def check_nibble(length):
    """16.2: the nibble that makes the four nibbles of the header word XOR to 0xF."""
    return ((length >> 8) ^ (length >> 4) ^ length ^ 0xF) & 0xF


def parity(reserved, frame_length, tail):
    """16.4: seed 0xA9, the reserved field, the length, then the payload and padding bytes."""
    acc = reserved ^ frame_length ^ 0xA9
    for byte in tail:
        acc ^= byte
    return ((acc >> 8) ^ acc) & 0xFF


def build_extra_data(evo, padding=b"", reserved=0, bad_nibble=False, bad_parity=False, length=None):
    """The region from the header word to the parity byte. extra_data_length is the words less one
    (ERRATA question 19) unless `length` is given."""
    body = bytes(evo) + bytes(padding)
    tail_len = 2 + 2 + len(body) + 1
    words = (tail_len + 1) // 2
    if tail_len % 2:
        body += b"\x00"  # the region is whole 16-bit words
    ext_len = words - 1 if length is None else length
    nibble = check_nibble(ext_len) ^ (1 if bad_nibble else 0)
    head = ((nibble << 12) | (ext_len & 0xFFF)).to_bytes(2, "big")
    evo_word = ((reserved << 12) | len(evo)).to_bytes(2, "big")
    par = parity(reserved, len(evo), body) ^ (0x5A if bad_parity else 0)
    return head + evo_word + body + bytes([par])


def parse_extra_data(data, reading):
    if len(data) < 2:
        return None
    word = int.from_bytes(data[0:2], "big")
    nibble, length = word >> 12, word & 0xFFF
    padding_only = nibble == 0 and length == 0  # 16.2: the padding-only form is exempt
    out = {
        "padding_only": padding_only,
        "check_ok": padding_only or nibble == check_nibble(length),
        "framing_ok": False,
        "parity_ok": False,
        "reserved_zero": False,
        "length_fits": False,
        "length": length,
        "frame_bytes": 0,
    }
    if len(data) >= 5:
        evo_word = int.from_bytes(data[2:4], "big")
        reserved, frame_bytes = evo_word >> 12, evo_word & 0xFFF
        out["frame_bytes"] = frame_bytes
        if 4 + frame_bytes + 1 <= len(data):
            out["framing_ok"] = True
            out["reserved_zero"] = reserved == 0
            out["parity_ok"] = parity(reserved, frame_bytes, data[4:-1]) == data[-1]
            out["length_fits"] = (length + 1) * 16 >= frame_bytes * 8 + 40
            out["container"] = parse_evo_frame(data[4 : 4 + frame_bytes], reading)
    if "container" not in out:
        out["container"] = error("no_evolution_payload", 0)
    return out


# ---------------------------------------------------------------------------------------------
# the vectors


def vector(name, note, built_as, extra):
    expect = {reading: parse_extra_data(extra, reading) for reading in READINGS}
    return {
        "name": name,
        "note": note,
        "built_as": built_as,
        "extra_data": extra.hex(),
        "expect": expect,
    }


def make_vectors():
    out = []

    def add(name, note, reading, evo, **extra_args):
        out.append(vector(name, note, reading, build_extra_data(evo, **extra_args)))

    oamd = bytes((i * 37 + 5) & 0xFF for i in range(23))
    t56 = table56_config

    # --- the container's header: versions and key ids -------------------------------------------
    for reading in READINGS:
        add(
            f"{reading}_no_payloads",
            "version 0, key 0, no payloads, protection 1/0",
            reading,
            build_evo_frame(reading),
        )
        for version in (1, 2, 3, 4, 20):
            add(
                f"{reading}_version_{version}",
                "the 2-bit version and its extension",
                reading,
                build_evo_frame(reading, version=version),
            )
        for key_id in (0, 1, 2, 3, 4, 5, 6, 7, 8, 70 if reading == "spec36" else 300):
            add(
                f"{reading}_key_id_{key_id}",
                "the 3-bit key id and its extension",
                reading,
                build_evo_frame(reading, key_id=key_id),
            )

    # --- payloads: none, one, several; sizes at the edges of the size coding ------------------
    for reading in READINGS:
        add(
            f"{reading}_one_oamd_payload",
            "payload id 11, Table 56 configuration",
            reading,
            build_evo_frame(reading, payloads=[(11, t56(), oamd)]),
        )
        add(
            f"{reading}_three_payloads",
            "ids 11, 14 and 7 in one frame",
            reading,
            build_evo_frame(
                reading,
                payloads=[
                    (11, t56(), oamd),
                    (14, t56(group_id=2), b"\x01\x02\x03"),
                    (7, t56(), b""),
                ],
            ),
        )
        add(
            f"{reading}_empty_payload",
            "a payload of size 0",
            reading,
            build_evo_frame(reading, payloads=[(11, t56(), b"")]),
        )
        for size in (254, 255, 256, 300):
            add(
                f"{reading}_payload_size_{size}",
                "the edges of variable_bits(8): one group, an escape, two groups",
                reading,
                build_evo_frame(
                    reading, payloads=[(11, t56(), bytes((i * 7) & 0xFF for i in range(size)))]
                ),
            )
    for pid in (31, 32, 40, 60):
        add(
            f"emdf_payload_id_{pid}",
            "payload ids at and above the escape at 31",
            "emdf",
            build_evo_frame("emdf", payloads=[(pid, t56(), b"\xaa\xbb")]),
        )
    add(
        "spec36_payload_id_31",
        "spec36 reads id 31 as an ordinary id",
        "spec36",
        build_evo_frame("spec36", payloads=[(31, t56(), b"\xaa\xbb")]),
    )

    # --- every flag of evo_payload_config() ---------------------------------------------------
    configs = {
        "smploffst": default_config() | {"sample_offset": 1234, "priority": 9, "proc_allowed": 1},
        "smploffst_small": default_config()
        | {"sample_offset": 5, "priority": 0, "proc_allowed": 0},
        "smploffst_edge": default_config()
        | {"sample_offset": 2047, "priority": 31, "proc_allowed": 3},
        "duration": t56(duration=5000),
        "duration_small": t56(duration=7),
        "groupid_multi": t56(group_id=20),
        "groupid_3": t56(group_id=3),
        "codecdata": t56(codec_data=0xA5),
        "discard_unknown": default_config() | {"discard_unknown": True},
        "aligned_dup": t56(
            create_duplicate=True, remove_duplicate=True, priority=31, proc_allowed=3
        ),
        "not_aligned": default_config(),
        "everything": default_config()
        | {
            "sample_offset": 100,
            "duration": 3000,
            "group_id": 9,
            "codec_data": 0x11,
            "priority": 17,
            "proc_allowed": 2,
        },
    }
    for reading in READINGS:
        for name, cfg in configs.items():
            add(
                f"{reading}_config_{name}",
                "one payload with this payload configuration",
                reading,
                build_evo_frame(reading, payloads=[(11, cfg, b"\x5a\xa5")]),
            )

    # --- protection: every combination of the two length codes ---------------------------------
    for reading in READINGS:
        for primary in range(4):
            for secondary in range(4):
                add(
                    f"{reading}_protection_{primary}_{secondary}",
                    "protection length codes (a primary 0 is reserved in emdf)",
                    reading,
                    build_evo_frame(
                        reading,
                        payloads=[(11, t56(), b"\x01")],
                        primary=primary,
                        secondary=secondary,
                    ),
                )

    # --- padding inside the frame and in the region --------------------------------------------
    base = build_evo_frame("emdf", payloads=[(11, t56(), oamd)])
    add(
        "padding_in_frame_zero",
        "zero bits after the protection, to the end of the payload",
        "emdf",
        build_evo_frame("emdf", payloads=[(11, t56(), oamd)], pad_bits=13),
    )
    nonzero = bytearray(build_evo_frame("emdf", payloads=[(11, t56(), oamd)]))
    nonzero += b"\x80"
    add("padding_in_frame_nonzero", "a set bit after the protection", "emdf", bytes(nonzero))
    add(
        "padding_in_region",
        "extra data padding between the payload and the parity byte",
        "emdf",
        base,
        padding=b"\x00\x00\x00",
    )
    add(
        "padding_in_region_nonzero",
        "padding bytes that are not zero are in the parity",
        "emdf",
        base,
        padding=b"\x12\x34",
    )

    # --- the region's checks ------------------------------------------------------------------
    add(
        "bad_check_nibble",
        "header_check_nibble does not match extra_data_length",
        "emdf",
        base,
        bad_nibble=True,
    )
    add("bad_parity", "the parity byte is wrong", "emdf", base, bad_parity=True)
    add("evo_reserved_set", "evo_reserved is not 0", "emdf", base, reserved=5)
    add("length_too_small", "extra_data_length under the capacity rule", "emdf", base, length=1)
    add("length_large", "extra_data_length over the capacity rule", "emdf", base, length=0x7FF)
    add(
        "nibble_and_parity_bad",
        "both checks fail and the frame still reads",
        "emdf",
        base,
        bad_nibble=True,
        bad_parity=True,
    )
    add("empty_evolution_payload", "evo_frame_byte_length 0", "emdf", b"")

    # --- truncations: each parse failure has a vector, and INTENDED says which ----------------
    good = build_evo_frame("emdf", payloads=[(11, t56(), oamd)])
    no_group = default_config() | {"frame_aligned": True}
    add(
        "truncated_in_version_extension",
        "the version's extension runs past the end",
        "emdf",
        bytes([0xFF]),
    )
    add(
        "truncated_in_key_extension",
        "the key id's extension runs past the end",
        "emdf",
        bytes([0x3F]),
    )
    add(
        "missing_terminator",
        "the frame ends right after a payload",
        "emdf",
        _no_terminator("emdf", [(11, t56(), b"\x01\x02")]),
    )
    add(
        "truncated_payload_id_escape",
        "ends inside an escaped payload id",
        "emdf",
        bytes([0x07, 0xFF]),
    )
    add("truncated_config", "ends inside the payload configuration", "emdf", bytes([0x02, 0xC9]))
    add(
        "truncated_size",
        "ends inside the payload size",
        "emdf",
        build_evo_frame("emdf", payloads=[(11, t56(), bytes(300))])[:5],
    )
    add(
        "truncated_payload",
        "the size says more bytes than the frame has",
        "emdf",
        build_evo_frame("emdf", payloads=[(11, t56(), b"\x01\x02\x03")], size_override=40),
    )
    add("truncated_payload_by_nine_bytes", "the payload is cut short", "emdf", good[:-9])
    add(
        "truncated_in_protection_codes",
        "ends inside the two length codes (one bit left of them)",
        "emdf",
        build_evo_frame("emdf", payloads=[(11, no_group, b"\x01")])[:-1],
    )
    add(
        "truncated_in_protection_bytes",
        "ends inside the protection bytes",
        "emdf",
        build_evo_frame("emdf", payloads=[(11, t56(), b"\x01")], primary=3, secondary=3)[:-5],
    )
    add(
        "spec36_truncated_in_size",
        "spec36: the size's escape runs past the end",
        "spec36",
        build_evo_frame("spec36", payloads=[(11, t56(), bytes(300))])[:5],
    )
    add(
        "spec36_cap_reached",
        "an escape value at its maximum number of extensions",
        "spec36",
        _spec36_cap(),
    )

    # --- the region's own framing --------------------------------------------------------------
    region = build_extra_data(good)
    exceeds = bytearray(region)
    exceeds[2:4] = (len(good) + 40).to_bytes(2, "big")
    out.append(
        vector(
            "framing_exceeds_region",
            "evo_frame_byte_length is larger than the region",
            "emdf",
            bytes(exceeds),
        )
    )
    out.append(
        vector(
            "region_of_three_bytes",
            "a header word and one byte: no evolution framing",
            "emdf",
            region[:3],
        )
    )
    out.append(vector("region_of_two_bytes", "only the header word", "emdf", region[:2]))

    # --- the header word (16.2): the padding-only form, and a nibble that is the old formula ----
    out.append(
        vector(
            "padding_only_region",
            "header word zero, then zero words: no nibble rule, no evolution frame",
            "emdf",
            bytes(8),
        )
    )
    old_nibble = bytearray(region)
    old_nibble[0] = (old_nibble[0] & 0x0F) | (((check_nibble(len(region) // 2 - 1) ^ 0xF)) << 4)
    out.append(
        vector(
            "nibble_without_the_xor_f",
            "the nibble that is the XOR of the length's three nibbles alone: wrong under 16.2",
            "emdf",
            bytes(old_nibble),
        )
    )

    intended = {
        "truncated_in_version_extension": ("emdf", "truncated_header"),
        "truncated_in_key_extension": ("emdf", "truncated_header"),
        "missing_terminator": ("emdf", "missing_terminator"),
        "truncated_payload_id_escape": ("emdf", "truncated_payload_id"),
        "truncated_config": ("emdf", "truncated_config"),
        "truncated_size": ("emdf", "truncated_size"),
        "truncated_payload": ("emdf", "truncated_payload"),
        "truncated_payload_by_nine_bytes": ("emdf", "truncated_payload"),
        "truncated_in_protection_codes": ("emdf", "truncated_protection"),
        "truncated_in_protection_bytes": ("emdf", "truncated_protection"),
        "spec36_truncated_in_size": ("spec36", "truncated_size"),
        "empty_evolution_payload": ("emdf", "empty"),
        "framing_exceeds_region": ("emdf", "no_evolution_payload"),
        "region_of_three_bytes": ("emdf", "no_evolution_payload"),
        **{f"emdf_protection_0_{s}": ("emdf", "reserved_protection_length") for s in range(4)},
    }
    by_name = {v["name"]: v for v in out}
    for name, (reading, kind) in intended.items():
        got = by_name[name]["expect"][reading]["container"]
        assert not got["ok"] and got["kind"] == kind, (name, got)
    seen = {kind for _, kind in intended.values()}
    assert seen == {
        "truncated_header",
        "missing_terminator",
        "truncated_payload_id",
        "truncated_config",
        "truncated_size",
        "truncated_payload",
        "truncated_protection",
        "empty",
        "no_evolution_payload",
        "reserved_protection_length",
    }, seen
    return out


def _no_terminator(reading, payloads):
    w = Writer()
    w.put(0, 2)
    w.put(0, 3)
    for pid, cfg, data in payloads:
        w.put(pid, 5)
        write_config(w, reading, cfg)
        vb_write(w, reading, 8, EXT["size"], len(data))
        for b in data:
            w.put(b, 8)
    return w.tobytes()


def _spec36_cap():
    # version 3 and an extension of 17 groups of "11": the cap ends the escape, no zero group
    w = Writer()
    w.put(3, 2)
    for _ in range(EXT["version"] + 1):
        w.put(3, 2)
    w.put(0, 3)  # key id 0
    w.put(0, 5)  # no payloads
    w.put(1, 2)
    w.put(0, 2)
    w.put(0xA0, 8)
    return w.tobytes()


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("write", "check"):
        p = sub.add_parser(name)
        p.add_argument("path", nargs="?", default=str(VECTORS))
    p = sub.add_parser("parse")
    p.add_argument("hex")
    p.add_argument("--reading", choices=READINGS, default="emdf")
    args = parser.parse_args(argv)
    if args.command == "parse":
        result = parse_extra_data(bytes.fromhex(args.hex), args.reading)
        print(json.dumps(result, indent=1))
        return 0
    lines = [json.dumps(v, sort_keys=True, separators=(",", ":")) for v in make_vectors()]
    text = '{"vectors":[\n' + ",\n".join(lines) + "\n]}\n"
    path = pathlib.Path(args.path)
    if args.command == "write":
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        print(f"wrote {path} ({len(make_vectors())} vectors)")
        return 0
    if not path.exists() or path.read_text() != text:
        print(f"{path} is out of date: run truehd_evolution.py write", file=sys.stderr)
        return 1
    print(f"{path} is current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
