#!/usr/bin/env python3
"""Check a signed firmware update index the way a board does (planning/esp32-ota.md, O10).

    python tools/hearth/index_verify.py --index hearth-update-beta.json \
        --sig hearth-update-beta.sig.json --anchors firmware/keys/trust.pem

A board that pulls reads two small files for its channel: the update index
(`hearth-update-<channel>.json`: which release is newest and, for each image,
its size and SHA-256) and the signature over it (`hearth-update-<channel>.sig.json`).
It installs an image only if the pair verifies against the trust anchors in its
build. This module is that check in Python with nothing but the standard library,
so that CI, a person looking at a channel before trusting it, and the tests need no
package. It is deliberately a separate implementation from the board's (mbedTLS)
and from the signing tool's (`cryptography`): three implementations that must
agree is the point.

What is checked, in this order (any failure is a VerifyError with a code the board
reports too):

 1. the signature file is well formed and its `sha256` field is the index's SHA-256
    (bad_sig_file, hash_mismatch);
 2. the chain (the leaf, then at most one intermediate; the root is never sent) ends
    at a trust anchor: each certificate is signed by the next, the last by an anchor,
    or is an anchor itself (untrusted). An anchor may itself be an intermediate or a
    self-signed leaf. The anchor's own dates are not looked at; every other
    certificate's are, against `now` when it is given (cert_expired,
    cert_not_yet_valid);
 3. the leaf is for code signing: not a CA, key usage digitalSignature, extended key
    usage codeSigning, a key of at least RSA 2048 or P-256 (leaf_usage, weak_key),
    and its subject common name is the pinned one, when a name is pinned (leaf_name);
 4. the signature is a valid signature by the leaf's key over the index file's exact
    bytes (bad_signature).

Supported: RSA PKCS#1 v1.5 and PSS, and ECDSA over P-256 and P-384, with SHA-256,
SHA-384 or SHA-512 in certificates and SHA-256 in the index signature. A certificate
signed with RSASSA-PSS, or with SHA-1 or MD5, is refused.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import datetime as dt
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

SIG_FORMAT = 1
MAX_INDEX_BYTES = 4096
MAX_SIG_BYTES = 8192
MAX_CHAIN = 3
CHANNELS = ("stable", "beta")
SIGNATURE_ALGS = ("rsa-pkcs1-sha256", "rsa-pss-sha256", "ecdsa-p256-sha256")

# The shape of a file name or a version the board will build a URL from.
FILE_NAME = re.compile(r"^[A-Za-z0-9_+-][A-Za-z0-9._+-]{0,95}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")


class VerifyError(Exception):
    """A pair that does not verify. `code` is the word the board reports."""

    def __init__(self, code: str, text: str) -> None:
        super().__init__(f"{code}: {text}")
        self.code = code
        self.text = text


# --- ASN.1 DER, only as much as a certificate needs ---------------------------------------------


@dataclass(frozen=True)
class Tlv:
    tag: int
    start: int  # of the header
    body: int  # of the content
    end: int  # one past the content

    def raw(self, data: bytes) -> bytes:
        return data[self.start : self.end]

    def content(self, data: bytes) -> bytes:
        return data[self.body : self.end]


def read_tlv(data: bytes, pos: int, limit: int | None = None) -> Tlv:
    limit = len(data) if limit is None else limit
    if pos + 2 > limit:
        raise VerifyError("untrusted", "a certificate is cut short")
    tag = data[pos]
    if tag & 0x1F == 0x1F:
        raise VerifyError("untrusted", "a certificate uses a high tag number")
    length = data[pos + 1]
    body = pos + 2
    if length & 0x80:
        count = length & 0x7F
        if count == 0 or count > 4 or body + count > limit:
            raise VerifyError("untrusted", "a certificate has a bad length")
        length = int.from_bytes(data[body : body + count], "big")
        body += count
    end = body + length
    if end > limit:
        raise VerifyError("untrusted", "a certificate is cut short")
    return Tlv(tag, pos, body, end)


def children(data: bytes, node: Tlv) -> list[Tlv]:
    out = []
    pos = node.body
    while pos < node.end:
        child = read_tlv(data, pos, node.end)
        out.append(child)
        pos = child.end
    return out


def oid_text(content: bytes) -> str:
    if not content:
        return ""
    first = content[0]
    parts = [first // 40, first % 40]
    value = 0
    for byte in content[1:]:
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            parts.append(value)
            value = 0
    return ".".join(str(part) for part in parts)


OID_RSA = "1.2.840.113549.1.1.1"
OID_EC = "1.2.840.10045.2.1"
OID_P256 = "1.2.840.10045.3.1.7"
OID_P384 = "1.3.132.0.34"
OID_CN = "2.5.4.3"
OID_BASIC = "2.5.29.19"
OID_KEY_USAGE = "2.5.29.15"
OID_EKU = "2.5.29.37"
OID_CODE_SIGNING = "1.3.6.1.5.5.7.3.3"
SIG_HASHES = {
    "1.2.840.113549.1.1.11": ("rsa", "sha256"),
    "1.2.840.113549.1.1.12": ("rsa", "sha384"),
    "1.2.840.113549.1.1.13": ("rsa", "sha512"),
    "1.2.840.10045.4.3.2": ("ecdsa", "sha256"),
    "1.2.840.10045.4.3.3": ("ecdsa", "sha384"),
}
OID_RSA_PSS = "1.2.840.113549.1.1.10"
DIGEST_INFO = {
    "sha256": bytes.fromhex("3031300d060960864801650304020105000420"),
    "sha384": bytes.fromhex("3041300d060960864801650304020205000430"),
    "sha512": bytes.fromhex("3051300d060960864801650304020305000440"),
}


# --- elliptic curves, for ECDSA verification only -----------------------------------------------


@dataclass(frozen=True)
class Curve:
    name: str
    p: int
    a: int
    b: int
    gx: int
    gy: int
    n: int
    size: int  # bytes of a coordinate


P256 = Curve(
    "P-256",
    0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF,
    0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFC,
    0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B,
    0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
    0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5,
    0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551,
    32,
)
P384 = Curve(
    "P-384",
    0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFFFF0000000000000000FFFFFFFF,
    0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFFFF0000000000000000FFFFFFFC,
    0xB3312FA7E23EE7E4988E056BE3F82D19181D9C6EFE8141120314088F5013875AC656398D8A2ED19D2A85C8EDD3EC2AEF,
    0xAA87CA22BE8B05378EB1C71EF320AD746E1D3B628BA79B9859F741E082542A385502F25DBF55296C3A545E3872760AB7,
    0x3617DE4A96262C6F5D9E98BF9292DC29F8F41DBD289A147CE9DA3113B5F0B8C00A60B1CE1D7E819D7A431D7C90EA0E5F,
    0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFC7634D81F4372DDF581A0DB248B0A77AECEC196ACCC52973,
    48,
)
CURVES = {OID_P256: P256, OID_P384: P384}

Point = tuple[int, int] | None


def _add(curve: Curve, one: Point, two: Point) -> Point:
    if one is None:
        return two
    if two is None:
        return one
    (x1, y1), (x2, y2) = one, two
    p = curve.p
    if x1 == x2:
        if (y1 + y2) % p == 0:
            return None
        slope = (3 * x1 * x1 + curve.a) * pow(2 * y1, -1, p) % p
    else:
        slope = (y2 - y1) * pow(x2 - x1, -1, p) % p
    x3 = (slope * slope - x1 - x2) % p
    return x3, (slope * (x1 - x3) - y1) % p


def _multiply(curve: Curve, k: int, point: Point) -> Point:
    result: Point = None
    addend = point
    while k:
        if k & 1:
            result = _add(curve, result, addend)
        addend = _add(curve, addend, addend)
        k >>= 1
    return result


def ecdsa_verify(curve: Curve, public: tuple[int, int], digest: bytes, r: int, s: int) -> bool:
    n = curve.n
    if not (0 < r < n and 0 < s < n):
        return False
    x, y = public
    if (y * y - (x * x * x + curve.a * x + curve.b)) % curve.p != 0:
        return False
    e = int.from_bytes(digest, "big")
    if len(digest) * 8 > n.bit_length():
        e >>= len(digest) * 8 - n.bit_length()
    w = pow(s, -1, n)
    point = _add(
        curve,
        _multiply(curve, e * w % n, (curve.gx, curve.gy)),
        _multiply(curve, r * w % n, public),
    )
    return point is not None and point[0] % n == r


def der_signature(sig: bytes) -> tuple[int, int]:
    """An ECDSA-Sig-Value: SEQUENCE { INTEGER r, INTEGER s }."""
    seq = read_tlv(sig, 0)
    if seq.tag != 0x30 or seq.end != len(sig):
        raise VerifyError("bad_signature", "an ECDSA signature is not DER")
    first, second = children(sig, seq)
    return (
        int.from_bytes(first.content(sig), "big"),
        int.from_bytes(second.content(sig), "big"),
    )


# --- RSA, for verification only -----------------------------------------------------------------


def rsa_raw(public: tuple[int, int], signature: bytes) -> bytes:
    n, e = public
    size = (n.bit_length() + 7) // 8
    if len(signature) != size:
        raise VerifyError("bad_signature", "an RSA signature is the wrong length")
    value = int.from_bytes(signature, "big")
    if value >= n:
        raise VerifyError("bad_signature", "an RSA signature is out of range")
    return pow(value, e, n).to_bytes(size, "big")


def rsa_pkcs1_verify(
    public: tuple[int, int], hash_name: str, digest: bytes, signature: bytes
) -> bool:
    em = rsa_raw(public, signature)
    tail = DIGEST_INFO[hash_name] + digest
    expected = b"\x00\x01" + b"\xff" * (len(em) - len(tail) - 3) + b"\x00" + tail
    return len(em) >= len(tail) + 11 and em == expected


def _mgf1(seed: bytes, length: int, hash_name: str) -> bytes:
    out = b""
    counter = 0
    while len(out) < length:
        out += hashlib.new(hash_name, seed + counter.to_bytes(4, "big")).digest()
        counter += 1
    return out[:length]


def rsa_pss_verify(
    public: tuple[int, int], hash_name: str, message_hash: bytes, signature: bytes
) -> bool:
    n, _ = public
    bits = n.bit_length() - 1
    em_len = (bits + 7) // 8
    em = rsa_raw(public, signature)[-em_len:]
    h_len = len(message_hash)
    if em_len < h_len + 2 or em[-1] != 0xBC:
        return False
    masked, h = em[: em_len - h_len - 1], em[em_len - h_len - 1 : -1]
    top = 0xFF >> (8 * em_len - bits)
    if masked[0] & ~top & 0xFF:
        return False
    block = bytes(a ^ b for a, b in zip(masked, _mgf1(h, len(masked), hash_name), strict=True))
    block = bytes([block[0] & top]) + block[1:]
    zeros = len(block) - len(block.lstrip(b"\x00"))
    if zeros >= len(block) or block[zeros] != 0x01:
        return False
    salt = block[zeros + 1 :]
    check = hashlib.new(hash_name, b"\x00" * 8 + message_hash + salt).digest()
    return check == h


# --- certificates -------------------------------------------------------------------------------


@dataclass
class Certificate:
    der: bytes
    tbs: bytes
    sig_kind: str  # "rsa" or "ecdsa"
    sig_hash: str
    signature: bytes
    issuer: bytes
    subject: bytes
    not_before: dt.datetime
    not_after: dt.datetime
    key_kind: str  # "rsa" or "ec"
    rsa_public: tuple[int, int] | None
    ec_curve: Curve | None
    ec_public: tuple[int, int] | None
    is_ca: bool
    path_len: int | None
    key_usage: int | None  # bit i set when usage i (digitalSignature = 0, keyCertSign = 5) is
    ext_key_usage: list[str] | None
    common_name: str | None


def _time(data: bytes, node: Tlv) -> dt.datetime:
    text = node.content(data).decode("ascii")
    if node.tag == 0x17:  # UTCTime YYMMDDHHMMSSZ
        year = int(text[0:2])
        year += 2000 if year < 50 else 1900
        rest = text[2:]
    elif node.tag == 0x18:  # GeneralizedTime YYYYMMDDHHMMSSZ
        year = int(text[0:4])
        rest = text[4:]
    else:
        raise VerifyError("untrusted", "a certificate date is not a time")
    return dt.datetime(
        year,
        int(rest[0:2]),
        int(rest[2:4]),
        int(rest[4:6]),
        int(rest[6:8]),
        int(rest[8:10]),
        tzinfo=dt.timezone.utc,
    )


def _common_name(data: bytes, name: Tlv) -> str | None:
    found = None
    for rdn in children(data, name):
        for pair in children(data, rdn):
            oid, value = children(data, pair)[:2]
            if oid_text(oid.content(data)) != OID_CN:
                continue
            raw = value.content(data)
            found = raw.decode("utf-16-be") if value.tag == 0x1E else raw.decode("utf-8", "replace")
    return found


def parse_certificate(der: bytes) -> Certificate:
    try:
        return _parse_certificate(der)
    except (IndexError, ValueError, UnicodeDecodeError) as error:
        raise VerifyError("untrusted", f"a certificate cannot be read ({error})") from error


def _parse_certificate(der: bytes) -> Certificate:
    outer = read_tlv(der, 0)
    if outer.tag != 0x30 or outer.end != len(der):
        raise VerifyError("untrusted", "a certificate is not a DER SEQUENCE")
    tbs_node, alg_node, sig_node = children(der, outer)
    tbs_items = children(der, tbs_node)
    at = 0
    if tbs_items[0].tag == 0xA0:
        at = 1
    # serial, signature algorithm, issuer, validity, subject, spki
    issuer = tbs_items[at + 2]
    validity = children(der, tbs_items[at + 3])
    subject = tbs_items[at + 4]
    spki = tbs_items[at + 5]
    alg_oid = oid_text(children(der, alg_node)[0].content(der))
    if alg_oid == OID_RSA_PSS:
        raise VerifyError("untrusted", "a certificate signed with RSASSA-PSS is not supported")
    if alg_oid not in SIG_HASHES:
        raise VerifyError(
            "weak_key", f"a certificate is signed with an unsupported algorithm ({alg_oid})"
        )
    sig_kind, sig_hash = SIG_HASHES[alg_oid]
    sig_bits = sig_node.content(der)
    spki_parts = children(der, spki)
    key_alg = children(der, spki_parts[0])
    key_oid = oid_text(key_alg[0].content(der))
    key_bits = spki_parts[1].content(der)[1:]
    rsa_public = ec_curve = ec_public = None
    if key_oid == OID_RSA:
        seq = read_tlv(key_bits, 0)
        n_node, e_node = children(key_bits, seq)
        rsa_public = (
            int.from_bytes(n_node.content(key_bits), "big"),
            int.from_bytes(e_node.content(key_bits), "big"),
        )
        key_kind = "rsa"
    elif key_oid == OID_EC:
        curve_oid = oid_text(key_alg[1].content(der))
        ec_curve = CURVES.get(curve_oid)
        if ec_curve is None:
            raise VerifyError("weak_key", f"a certificate has an unsupported curve ({curve_oid})")
        if key_bits[0] != 4 or len(key_bits) != 1 + 2 * ec_curve.size:
            raise VerifyError("untrusted", "a certificate's EC point is not uncompressed")
        ec_public = (
            int.from_bytes(key_bits[1 : 1 + ec_curve.size], "big"),
            int.from_bytes(key_bits[1 + ec_curve.size :], "big"),
        )
        key_kind = "ec"
    else:
        raise VerifyError("weak_key", f"a certificate has an unsupported key type ({key_oid})")

    is_ca = False
    path_len = None
    key_usage = None
    eku = None
    if tbs_items[-1].tag == 0xA3:
        for ext in children(der, children(der, tbs_items[-1])[0]):
            parts = children(der, ext)
            oid = oid_text(parts[0].content(der))
            value = parts[-1]
            inner = value.content(der)
            if oid == OID_BASIC:
                seq = read_tlv(inner, 0)
                for item in children(inner, seq):
                    if item.tag == 0x01:
                        is_ca = item.content(inner) != b"\x00"
                    elif item.tag == 0x02:
                        path_len = int.from_bytes(item.content(inner), "big")
            elif oid == OID_KEY_USAGE:
                bits = read_tlv(inner, 0)
                body = bits.content(inner)
                unused, payload = body[0], body[1:]
                key_usage = 0
                for index in range(len(payload) * 8 - unused):
                    if payload[index // 8] & (0x80 >> (index % 8)):
                        key_usage |= 1 << index
            elif oid == OID_EKU:
                seq = read_tlv(inner, 0)
                eku = [oid_text(item.content(inner)) for item in children(inner, seq)]
    return Certificate(
        der=der,
        tbs=tbs_node.raw(der),
        sig_kind=sig_kind,
        sig_hash=sig_hash,
        signature=sig_bits[1:],
        issuer=issuer.raw(der),
        subject=subject.raw(der),
        not_before=_time(der, validity[0]),
        not_after=_time(der, validity[1]),
        key_kind=key_kind,
        rsa_public=rsa_public,
        ec_curve=ec_curve,
        ec_public=ec_public,
        is_ca=is_ca,
        path_len=path_len,
        key_usage=key_usage,
        ext_key_usage=eku,
        common_name=_common_name(der, subject),
    )


def check_key_strength(cert: Certificate) -> None:
    if (
        cert.key_kind == "rsa"
        and cert.rsa_public is not None
        and cert.rsa_public[0].bit_length() < 2048
    ):
        raise VerifyError("weak_key", "an RSA key is shorter than 2048 bits")


def signed_by(child: Certificate, issuer: Certificate) -> bool:
    """Whether `issuer`'s key made `child`'s signature (and the names agree)."""
    if child.issuer != issuer.subject:
        return False
    digest = hashlib.new(child.sig_hash, child.tbs).digest()
    if child.sig_kind == "rsa" and issuer.key_kind == "rsa" and issuer.rsa_public:
        return rsa_pkcs1_verify(issuer.rsa_public, child.sig_hash, digest, child.signature)
    if (
        child.sig_kind == "ecdsa"
        and issuer.key_kind == "ec"
        and issuer.ec_curve
        and issuer.ec_public
    ):
        r, s = der_signature(child.signature)
        return ecdsa_verify(issuer.ec_curve, issuer.ec_public, digest, r, s)
    return False


def read_pem_certificates(text: str) -> list[bytes]:
    found = re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", text, re.S)
    out = []
    for body in found:
        try:
            out.append(base64.b64decode("".join(body.split()), validate=True))
        except binascii.Error as error:
            raise VerifyError(
                "untrusted", "an anchor file holds a certificate that is not base64"
            ) from error
    if not out:
        raise VerifyError("untrusted", "the anchor file holds no certificate")
    return out


# --- the index and its signature ----------------------------------------------------------------

# Up to nine digits a number, as the board reads one; ASCII digits only; \Z, not $, which would let
# a trailing newline through.
_VERSION = re.compile(
    r"^v?(\d{1,9})\.(\d{1,9})\.(\d{1,9})(?:-(alpha|beta|rc)\.(\d{1,9}))?"
    r"(?:-(\d{1,9})-g[0-9a-f]*)?(?:-dirty)?\Z",
    re.ASCII,
)
_STAGES = {"alpha": 1, "beta": 2, "rc": 3}


def version_key(text: str) -> tuple[int, int, int, int, int, int] | None:
    """The order of a version (the board's comparison, `firmware_update.hpp`), or None when
    the text is not one it understands. A build N commits past a tag sorts after the tag and
    before the next one; `-beta.N` sorts before the same number without it."""
    match = _VERSION.match(text)
    if not match:
        return None
    major, minor, patch, stage, pre, commits = match.groups()
    return (
        int(major),
        int(minor),
        int(patch),
        _STAGES[stage] if stage else 4,
        int(pre) if pre else 0,
        int(commits) if commits else 0,
    )


def check_index(data: bytes, channel: str | None = None) -> dict[str, Any]:
    """The index file's schema, as the board reads it. Returns the parsed document."""
    if len(data) > MAX_INDEX_BYTES:
        raise VerifyError(
            "bad_index", f"the index is {len(data)} bytes; a board reads {MAX_INDEX_BYTES}"
        )
    try:
        doc = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as error:
        raise VerifyError("bad_index", f"the index is not JSON ({error})") from error
    if not isinstance(doc, dict) or doc.get("format") != 1:
        raise VerifyError("bad_index", "the index is not format 1")
    if doc.get("channel") not in CHANNELS or (channel is not None and doc["channel"] != channel):
        raise VerifyError("bad_index", f"the index is for channel {doc.get('channel')!r}")
    sequence = doc.get("sequence")
    if not isinstance(sequence, int) or isinstance(sequence, bool) or not 1 <= sequence < 2**32:
        raise VerifyError("bad_index", "the index has no sequence number")
    version = doc.get("version")
    if not isinstance(version, str) or version_key(version) is None:
        raise VerifyError(
            "bad_index", f"the index version {version!r} is not one a board understands"
        )
    if not isinstance(doc.get("released"), str) or not re.fullmatch(
        r"\d{4}-\d\d-\d\d", doc["released"]
    ):
        raise VerifyError("bad_index", "the index has no release date")
    images = doc.get("images")
    if not isinstance(images, list) or not 1 <= len(images) <= 16:
        raise VerifyError("bad_index", "the index lists no images, or more than 16")
    for image in images:
        _check_image(image)
    return doc


def _check_image(image: Any) -> None:
    if not isinstance(image, dict):
        raise VerifyError("bad_index", "an index image is not an object")

    def integer(key: str, low: int, high: int) -> None:
        value = image.get(key)
        if not isinstance(value, int) or isinstance(value, bool) or not low <= value <= high:
            raise VerifyError("bad_index", f"an index image's {key} is {value!r}")

    for key in ("name", "project"):
        if not isinstance(image.get(key), str) or not image[key]:
            raise VerifyError("bad_index", f"an index image has no {key}")
    file = image.get("file")
    if not isinstance(file, str) or not FILE_NAME.match(file):
        raise VerifyError(
            "bad_index", f"an index image's file name {file!r} is not one a board will fetch"
        )
    if not isinstance(image.get("sha256"), str) or not HEX64.match(image["sha256"]):
        raise VerifyError("bad_index", "an index image's sha256 is not 64 lower-case hex digits")
    integer("size", 1, 16 * 1024 * 1024)
    integer("chip_id", 0, 0xFFFF)
    integer("min_rev_full", 0, 0xFFFF)
    integer("max_rev_full", 0, 0xFFFF)
    integer("secure_version", 0, 2**32 - 1)
    if not isinstance(image.get("flash_size"), str) or not re.fullmatch(
        r"\d+MB", image["flash_size"]
    ):
        raise VerifyError("bad_index", "an index image's flash_size is not like 16MB")
    if not isinstance(image.get("psram"), bool):
        raise VerifyError("bad_index", "an index image's psram is not true or false")


@dataclass
class VerifyResult:
    leaf_name: str | None
    leaf_not_after: dt.datetime
    algorithm: str
    chain_length: int
    index: dict[str, Any]


def verify_pair(
    index_bytes: bytes,
    sig_bytes: bytes,
    anchors_pem: str,
    *,
    leaf_name: str | None = None,
    now: dt.datetime | None = None,
    channel: str | None = None,
) -> VerifyResult:
    """Verify a signed index. `now` None judges no dates (a build with no clock)."""
    if len(sig_bytes) > MAX_SIG_BYTES:
        raise VerifyError(
            "bad_sig_file",
            f"the signature file is {len(sig_bytes)} bytes; a board reads {MAX_SIG_BYTES}",
        )
    try:
        sig = json.loads(sig_bytes.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as error:
        raise VerifyError("bad_sig_file", f"the signature file is not JSON ({error})") from error
    if not isinstance(sig, dict) or sig.get("format") != SIG_FORMAT:
        raise VerifyError("bad_sig_file", "the signature file is not format 1")
    algorithm = sig.get("alg")
    if algorithm not in SIGNATURE_ALGS:
        raise VerifyError(
            "bad_sig_file", f"the signature algorithm {algorithm!r} is not one a board knows"
        )
    chain_text = sig.get("chain")
    if (
        not isinstance(chain_text, list)
        or not 1 <= len(chain_text) <= MAX_CHAIN - 1
        or not all(isinstance(item, str) for item in chain_text)
    ):
        raise VerifyError(
            "bad_sig_file", "the signature file's chain is not one or two certificates"
        )
    try:
        chain = [parse_certificate(base64.b64decode(item, validate=True)) for item in chain_text]
        signature = base64.b64decode(str(sig.get("signature", "")), validate=True)
    except binascii.Error as error:
        raise VerifyError(
            "bad_sig_file", "the signature file holds text that is not base64"
        ) from error
    if sig.get("sha256") != hashlib.sha256(index_bytes).hexdigest():
        raise VerifyError("hash_mismatch", "the signature file is for another index than this one")

    anchors = [parse_certificate(der) for der in read_pem_certificates(anchors_pem)]
    _check_chain(chain, anchors, now)

    leaf = chain[0]
    check_key_strength(leaf)
    if leaf.is_ca:
        raise VerifyError("leaf_usage", "the signing certificate is a CA")
    if leaf.key_usage is None or not leaf.key_usage & 1:
        raise VerifyError(
            "leaf_usage", "the signing certificate's key usage lacks digitalSignature"
        )
    if leaf.ext_key_usage is None or OID_CODE_SIGNING not in leaf.ext_key_usage:
        raise VerifyError("leaf_usage", "the signing certificate is not for code signing")
    if leaf_name is not None and leaf.common_name != leaf_name:
        raise VerifyError(
            "leaf_name", f"the signing certificate is named {leaf.common_name!r}, not {leaf_name!r}"
        )

    message_hash = hashlib.sha256(index_bytes).digest()
    ok = False
    if algorithm == "ecdsa-p256-sha256":
        if (
            leaf.key_kind != "ec"
            or leaf.ec_curve is not P256
            or leaf.ec_public is None
            or len(signature) != 64
        ):
            raise VerifyError(
                "bad_signature", "the signature is ECDSA P-256 and the signing key is not"
            )
        ok = ecdsa_verify(
            P256,
            leaf.ec_public,
            message_hash,
            int.from_bytes(signature[:32], "big"),
            int.from_bytes(signature[32:], "big"),
        )
    else:
        if leaf.key_kind != "rsa" or leaf.rsa_public is None:
            raise VerifyError("bad_signature", "the signature is RSA and the signing key is not")
        if algorithm == "rsa-pkcs1-sha256":
            ok = rsa_pkcs1_verify(leaf.rsa_public, "sha256", message_hash, signature)
        else:
            ok = rsa_pss_verify(leaf.rsa_public, "sha256", message_hash, signature)
    if not ok:
        raise VerifyError("bad_signature", "the signature does not match the index")
    return VerifyResult(
        leaf.common_name, leaf.not_after, algorithm, len(chain), check_index(index_bytes, channel)
    )


def _check_chain(
    chain: list[Certificate], anchors: list[Certificate], now: dt.datetime | None
) -> None:
    anchor_ders = {anchor.der for anchor in anchors}
    below = 0  # CA certificates under the one being examined
    for position, cert in enumerate(chain):
        if cert.der in anchor_ders:
            return  # an anchor itself: its own dates are not looked at
        if now is not None:
            if now < cert.not_before:
                raise VerifyError(
                    "cert_not_yet_valid",
                    "a certificate in the chain is not valid yet (or this board's date is wrong)",
                )
            if now > cert.not_after:
                raise VerifyError(
                    "cert_expired",
                    "a certificate in the chain has expired (or this board's date is wrong)",
                )
        check_key_strength(cert)
        if position > 0:
            if not cert.is_ca:
                raise VerifyError("untrusted", "an intermediate certificate is not a CA")
            if cert.key_usage is not None and not cert.key_usage & (1 << 5):
                raise VerifyError(
                    "untrusted", "an intermediate certificate may not sign certificates"
                )
            if cert.path_len is not None and below > cert.path_len:
                raise VerifyError(
                    "untrusted", "an intermediate certificate's path length is exceeded"
                )
            below += 1
        if position + 1 < len(chain) and signed_by(cert, chain[position + 1]):
            continue
        for anchor in anchors:
            if not anchor.is_ca and anchor.der != cert.der:
                continue
            if signed_by(cert, anchor):
                if anchor.path_len is not None and below > anchor.path_len:
                    raise VerifyError("untrusted", "the anchor's path length is exceeded")
                return
        raise VerifyError(
            "untrusted", "the signing certificate does not chain to this build's trust anchor"
        )
    raise VerifyError(
        "untrusted", "the signing certificate does not chain to this build's trust anchor"
    )


def parse_time(text: str) -> dt.datetime:
    value = dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
    return value if value.tzinfo else value.replace(tzinfo=dt.timezone.utc)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--index", type=Path, required=True)
    parser.add_argument("--sig", type=Path, required=True)
    parser.add_argument("--anchors", type=Path, required=True)
    parser.add_argument("--leaf-name", help="the common name the leaf must have")
    parser.add_argument("--channel", choices=CHANNELS)
    parser.add_argument(
        "--now", help="judge dates at this time (ISO 8601); default is the current time"
    )
    parser.add_argument(
        "--no-clock", action="store_true", help="judge no dates, like a build with no clock"
    )
    args = parser.parse_args(argv)
    now = (
        None
        if args.no_clock
        else (parse_time(args.now) if args.now else dt.datetime.now(dt.timezone.utc))
    )
    try:
        result = verify_pair(
            args.index.read_bytes(),
            args.sig.read_bytes(),
            args.anchors.read_text("utf-8"),
            leaf_name=args.leaf_name,
            now=now,
            channel=args.channel,
        )
    except OSError as error:
        print(f"index_verify: {error}", file=sys.stderr)
        return 1
    except VerifyError as error:
        print(f"index_verify: not trusted: {error}", file=sys.stderr)
        return 1
    print(
        f"index_verify: {args.index.name} is signed by {result.leaf_name!r} ({result.algorithm}, "
        f"valid to {result.leaf_not_after:%Y-%m-%d}), channel {result.index['channel']}, "
        f"{result.index['version']}, sequence {result.index['sequence']}, "
        f"{len(result.index['images'])} images"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
