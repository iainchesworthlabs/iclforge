#!/usr/bin/env python3
"""Does OpenSSL accept what forge's AC-4 content signing writes?

forge verifies its own signatures, and a signer and a verifier written together can agree on
something nobody else would accept. This reads a signed stream the way a third party would, with no
forge code in the loop beyond the stream itself, and asks OpenSSL (the oracle for ECDSA and X.509
in CONTRIBUTING.md's list) to check each claim:

  1. The certificates the signature carries parse (`openssl x509`), and the signing certificate
     chains to the root the test generated (`openssl verify`).
  2. The SHA-256 of every byte before the trailer is the content digest the signature holds.
  3. The SHA-256 of the signer information, as received, is the digest the signed information holds.
  4. The ECDSA signature (r and s from the blob, put into a DER Ecdsa-Sig-Value) verifies under the
     signing certificate's public key over the bytes of the signed information
     (`openssl dgst -sha256 -verify`).
  5. The same check fails when one bit of the signed information is changed, so it can fail.

The blob is read with a small Thrift binary reader written here from the layout in
libs/ac4/ERRATA.md, not from forge's. The certificate chain is made with `openssl` at run time from
fresh keys; nothing is kept and no key or certificate of any issuer is involved.

usage: check_content_signature_openssl.py --forge <path-to-forge> [--openssl openssl] [--keep]
Exits 0 when every check passes, 1 when one fails, 77 when openssl is not available.
"""

from __future__ import annotations

import argparse
import hashlib
import math
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


class Reader:
    """A Thrift binary reader that keeps the byte span of every struct it reads."""

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
                return self.take(count)  # a list<byte> is kept as the bytes it holds
            return [self.value(element) for _ in range(count)]
        if ttype == T_MAP:
            key_type, value_type = self.take(1)[0], self.take(1)[0]
            count = struct.unpack(">i", self.take(4))[0]
            return {self.value(key_type): self.value(value_type) for _ in range(count)}
        raise ValueError(f"unknown Thrift type {ttype} at {self.pos}")

    def struct(self) -> dict:
        start = self.pos
        fields: dict = {}
        while True:
            ttype = self.take(1)[0]
            if ttype == T_STOP:
                break
            (field_id,) = struct.unpack(">h", self.take(2))
            fields[field_id] = self.value(ttype)
        fields["span"] = (start, self.pos)
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


def der_integer(value: bytes) -> bytes:
    value = value.lstrip(b"\x00") or b"\x00"
    if value[0] & 0x80:
        value = b"\x00" + value
    return b"\x02" + der_length(len(value)) + value


def der_length(n: int) -> bytes:
    if n < 0x80:
        return bytes([n])
    raw = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(raw)]) + raw


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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--forge", required=True, help="the forge executable")
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--keep", action="store_true", help="keep the working directory and print it")
    args = parser.parse_args()

    if shutil.which(args.openssl) is None:
        print(f"{args.openssl} not found: nothing to cross-check against", file=sys.stderr)
        return 77
    forge = str(Path(args.forge).resolve())
    work = Path(tempfile.mkdtemp(prefix="content-signature-openssl-"))
    check = Check()
    try:
        make_pki(args.openssl, work)
        write_wav(work / "tone.wav")
        forge_run = lambda *a: run([forge, *a], cwd=work)  # noqa: E731
        encoded = forge_run("ac4-encode", "tone.wav", "plain.ac4", "192")
        check.ok(encoded.returncode == 0, "forge ac4-encode writes the unsigned stream")
        signed = forge_run("ac4-sign", "plain.ac4", "signed.ac4", "content-key=signer.key", "content-chain=chain.pem")
        if not check.ok(signed.returncode == 0, "forge ac4-sign signs it with the OpenSSL-made key and chain"):
            print(signed.stderr, file=sys.stderr)
            return 1

        plain = (work / "plain.ac4").read_bytes()
        stream = (work / "signed.ac4").read_bytes()
        boundary = syncframe_end(stream)
        check.ok(stream[:boundary] == plain and boundary == len(plain), "the sync frames are the unsigned stream's, byte for byte")
        blob = stream[boundary:]

        # --- the blob, read with the reader above ---
        content_signature = Reader(blob).struct()
        signer_info = content_signature[3]
        entry = content_signature[4][0]
        signed_info = entry[1]
        signature_data = entry[2]
        signed_info_bytes = blob[signed_info["span"][0] : signed_info["span"][1]]
        signer_info_bytes = blob[signer_info["span"][0] : signer_info["span"][1]]

        # --- 2: the content digest ---
        digests = signed_info[7]
        content_digest = digests[0][32]
        check.ok(
            hashlib.sha256(stream[:boundary]).digest() == content_digest,
            "SHA-256 of the sync frames is the content digest in the signed information",
        )
        # --- 3: the signer-information digest ---
        check.ok(
            hashlib.sha256(signer_info_bytes).digest() == signed_info[6][32],
            "SHA-256 of the signer information, as received, is the digest the signed information holds",
        )

        # --- 1: the certificates ---
        wrapped = signer_info[10][4]
        check.ok(wrapped[0] == 0x40 and wrapped[1] == 0x82, "the certificates are wrapped as the sample's are (40 82 LL LL)")
        body = wrapped[4 : 4 + int.from_bytes(wrapped[2:4], "big")]
        certificates = der_sequences(body)
        check.ok(len(certificates) == 3, f"three certificates are carried (root, issuing CA, signer): {len(certificates)}")
        for index, der in enumerate(certificates):
            (work / f"carried{index}.der").write_bytes(der)
        subjects = []
        for index in range(len(certificates)):
            parsed = run([args.openssl, "x509", "-inform", "DER", "-in", f"carried{index}.der", "-noout", "-subject"], cwd=work)
            subjects.append(parsed.stdout.strip())
            check.ok(parsed.returncode == 0, f"openssl reads carried certificate {index}: {parsed.stdout.strip()}")
        check.ok("Test Root CA" in subjects[0], "the root comes first, as in the sample")
        check.ok("Code/Content Signer" in subjects[-1], "the signer, with its title, comes last")
        verified = run(
            [args.openssl, "verify", "-CAfile", "root.pem", "-untrusted", "issuing.pem", "signer.pem"], cwd=work
        )
        check.ok(verified.returncode == 0, f"openssl verify chains the signer to the root: {verified.stdout.strip() or verified.stderr.strip()}")
        check.ok(
            run([args.openssl, "x509", "-inform", "DER", "-in", f"carried{len(certificates) - 1}.der", "-outform", "PEM"], cwd=work).stdout
            == (work / "signer.pem").read_text(),
            "the last carried certificate is the signer's certificate exactly",
        )

        # --- 4 and 5: the ECDSA signature ---
        rs = signature_data[4]
        r, s = rs[b"ecc_r_value"], rs[b"ecc_s_value"]
        check.ok(len(r) == 32 and len(s) == 32, "r and s are 32 bytes each")
        (work / "signature.der").write_bytes(ecdsa_sig_value(r, s))
        (work / "signed_info.bin").write_bytes(signed_info_bytes)
        public_key = run([args.openssl, "x509", "-in", "signer.pem", "-pubkey", "-noout"], cwd=work)
        (work / "signer_pub.pem").write_text(public_key.stdout)
        verify = run(
            [args.openssl, "dgst", "-sha256", "-verify", "signer_pub.pem", "-signature", "signature.der", "signed_info.bin"],
            cwd=work,
        )
        check.ok(
            verify.returncode == 0 and "Verified OK" in verify.stdout,
            "openssl dgst -verify accepts the ECDSA signature over the signed information",
        )
        damaged = bytearray(signed_info_bytes)
        damaged[len(damaged) // 2] ^= 0x01
        (work / "damaged_info.bin").write_bytes(bytes(damaged))
        refused = run(
            [args.openssl, "dgst", "-sha256", "-verify", "signer_pub.pem", "-signature", "signature.der", "damaged_info.bin"],
            cwd=work,
        )
        check.ok(refused.returncode != 0, "the same check refuses the signed information with one bit changed")

        # --- forge agrees, and refuses what OpenSSL's reading says is damaged ---
        agreed = forge_run("ac4-verify", "signed.ac4", "trust-anchor=root.pem")
        check.ok(agreed.returncode == 0, "forge ac4-verify accepts it against the same root")
        flipped = bytearray(stream)
        flipped[len(plain) // 2] ^= 0x01
        (work / "flipped.ac4").write_bytes(bytes(flipped))
        check.ok(
            forge_run("ac4-verify", "flipped.ac4", "trust-anchor=root.pem").returncode == 6,
            "forge ac4-verify refuses it with one bit of audio changed (exit 6)",
        )
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
