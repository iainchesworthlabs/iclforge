"""Makes a protected (Common Encryption) fragmented MP4 from a clear AC-3, E-AC-3 or AC-4 MP4.

Why. FFmpeg 6.1 writes `enca` and `tenc` for AC-3 and E-AC-3 but leaves the samples clear and the
`senc` empty, and cannot put AC-4 in an MP4 at all, so it cannot make a protected Dolby fixture.
GPAC's `MP4Box -crypt` can, but not the layouts a packager uses for a clear lead-in or a key
rotation. This script writes those. It exists to make test input for the work planned in
planning/cenc-conformance.md; nothing in the library reads it.

What it is not. The scheme rules below are written from memory, not from ISO/IEC 23001-7, which this
project does not hold. They are checked only by agreement with other implementations: GPAC 2.2.1
and FFmpeg 6.1.1 decrypt the `core` layout of all four schemes to the clear samples, and GPAC
decrypts `leader_entry`, `leader_seig` and `rotate` too. A 1:9 pattern on audio is not agreed by
either of them. Do not use this file as a reference for the standard.

Input is an MP4 with one `ac-3`, `ec-3` or `ac-4` track, such as `forge mp4 in.ec3 out.mp4` or
`forge ac4-encode in.wav out.mp4` writes. Output is a fragmented MP4 (default-base-is-moof, one
`traf` per `moof`) whose `stsd` entry is `enca`, with the original configuration box kept and a
`sinf` after it. Audio is protected whole-sample, with no subsample map. The keys are fixed test
values; change them with --kid and --key.

    python3 tools/generators/cenc_make_protected.py clear.mp4 out.mp4 cenc
    python3 tools/generators/cenc_make_protected.py clear.mp4 out.mp4 cbcs --variant leader_entry

Schemes: cenc (AES-CTR, 8 or 16 byte IV per sample), cbc1 (AES-CBC, 16 byte IV), cens (AES-CTR with
a crypt:skip pattern), cbcs (AES-CBC with a pattern and a constant IV, so no `senc`).

Variants (--variant):
  core              every fragment encrypted, one sample description
  leader_entry      stsd holds the clear entry, then `enca`; fragment 1 is clear and uses entry 1
  leader_entry_rev  stsd holds `enca`, then the clear entry (the order Shaka Packager writes);
                    fragment 1 is clear and uses entry 2
  leader_seig       one `enca` entry; fragment 1 is clear, marked by a 'seig' group with
                    isProtected 0
  rotate            two keys; the 'seig' group names the second KID in the even fragments

Needs the `cryptography` package. The `core` output is checked by decrypting it again here
(--no-verify turns that off); the real checks are `MP4Box -decrypt` and
`ffprobe -decryption_key KEY -show_data_hash sha256 -show_entries packet=data_hash`.
"""

import argparse
import struct
import sys
from dataclasses import dataclass

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:  # pragma: no cover - environment dependent
    sys.exit("cenc_make_protected.py needs the 'cryptography' package")

KID1 = bytes.fromhex("a1b2c3d4e5f60718293a4b5c6d7e8f90")
KEY1 = bytes.fromhex("00112233445566778899aabbccddeeff")
KID2 = bytes.fromhex("0f1e2d3c4b5a69788796a5b4c3d2e1f0")
KEY2 = bytes.fromhex("ffeeddccbbaa99887766554433221100")
CONSTANT_IV = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
# The W3C common SystemID and Widevine's, both as published (W3C "cenc" init data format 4.1;
# DASH-IF IOP-6 7.3). The Widevine box carries a made-up payload: CENC does not define it.
COMMON_SYSTEM_ID = bytes.fromhex("1077efecc0b24d02ace33c1e52e2fb4b")
WIDEVINE_SYSTEM_ID = bytes.fromhex("edef8ba979d64acea3c827dcd51d21ed")

SCHEMES = ("cenc", "cbc1", "cens", "cbcs")
VARIANTS = ("core", "leader_entry", "leader_entry_rev", "leader_seig", "rotate")
AUDIO_ENTRY_BYTES = 28  # SampleEntry (8) and AudioSampleEntry (20) fields before the children
SYNC_SAMPLE_FLAGS = 0x02000000
CLEAR_LEAD_VARIANTS = ("leader_entry", "leader_entry_rev", "leader_seig")


# --- boxes ------------------------------------------------------------------------------------


def box(kind: bytes, payload: bytes = b"") -> bytes:
    return struct.pack(">I4s", 8 + len(payload), kind) + payload


def fullbox(kind: bytes, version: int, flags: int, payload: bytes = b"") -> bytes:
    return box(kind, struct.pack(">I", (version << 24) | flags) + payload)


def boxes(data: bytes, start: int = 0, end: int | None = None):
    """Yields (type, offset, header_size, size) for the boxes in data[start:end]."""
    end = len(data) if end is None else end
    pos = start
    while pos + 8 <= end:
        size, kind = struct.unpack(">I4s", data[pos : pos + 8])
        header = 8
        if size == 1:
            size = struct.unpack(">Q", data[pos + 8 : pos + 16])[0]
            header = 16
        elif size == 0:
            size = end - pos
        if size < header or pos + size > end:
            return
        yield kind, pos, header, size
        pos += size


def find(data: bytes, path: str):
    """The first box on a 'moov/trak/...' path as (offset, header_size, size), or None."""
    start, end = 0, len(data)
    found = None
    for name in path.split("/"):
        found = next(
            ((p, h, s) for k, p, h, s in boxes(data, start, end) if k.decode("latin1") == name),
            None,
        )
        if found is None:
            return None
        start, end = found[0] + found[1], found[0] + found[2]
        if name == "stsd":
            start += 8
    return found


def u32(data: bytes, at: int) -> int:
    return struct.unpack(">I", data[at : at + 4])[0]


def u64(data: bytes, at: int) -> int:
    return struct.unpack(">Q", data[at : at + 8])[0]


# --- reading the clear input and the protected output ------------------------------------------


def _progressive_samples(data: bytes) -> list[bytes]:
    stbl = find(data, "moov/trak/mdia/minf/stbl")
    if not stbl:
        return []
    sizes: list[int] = []
    offsets: list[int] = []
    runs: list[tuple[int, int]] = []
    for kind, pos, header, _size in boxes(data, stbl[0] + stbl[1], stbl[0] + stbl[2]):
        body = pos + header
        if kind == b"stsz":
            uniform, count = u32(data, body + 4), u32(data, body + 8)
            sizes = (
                [uniform] * count
                if uniform
                else [u32(data, body + 12 + 4 * i) for i in range(count)]
            )
        elif kind == b"stco":
            offsets = [u32(data, body + 8 + 4 * i) for i in range(u32(data, body + 4))]
        elif kind == b"co64":
            offsets = [u64(data, body + 8 + 8 * i) for i in range(u32(data, body + 4))]
        elif kind == b"stsc":
            runs = [
                (u32(data, body + 8 + 12 * i), u32(data, body + 12 + 12 * i))
                for i in range(u32(data, body + 4))
            ]
    samples: list[bytes] = []
    index = 0
    for chunk, first_offset in enumerate(offsets):
        offset = first_offset
        for _ in range(max((n for first, n in runs if first <= chunk + 1), default=0)):
            if index >= len(sizes):
                break
            samples.append(data[offset : offset + sizes[index]])
            offset += sizes[index]
            index += 1
    return samples


def _fragment_samples(data: bytes, moof_start: int, start: int, end: int, size_default: int):
    base, runs = moof_start, []
    for kind, pos, header, _size in boxes(data, start, end):
        body = pos + header
        if kind == b"tfhd":
            flags = u32(data, body) & 0xFFFFFF
            at = body + 8
            if flags & 0x1:
                base = u64(data, at)
                at += 8
            at += 4 * bool(flags & 0x2) + 4 * bool(flags & 0x8)
            if flags & 0x10:
                size_default = u32(data, at)
        elif kind == b"trun":
            runs.append(body)
    out = []
    cursor = None
    for body in runs:
        flags, count, at = u32(data, body) & 0xFFFFFF, u32(data, body + 4), body + 8
        if flags & 0x1:
            cursor = base + struct.unpack(">i", data[at : at + 4])[0]
            at += 4
        elif cursor is None:
            cursor = base
        at += 4 * bool(flags & 0x4)
        for _ in range(count):
            at += 4 * bool(flags & 0x100)
            size = size_default
            if flags & 0x200:
                size = u32(data, at)
                at += 4
            at += 4 * bool(flags & 0x400) + 4 * bool(flags & 0x800)
            out.append(data[cursor : cursor + size])
            cursor += size
    return out


def read_samples(data: bytes) -> list[bytes]:
    """Samples of the first track, from a progressive file or from moof/trun fragments."""
    samples = _progressive_samples(data)
    trex = find(data, "moov/mvex/trex")
    default_size = u32(data, trex[0] + trex[1] + 16) if trex else 0
    for kind, mpos, mheader, msize in boxes(data):
        if kind != b"moof":
            continue
        for tkind, tpos, theader, tsize in boxes(data, mpos + mheader, mpos + msize):
            if tkind == b"traf":
                samples += _fragment_samples(data, mpos, tpos + theader, tpos + tsize, default_size)
    return samples


# --- the ciphers -------------------------------------------------------------------------------


def _xor(a: bytes, b: bytes) -> bytes:
    return bytes(x ^ y for x, y in zip(a, b, strict=False))


def _ctr_keystream(key: bytes, iv: bytes, blocks: int) -> bytes:
    # An 8-byte IV is the high half of the counter block; the low half counts from 0.
    counter = int.from_bytes(iv + b"\x00" * (16 - len(iv)), "big")
    ecb = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return b"".join(
        ecb.update(((counter + i) % (1 << 128)).to_bytes(16, "big")) for i in range(blocks)
    )


def _pattern_blocks(length: int, crypt: int, skip: int) -> list[int]:
    """Indexes of the whole 16-byte blocks a pattern encrypts; n:0 encrypts every block."""
    whole = length // 16
    if skip == 0:
        return list(range(whole))
    chosen, at = [], 0
    while at < whole:
        chosen += [b for b in range(at, at + crypt) if b < whole]
        at += crypt + skip
    return chosen


def crypt_sample(
    scheme: str,
    key: bytes,
    sample: bytes,
    iv: bytes,
    crypt: int = 0,
    skip: int = 0,
    *,
    decrypt: bool = False,
) -> bytes:
    """Encrypts (or decrypts) one whole sample as the protected range, with no subsample map."""
    if scheme == "cenc":
        return _xor(sample, _ctr_keystream(key, iv, (len(sample) + 15) // 16))
    chosen = _pattern_blocks(len(sample), crypt, skip)
    out = bytearray(sample)
    if scheme == "cens":
        stream = _ctr_keystream(key, iv, len(chosen))
        for n, block in enumerate(chosen):
            out[block * 16 : block * 16 + 16] = _xor(
                sample[block * 16 : block * 16 + 16], stream[n * 16 : n * 16 + 16]
            )
        return bytes(out)
    cipher = Cipher(algorithms.AES(key), modes.CBC(iv))
    chain = cipher.decryptor() if decrypt else cipher.encryptor()
    if scheme == "cbc1":
        whole = len(sample) // 16 * 16
        return chain.update(sample[:whole]) + chain.finalize() + sample[whole:]
    for block in chosen:  # cbcs: the chain runs over the encrypted blocks only
        out[block * 16 : block * 16 + 16] = chain.update(sample[block * 16 : block * 16 + 16])
    return bytes(out)


def sample_iv(number: int, iv_size: int) -> bytes:
    """The IV of the number-th protected sample (1-based); the constant IV when iv_size is 0."""
    if iv_size == 0:
        return CONSTANT_IV
    if iv_size == 8:
        return number.to_bytes(8, "big")
    return (0x1000 + number).to_bytes(16, "big")


# --- protection boxes --------------------------------------------------------------------------


@dataclass(frozen=True)
class Plan:
    scheme: str
    variant: str
    iv_size: int  # 0 means a constant IV
    crypt: int
    skip: int
    kid1: bytes
    key1: bytes
    kid2: bytes
    key2: bytes

    def key_of(self, number: int) -> bytes:
        """The key of fragment number (1-based)."""
        return self.key2 if self.variant == "rotate" and number % 2 == 0 else self.key1


def tenc(plan: Plan, kid: bytes) -> bytes:
    version = 1 if plan.scheme in ("cens", "cbcs") else 0
    body = b"\x00" + (bytes([(plan.crypt << 4) | plan.skip]) if version else b"\x00")
    body += bytes([1, plan.iv_size]) + kid
    if plan.iv_size == 0:
        body += bytes([len(CONSTANT_IV)]) + CONSTANT_IV
    return fullbox(b"tenc", version, 0, body)


def sinf(plan: Plan, original: bytes, kid: bytes) -> bytes:
    return box(
        b"sinf",
        box(b"frma", original)
        + fullbox(b"schm", 0, 0, plan.scheme.encode() + struct.pack(">I", 0x00010000))
        + box(b"schi", tenc(plan, kid)),
    )


def seig_entry(plan: Plan, protected: int, kid: bytes) -> bytes:
    """A CencSampleEncryptionGroupEntry; an unprotected entry has no IV and an all-zero KID."""
    if not protected:
        return b"\x00\x00\x00\x00" + bytes(16)
    entry = b"\x00" + bytes([(plan.crypt << 4) | plan.skip]) + bytes([1, plan.iv_size]) + kid
    if plan.iv_size == 0:
        entry += bytes([len(CONSTANT_IV)]) + CONSTANT_IV
    return entry


def sample_group_boxes(entries: list[bytes], count: int, index: int) -> bytes:
    described = b"".join(struct.pack(">I", len(e)) + e for e in entries)
    sgpd = fullbox(b"sgpd", 1, 0, b"seig" + struct.pack(">II", 0, len(entries)) + described)
    sbgp = fullbox(b"sbgp", 0, 0, b"seig" + struct.pack(">III", 1, count, index))
    return sgpd + sbgp


def pssh_v1(system_id: bytes, kids: list[bytes]) -> bytes:
    body = system_id + struct.pack(">I", len(kids)) + b"".join(kids) + struct.pack(">I", 0)
    return fullbox(b"pssh", 1, 0, body)


def pssh_v0(system_id: bytes, data: bytes) -> bytes:
    return fullbox(b"pssh", 0, 0, system_id + struct.pack(">I", len(data)) + data)


# --- the file ----------------------------------------------------------------------------------


def init_segment(clear: bytes, plan: Plan) -> tuple[bytes, int]:
    """ftyp and moov of the output, and the sample duration of the input."""
    pos, header, size = find(clear, "moov/trak/mdia/minf/stbl/stsd")
    kind, first, _h, entry_size = next(boxes(clear, pos + header + 8, pos + size))
    entry = clear[first : first + entry_size]
    if kind not in (b"ec-3", b"ac-3", b"ac-4"):
        sys.exit("the input needs one ac-3, ec-3 or ac-4 track")
    prefix = entry[8 : 8 + AUDIO_ENTRY_BYTES]
    children = entry[8 + AUDIO_ENTRY_BYTES :]
    protected = box(b"enca", prefix + children + sinf(plan, kind, plan.kid1))
    if plan.variant == "leader_entry":
        entries = [entry, protected]
    elif plan.variant == "leader_entry_rev":
        entries = [protected, entry]
    else:
        entries = [protected]
    stsd = fullbox(b"stsd", 0, 0, struct.pack(">I", len(entries)) + b"".join(entries))

    def part(path: str) -> bytes:
        at, _h, length = find(clear, path)
        return clear[at : at + length]

    empty = (
        fullbox(b"stts", 0, 0, struct.pack(">I", 0))
        + fullbox(b"stsc", 0, 0, struct.pack(">I", 0))
        + fullbox(b"stsz", 0, 0, struct.pack(">II", 0, 0))
        + fullbox(b"stco", 0, 0, struct.pack(">I", 0))
    )
    stbl = box(b"stbl", stsd + empty)
    minf = box(b"minf", part("moov/trak/mdia/minf/smhd") + part("moov/trak/mdia/minf/dinf") + stbl)
    mdia = box(b"mdia", part("moov/trak/mdia/mdhd") + part("moov/trak/mdia/hdlr") + minf)
    trak = box(b"trak", part("moov/trak/tkhd") + mdia)
    stts_at, stts_header, _ = find(clear, "moov/trak/mdia/minf/stbl/stts")
    delta = u32(clear, stts_at + stts_header + 12)
    default_index = 2 if plan.variant == "leader_entry" else 1
    trex = fullbox(
        b"trex", 0, 0, struct.pack(">IIIII", 1, default_index, delta, 0, SYNC_SAMPLE_FLAGS)
    )
    kids = [plan.kid1] + ([plan.kid2] if plan.variant == "rotate" else [])
    pssh = pssh_v1(COMMON_SYSTEM_ID, kids) + pssh_v0(WIDEVINE_SYSTEM_ID, b"\x12\x10" + plan.kid1)
    moov = box(b"moov", part("moov/mvhd") + trak + box(b"mvex", trex) + pssh)
    brands = [b"iso6", b"cmfc"] + (
        [plan.scheme.encode()] if plan.scheme in ("cenc", "cbcs") else []
    )
    ftyp = box(b"ftyp", b"iso6" + struct.pack(">I", 0) + b"".join(brands))
    return ftyp + moov, delta


def fragment_groups(plan: Plan, number: int, count: int, clear: bool) -> bytes:
    """The 'seig' sample group boxes of a fragment, for the variants that use them."""
    if plan.variant == "leader_seig":
        entries = [seig_entry(plan, 0, bytes(16)), seig_entry(plan, 1, plan.kid1)]
        return sample_group_boxes(entries, count, 1 if clear else 2)
    if plan.variant == "rotate":
        entries = [seig_entry(plan, 1, plan.kid1), seig_entry(plan, 1, plan.kid2)]
        return sample_group_boxes(entries, count, 2 if number % 2 == 0 else 1)
    return b""


def movie_fragment(
    plan: Plan, number: int, decode_time: int, coded: list[bytes], ivs: list[bytes], clear: bool
) -> bytes:
    """One moof; its trun data offset and saio offset are fixed up from a trial build."""
    two_entries = plan.variant in ("leader_entry", "leader_entry_rev")
    clear_first = plan.variant == "leader_entry"  # the clear entry is entry 1 in this layout
    description = (1 if clear else 2) if clear_first else (2 if clear else 1)
    tfhd = fullbox(
        b"tfhd",
        0,
        0x020000 | (0x2 if two_entries else 0),
        struct.pack(">I", 1) + (struct.pack(">I", description) if two_entries else b""),
    )
    tfdt = fullbox(b"tfdt", 1, 0, struct.pack(">Q", decode_time))
    groups = fragment_groups(plan, number, len(coded), clear)
    with_aux = plan.iv_size != 0 and not clear

    def build(data_offset: int, saio_offset: int) -> bytes:
        sizes = b"".join(struct.pack(">I", len(s)) for s in coded)
        trun = fullbox(b"trun", 0, 0x000201, struct.pack(">Ii", len(coded), data_offset) + sizes)
        aux = b""
        if with_aux:
            aux = (
                fullbox(b"saiz", 0, 0, bytes([plan.iv_size]) + struct.pack(">I", len(coded)))
                + fullbox(b"saio", 0, 0, struct.pack(">II", 1, saio_offset))
                + fullbox(b"senc", 0, 0, struct.pack(">I", len(coded)) + b"".join(ivs))
            )
        traf = box(b"traf", tfhd + tfdt + groups + trun + aux)
        return box(b"moof", fullbox(b"mfhd", 0, 0, struct.pack(">I", number)) + traf)

    data_offset = len(build(0, 0)) + 8  # the moof, then the 8-byte mdat header
    # senc is the last box of the moof, so its IVs end it
    saio_offset = len(build(data_offset, 0)) - len(ivs) * plan.iv_size if with_aux else 0
    return build(data_offset, saio_offset)


def build(clear: bytes, plan: Plan, per_fragment: int) -> bytes:
    samples = read_samples(clear)
    if not samples:
        sys.exit("the input has no samples")
    out, delta = init_segment(clear, plan)
    iv_counter = 0
    decode_time = 0
    for number, start in enumerate(range(0, len(samples), per_fragment), 1):
        chunk = samples[start : start + per_fragment]
        clear_fragment = plan.variant in CLEAR_LEAD_VARIANTS and number == 1
        coded, ivs = [], []
        for sample in chunk:
            if clear_fragment:
                coded.append(sample)
                continue
            iv_counter += 1
            ivs.append(sample_iv(iv_counter, plan.iv_size))
            key = plan.key_of(number)
            coded.append(crypt_sample(plan.scheme, key, sample, ivs[-1], plan.crypt, plan.skip))
        out += movie_fragment(plan, number, decode_time, coded, ivs, clear_fragment)
        out += box(b"mdat", b"".join(coded))
        decode_time += delta * len(chunk)
    return out


def verify(clear: bytes, protected: bytes, plan: Plan) -> None:
    """Decrypts the `core` output with its key and compares with the clear samples."""
    if plan.variant != "core":
        return  # the other layouts are for the oracles to judge
    got, want = read_samples(protected), read_samples(clear)
    if len(got) != len(want):
        sys.exit("verify: the sample count differs")
    for n, (sample, original) in enumerate(zip(got, want, strict=True), 1):
        iv = sample_iv(n, plan.iv_size)
        plain = crypt_sample(
            plan.scheme, plan.key1, sample, iv, plan.crypt, plan.skip, decrypt=True
        )
        if plain != original:
            sys.exit(f"verify: sample {n} does not decrypt to the clear sample")


def parse_pattern(text: str) -> tuple[int, int]:
    crypt, skip = (int(x) for x in text.split(":"))
    return crypt, skip


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", maxsplit=1)[0])
    parser.add_argument("input", help="clear MP4 with one ac-3, ec-3 or ac-4 track")
    parser.add_argument("output", help="protected fragmented MP4 to write")
    parser.add_argument("scheme", choices=SCHEMES)
    parser.add_argument("--variant", choices=VARIANTS, default="core")
    parser.add_argument("--iv-size", type=int, choices=(8, 16), default=0, help="cenc and cens IV")
    parser.add_argument("--pattern", default="1:0", help="crypt:skip blocks for cens and cbcs")
    parser.add_argument("--fragment", type=int, default=16, help="samples per fragment")
    parser.add_argument("--kid", default=KID1.hex())
    parser.add_argument("--key", default=KEY1.hex())
    parser.add_argument("--kid2", default=KID2.hex(), help="second KID, for --variant rotate")
    parser.add_argument("--key2", default=KEY2.hex(), help="second key, for --variant rotate")
    parser.add_argument("--no-verify", action="store_true")
    args = parser.parse_args()

    patterned = args.scheme in ("cens", "cbcs")
    crypt, skip = parse_pattern(args.pattern) if patterned else (0, 0)
    if args.scheme == "cbcs":
        iv_size = 0
    else:
        iv_size = args.iv_size or (8 if args.scheme in ("cenc", "cens") else 16)
    plan = Plan(
        args.scheme,
        args.variant,
        iv_size,
        crypt,
        skip,
        bytes.fromhex(args.kid),
        bytes.fromhex(args.key),
        bytes.fromhex(args.kid2),
        bytes.fromhex(args.key2),
    )
    with open(args.input, "rb") as handle:
        clear = handle.read()
    protected = build(clear, plan, args.fragment)
    if not args.no_verify:
        verify(clear, protected, plan)
    with open(args.output, "wb") as handle:
        handle.write(protected)
    print(f"wrote {args.output}: {args.scheme}, {args.variant}, {len(protected)} bytes")


if __name__ == "__main__":
    main()
