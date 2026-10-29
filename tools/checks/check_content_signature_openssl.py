#!/usr/bin/env python3
"""Does OpenSSL accept what forge's AC-4 content signing writes, and what a real blob holds?

forge verifies its own signatures, and a signer and a verifier written together can agree on
something nobody else would accept. This reads a content signature the way a third party would, with
no forge code in the loop beyond the stream itself, and asks OpenSSL (the oracle for ECDSA and X.509
in CONTRIBUTING.md's list) to check each claim:

  1. The certificates the signature carries parse (`openssl x509`), and the signing certificate
     chains to the root (`openssl verify`).
  2. The SHA-256 of every byte before the trailer is the content digest the signature holds
     (--forge only: a bare blob has no stream).
  3. The SHA-256 of the signer information, as received, is the digest the signed information holds.
  4. The ECDSA signature (r and s from the blob, put into a DER Ecdsa-Sig-Value) verifies under the
     signing certificate's public key over the bytes of the signed information
     (`openssl dgst -sha256 -verify`).
  5. The same check fails when one bit of the signed information is changed, and when the content
     digest is taken as the thing that was signed, so it can fail and the reading is the one that
     verifies.

The blob is read with a small Thrift binary reader written here from the layout in
libs/ac4/ERRATA.md, not from forge's. With --forge the chain is made with `openssl` at run time from
fresh keys; nothing is kept and no key or certificate of any issuer is involved.

usage: check_content_signature_openssl.py (--forge <path-to-forge> | --sample <blob>)
                                          [--openssl openssl] [--keep]

--sample reads a bare blob, such as one extracted from somewhere else, and runs the checks that need
no stream (1 and 3 to 5); nothing of forge is run, and nothing of the blob is printed beyond lengths
and verdicts. Exits 0 when every check passes, 1 when one fails, 77 when openssl is not available.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

# Thrift binary protocol type codes.
T_STOP, T_BOOL, T_BYTE, T_DOUBLE, T_I16, T_I32, T_I64, T_STRING = 0, 2, 3, 4, 6, 8, 10, 11
T_STRUCT, T_MAP, T_SET, T_LIST = 12, 13, 14, 15


class ByteList(bytes):
    """A Thrift list whose elements are of type BYTE, as opposed to a binary string."""


class Reader:
    """A Thrift binary reader that keeps the byte span and field order of every struct it reads."""

    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def take(self, n: int) -> bytes:
        if n < 0 or self.pos + n > len(self.data):
            raise ValueError(f"read of {n} bytes at {self.pos} runs past the {len(self.data)}-byte blob")
        out = self.data[self.pos : self.pos + n]
        self.pos += n
        return out

    def value(self, ttype: int):
        if ttype == T_BOOL or ttype == T_BYTE:
            return self.take(1)[0]
        if ttype == T_I16:
            return struct.unpack(">h", self.take(2))[0]
        if ttype == T_I32:
            return struct.unpack(">i", self.take(4))[0]
        if ttype == T_I64:
            return struct.unpack(">q", self.take(8))[0]
        if ttype == T_DOUBLE:
            return struct.unpack(">d", self.take(8))[0]
        if ttype == T_STRING:
            return self.take(struct.unpack(">i", self.take(4))[0])
        if ttype == T_STRUCT:
            return self.struct()
        if ttype in (T_LIST, T_SET):
            element = self.take(1)[0]
            count = struct.unpack(">i", self.take(4))[0]
            if element == T_BYTE:
                return ByteList(self.take(count))  # a list<byte> is kept as the bytes it holds
            return [self.value(element) for _ in range(count)]
        if ttype == T_MAP:
            key_type, value_type = self.take(1)[0], self.take(1)[0]
            count = struct.unpack(">i", self.take(4))[0]
            return {self.value(key_type): self.value(value_type) for _ in range(count)}
        raise ValueError(f"unknown Thrift type {ttype} at {self.pos}")

    def struct(self) -> dict:
        start = self.pos
        fields: dict = {}
        order: list[int] = []
        while True:
            ttype = self.take(1)[0]
            if ttype == T_STOP:
                break
            (field_id,) = struct.unpack(">h", self.take(2))
            order.append(field_id)
            fields[field_id] = self.value(ttype)
        fields["span"] = (start, self.pos)
        fields["order"] = order
        return fields


def syncframe_end(stream: bytes) -> int:
    """The offset where the sync frames stop, which is where a trailer starts (CS spec v2.2 8.5)."""
    pos = 0
    while pos + 4 <= len(stream) and stream[pos] == 0xAC and stream[pos + 1] in (0x40, 0x41):
        has_crc = stream[pos + 1] == 0x41
        size = int.from_bytes(stream[pos + 2 : pos + 4], "big")
        header = 4
        if size == 0xFFFF:
            size = int.from_bytes(stream[pos + 4 : pos + 7], "big")
            header = 7
        pos += header + size + (2 if has_crc else 0)
    return pos


def der_sequences(data: bytes) -> list[bytes]:
    """The DER SEQUENCEs laid end to end in data (the certificates)."""
    out, pos = [], 0
    while pos < len(data):
        if data[pos] != 0x30:
            raise ValueError(f"a certificate does not start with a SEQUENCE at {pos}")
        length_byte = data[pos + 1]
        if length_byte < 0x80:
            header, length = 2, length_byte
        else:
            count = length_byte & 0x7F
            header, length = 2 + count, int.from_bytes(data[pos + 2 : pos + 2 + count], "big")
        out.append(data[pos : pos + header + length])
        pos += header + length
    return out


def der_length(n: int) -> bytes:
    if n < 0x80:
        return bytes([n])
    raw = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(raw)]) + raw


def der_integer(value: bytes) -> bytes:
    value = value.lstrip(b"\x00") or b"\x00"
    if value[0] & 0x80:
        value = b"\x00" + value
    return b"\x02" + der_length(len(value)) + value


def ecdsa_sig_value(r: bytes, s: bytes) -> bytes:
    body = der_integer(r) + der_integer(s)
    return b"\x30" + der_length(len(body)) + body


class Check:
    def __init__(self):
        self.failures = 0

    def ok(self, condition: bool, message: str) -> bool:
        print(("  ok    " if condition else "  FAIL  ") + message)
        if not condition:
            self.failures += 1
        return condition


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, **kwargs)


def make_pki(openssl: str, work: Path) -> None:
    """A root, an issuing CA and a signer whose subject carries the title the spec asks for."""

    def sh(*args: str) -> None:
        result = run([openssl, *args], cwd=work)
        if result.returncode != 0:
            raise RuntimeError(f"openssl {' '.join(args)}: {result.stderr.strip()}")

    for name in ("root", "issuing", "signer"):
        sh("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", f"{name}.key")
    sh("req", "-new", "-x509", "-key", "root.key", "-sha256", "-days", "30",
       "-subj", "/C=AU/O=ICL Forge Test/CN=Test Root CA",
       "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign",
       "-out", "root.pem")
    (work / "ca.ext").write_text("basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign\n")
    (work / "signer.ext").write_text("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n")
    sh("req", "-new", "-key", "issuing.key", "-sha256", "-subj", "/C=AU/O=ICL Forge Test/CN=Test Issuing CA",
       "-out", "issuing.csr")
    sh("x509", "-req", "-in", "issuing.csr", "-CA", "root.pem", "-CAkey", "root.key", "-CAcreateserial",
       "-sha256", "-days", "30", "-extfile", "ca.ext", "-out", "issuing.pem")
    sh("req", "-new", "-key", "signer.key", "-sha256",
       "-subj", "/C=AU/O=ICL Forge Test/CN=Test Content Signer/title=Code\\/Content Signer", "-out", "signer.csr")
    sh("x509", "-req", "-in", "signer.csr", "-CA", "issuing.pem", "-CAkey", "issuing.key", "-CAcreateserial",
       "-sha256", "-days", "30", "-extfile", "signer.ext", "-out", "signer.pem")
    (work / "chain.pem").write_text(
        (work / "signer.pem").read_text() + (work / "issuing.pem").read_text() + (work / "root.pem").read_text()
    )


def write_wav(path: Path) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(48000)
        frames = bytearray()
        for n in range(48000 // 2):
            v = int(6000 * math.sin(2 * math.pi * 440 * n / 48000))
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))


def analyse_blob(blob: bytes, work: Path, openssl: str, check: Check) -> dict:
    """Everything that can be said of a blob without the stream it was made for.

    Returns the content digest it holds and the certificates it carries, for the caller to use.
    """
    content_signature = Reader(blob).struct()
    signer_info = content_signature[3]
    entry = content_signature[4][0]
    signed_info = entry[1]
    signature_data = entry[2]
    signed_info_bytes = blob[signed_info["span"][0] : signed_info["span"][1]]
    signer_info_bytes = blob[signer_info["span"][0] : signer_info["span"][1]]
    content_digest = signed_info[7][0][32]

    # Every byte list is a list of BYTE elements, and fields are written in ascending id order.
    byte_lists = {
        "nonce": signer_info[1],
        "content digest": content_digest,
        "signer information digest": signed_info[6][32],
        "certificates": signer_info[10][4],
        "r": signature_data[4][b"ecc_r_value"],
        "s": signature_data[4][b"ecc_s_value"],
    }
    check.ok(
        all(isinstance(v, ByteList) for v in byte_lists.values()),
        "every byte list is a Thrift list of BYTE elements: " + ", ".join(byte_lists),
    )
    structs = [content_signature, signer_info, signer_info[10], entry, signed_info, signed_info[6], signed_info[7][0], signature_data]
    check.ok(all(st["order"] == sorted(st["order"]) for st in structs), "fields are written in ascending id order in every struct")

    # The signer-information digest is over the bytes as received.
    check.ok(
        hashlib.sha256(signer_info_bytes).digest() == signed_info[6][32],
        "SHA-256 of the signer information, as received, is the digest the signed information holds",
    )

    # The certificates: a wrapper, then DER back to back.
    wrapped = bytes(signer_info[10][4])
    check.ok(
        wrapped[0] == 0x40 and wrapped[1] == 0x82 and int.from_bytes(wrapped[2:4], "big") == len(wrapped) - 4,
        "the certificates start with a 40 82 LL LL wrapper whose length is the rest of the list",
    )
    certificates = der_sequences(wrapped[4:])
    for index, der in enumerate(certificates):
        (work / f"carried{index}.der").write_bytes(der)
        parsed = run([openssl, "x509", "-inform", "DER", "-in", f"carried{index}.der", "-noout", "-subject"], cwd=work)
        check.ok(parsed.returncode == 0, f"openssl reads carried certificate {index} of {len(certificates)}")
        pem = run([openssl, "x509", "-inform", "DER", "-in", f"carried{index}.der", "-outform", "PEM"], cwd=work).stdout
        (work / f"carried{index}.pem").write_text(pem)

    # The ECDSA signature, over the bytes of the signed information.
    r, s = signature_data[4][b"ecc_r_value"], signature_data[4][b"ecc_s_value"]
    check.ok(len(r) == 32 and len(s) == 32, f"r and s are raw 32-byte values, not DER: {len(r)} and {len(s)}")
    (work / "signature.der").write_bytes(ecdsa_sig_value(r, s))
    (work / "signed_info.bin").write_bytes(signed_info_bytes)
    (work / "content_digest.bin").write_bytes(bytes(content_digest))
    public_key = run([openssl, "x509", "-in", f"carried{len(certificates) - 1}.pem", "-pubkey", "-noout"], cwd=work)
    (work / "signer_pub.pem").write_text(public_key.stdout)

    def dgst(message: str) -> subprocess.CompletedProcess:
        return run([openssl, "dgst", "-sha256", "-verify", "signer_pub.pem", "-signature", "signature.der", message], cwd=work)

    verify = dgst("signed_info.bin")
    check.ok(
        verify.returncode == 0 and "Verified OK" in verify.stdout,
        "openssl dgst -verify accepts the signature over the signed information",
    )
    damaged = bytearray(signed_info_bytes)
    damaged[len(damaged) // 2] ^= 0x01
    (work / "damaged_info.bin").write_bytes(bytes(damaged))
    check.ok(dgst("damaged_info.bin").returncode != 0, "the same check refuses the signed information with one bit changed")
    # The reading CS spec v2.3 gives (the content digest is what is signed) must not verify.
    check.ok(dgst("content_digest.bin").returncode != 0, "the signature does not verify over SHA-256 of the content digest")
    raw_digest = run(
        [openssl, "pkeyutl", "-verify", "-pubin", "-inkey", "signer_pub.pem", "-sigfile", "signature.der", "-in", "content_digest.bin"],
        cwd=work,
    )
    check.ok(raw_digest.returncode != 0, "the signature does not verify with the content digest taken as the hash that was signed")
    return {"content_digest": bytes(content_digest), "certificates": certificates}


def check_sample(blob_path: Path, work: Path, openssl: str, check: Check) -> None:
    """A blob from somewhere else: what OpenSSL says of it, with no stream to compare it to."""
    parts = analyse_blob(blob_path.read_bytes(), work, openssl, check)
    count = len(parts["certificates"])
    # The first carried certificate is the root and the last the signer. The check that uses the
    # time is reported and not asserted: it depends on the day this runs.
    command = [openssl, "verify", "-CAfile", "carried0.pem"]
    for index in range(1, count - 1):
        command += ["-untrusted", f"carried{index}.pem"]
    command.append(f"carried{count - 1}.pem")
    ignoring_time = run(command[:2] + ["-no_check_time"] + command[2:], cwd=work)
    check.ok(ignoring_time.returncode == 0, "openssl verify chains the last certificate to the first, ignoring validity times")
    with_time = run(command, cwd=work)
    # Only the reason: openssl's first line names the certificate, which is not this script's to print.
    reasons = re.findall(r"error \d+ at \d+ depth lookup: [^\n]*", with_time.stdout + with_time.stderr)
    detail = "; ".join(reasons) if reasons else "no error"
    print(f"  info  the same with validity times checked today: exit {with_time.returncode}: {detail}")
    text = run([openssl, "x509", "-in", f"carried{count - 1}.pem", "-noout", "-text"], cwd=work).stdout
    check.ok("ecdsa-with-SHA256" in text, "the signing certificate is signed ecdsa-with-SHA256")
    check.ok(
        "CA:TRUE" in text,
        "the signing certificate also carries BasicConstraints CA:TRUE, which a verifier must not reject",
    )


def check_forge(forge: str, work: Path, openssl: str, check: Check) -> None:
    make_pki(openssl, work)
    write_wav(work / "tone.wav")

    def forge_run(*arguments: str) -> subprocess.CompletedProcess:
        return run([forge, *arguments], cwd=work)

    check.ok(forge_run("ac4-encode", "tone.wav", "plain.ac4", "192").returncode == 0, "forge ac4-encode writes the unsigned stream")
    signed = forge_run("ac4-sign", "plain.ac4", "signed.ac4", "content-key=signer.key", "content-chain=chain.pem")
    if not check.ok(signed.returncode == 0, "forge ac4-sign signs it with the OpenSSL-made key and chain"):
        print(signed.stderr, file=sys.stderr)
        return

    plain = (work / "plain.ac4").read_bytes()
    stream = (work / "signed.ac4").read_bytes()
    boundary = syncframe_end(stream)
    check.ok(stream[:boundary] == plain and boundary == len(plain), "the sync frames are the unsigned stream's, byte for byte")

    parts = analyse_blob(stream[boundary:], work, openssl, check)
    check.ok(
        hashlib.sha256(stream[:boundary]).digest() == parts["content_digest"],
        "SHA-256 of the sync frames is the content digest in the signed information",
    )
    check.ok(len(parts["certificates"]) == 3, f"three certificates are carried (root, issuing CA, signer): {len(parts['certificates'])}")
    verified = run([openssl, "verify", "-CAfile", "root.pem", "-untrusted", "issuing.pem", "signer.pem"], cwd=work)
    check.ok(verified.returncode == 0, f"openssl verify chains the signer to the root: {verified.stdout.strip() or verified.stderr.strip()}")
    check.ok(
        (work / "carried2.pem").read_text() == (work / "signer.pem").read_text(),
        "the last carried certificate is the signer's certificate exactly",
    )
    subject = run([openssl, "x509", "-in", "carried2.pem", "-noout", "-subject"], cwd=work).stdout
    check.ok("Code/Content Signer" in subject, "the signer carries the title the spec asks for")

    # forge agrees, and refuses what is damaged.
    check.ok(forge_run("ac4-verify", "signed.ac4", "trust-anchor=root.pem").returncode == 0, "forge ac4-verify accepts it against the same root")
    flipped = bytearray(stream)
    flipped[len(plain) // 2] ^= 0x01
    (work / "flipped.ac4").write_bytes(bytes(flipped))
    check.ok(
        forge_run("ac4-verify", "flipped.ac4", "trust-anchor=root.pem").returncode == 6,
        "forge ac4-verify refuses it with one bit of audio changed (exit 6)",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--forge", help="the forge executable: sign a stream it encodes, and read that")
    modes.add_argument("--sample", help="a bare content signature blob to read; nothing of forge is run")
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--keep", action="store_true", help="keep the working directory and print it")
    args = parser.parse_args()

    if shutil.which(args.openssl) is None:
        print(f"{args.openssl} not found: nothing to cross-check against", file=sys.stderr)
        return 77
    work = Path(tempfile.mkdtemp(prefix="content-signature-openssl-"))
    check = Check()
    try:
        if args.sample:
            check_sample(Path(args.sample), work, args.openssl, check)
        else:
            check_forge(str(Path(args.forge).resolve()), work, args.openssl, check)
    except Exception as error:  # a malformed blob is a failure of the check, not a crash
        print(f"  FAIL  {type(error).__name__}: {error}")
        check.failures += 1
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    print("all checks passed" if check.failures == 0 else f"{check.failures} check(s) failed")
    return 0 if check.failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
