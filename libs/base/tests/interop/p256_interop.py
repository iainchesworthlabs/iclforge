#!/usr/bin/env python3
"""Differential test of the in-tree P-256 / ECDSA / X.509 code against OpenSSL.

libs/base/tests/test_interop.cpp drives this script in three steps, with a directory of its own
(`--dir`) that both sides read and write:

  generate   OpenSSL makes the material: keys in every format it writes, signatures over random
             digests, valid and tampered, and certificate chains for a set of scenarios, each with
             OpenSSL's own verdict on the chain. Nothing here is committed key material: every
             key lives only in the directory, which is a scratch directory of the test run.
  (the C++ test then reads all of that with the library under test, checks that every signature
   and every verdict agrees, and writes what it makes itself: signatures, keys and certificates)
  verify     OpenSSL checks what the library wrote: its signatures verify, its certificates parse
             and are signed by the keys they claim, `openssl verify` accepts its chain, and the
             keys it wrote load and give the public key it said.

The script exits 0 when everything it checked holds, 1 on a disagreement (printing what), and 77
when the tools are missing (CTest's and the test's convention for "skipped").

Why a script and not C++: the point is a second implementation, and OpenSSL is the one the whole
industry checks against. The `cryptography` package is OpenSSL's API; the `openssl` command line
is used where a person would use it, for the files a person is handed (key formats, a v1
certificate), so that what the loaders accept is what `openssl` writes.
"""

import argparse
import datetime
import os
import pathlib
import secrets
import shutil
import subprocess
import sys

try:
    from cryptography import x509
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import Prehashed
    from cryptography.x509.oid import NameOID
except ImportError:
    print("skip: the Python 'cryptography' package is not installed")
    sys.exit(77)

if shutil.which("openssl") is None:
    print("skip: the openssl command line tool is not installed")
    sys.exit(77)

# The time every chain is judged at, the same as in the C++ test: 2026-10-29T00:00:00Z.
NOW = 1793232000
NOW_DT = datetime.datetime.fromtimestamp(NOW, datetime.timezone.utc)
# The specification's leaf profile: title, common name and serial (section 5).
LEAF_SERIAL = 0x1798A2B5D62000000005
UTC = datetime.timezone.utc

failures = []


def fail(message: str) -> None:
    failures.append(message)
    print(f"FAIL: {message}")


def date(y, m, d):
    return datetime.datetime(y, m, d, tzinfo=UTC)


def run(*args, check=True, input=None):
    result = subprocess.run(args, capture_output=True, text=True, input=input)
    if check and result.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} failed:\n{result.stdout}\n{result.stderr}")
    return result


def uncompressed(public_key) -> bytes:
    return public_key.public_bytes(serialization.Encoding.X962,
                                   serialization.PublicFormat.UncompressedPoint)


def sign_der(private_key, digest: bytes) -> bytes:
    return private_key.sign(digest, ec.ECDSA(Prehashed(hashes.SHA256())))


def verifies(public_key, digest: bytes, der: bytes) -> bool:
    try:
        public_key.verify(der, digest, ec.ECDSA(Prehashed(hashes.SHA256())))
        return True
    except (InvalidSignature, ValueError):
        return False


def serial_hex(value: int) -> str:
    """Uppercase hexadecimal of whole bytes, the form Certificate::serial_hex writes ("01", not
    "1"): a serial is an octet string with the sign byte taken away."""
    length = max(1, (value.bit_length() + 7) // 8)
    return f"{value:0{2 * length}X}"


def public_from_hex(text: str):
    return ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), bytes.fromhex(text))


# --- generate ---------------------------------------------------------------------------------

def generate_signatures(d: pathlib.Path) -> None:
    """Signatures OpenSSL makes (random nonces, so no two runs are alike) and tampered ones."""
    valid = []
    invalid = []
    digests = [b"\x00" * 32, b"\xff" * 32, b"\x01" + b"\x00" * 31, b"\x00" * 31 + b"\x01"]
    for i in range(300):
        key = ec.generate_private_key(ec.SECP256R1())
        digest = digests[i] if i < len(digests) else secrets.token_bytes(32)
        der = sign_der(key, digest)
        pub = key.public_key()
        valid.append((uncompressed(pub), digest, der))
        # One thing wrong at a time: the digest, a signature byte, the key.
        flipped = bytes([digest[0] ^ (1 << (i % 8))]) + digest[1:]
        bad_sig = bytearray(der)
        bad_sig[-1 - (i % 8)] ^= 0x01
        other = ec.generate_private_key(ec.SECP256R1()).public_key()
        for pub_bytes, d2, s2 in ((uncompressed(pub), flipped, der),
                                  (uncompressed(pub), digest, bytes(bad_sig)),
                                  (uncompressed(other), digest, der)):
            invalid.append((pub_bytes, d2, s2))
    # Whatever the labels, the invalid ones must be invalid for OpenSSL too, or the test would
    # be asserting something about this script and not about the library.
    invalid = [row for row in invalid if not verifies(public_from_hex(row[0].hex()), row[1], row[2])]
    for name, rows in (("theirs_valid.txt", valid), ("theirs_invalid.txt", invalid)):
        (d / name).write_text("".join(f"{p.hex()} {g.hex()} {s.hex()}\n" for p, g, s in rows))


def generate_keys(d: pathlib.Path) -> None:
    """Private keys in the formats a person is handed, written by OpenSSL."""
    lines = []
    for i in range(4):
        key = ec.generate_private_key(ec.SECP256R1())
        pub = uncompressed(key.public_key()).hex()
        enc = serialization.Encoding
        fmt = serialization.PrivateFormat
        none = serialization.NoEncryption()
        (d / f"key{i}_pkcs8.pem").write_bytes(key.private_bytes(enc.PEM, fmt.PKCS8, none))
        (d / f"key{i}_pkcs8.der").write_bytes(key.private_bytes(enc.DER, fmt.PKCS8, none))
        (d / f"key{i}_sec1.pem").write_bytes(key.private_bytes(enc.PEM, fmt.TraditionalOpenSSL, none))
        (d / f"key{i}_sec1.der").write_bytes(key.private_bytes(enc.DER, fmt.TraditionalOpenSSL, none))
        (d / f"key{i}_pkcs8_encrypted.pem").write_bytes(key.private_bytes(
            enc.PEM, fmt.PKCS8, serialization.BestAvailableEncryption(b"passphrase")))
        (d / f"key{i}_sec1_encrypted.pem").write_bytes(key.private_bytes(
            enc.PEM, fmt.TraditionalOpenSSL, serialization.BestAvailableEncryption(b"passphrase")))
        (d / f"key{i}_public.pem").write_bytes(key.public_key().public_bytes(
            enc.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
        (d / f"key{i}_public.der").write_bytes(key.public_key().public_bytes(
            enc.DER, serialization.PublicFormat.SubjectPublicKeyInfo))
        lines.append(f"{i} {pub}\n")
    # The command line, which is what the specification's operators will have used: `ecparam
    # -genkey` writes an EC PARAMETERS block before the key unless -noout is given.
    run("openssl", "ecparam", "-name", "prime256v1", "-genkey", "-out", str(d / "cli_ecparam.pem"))
    run("openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(d / "cli_noout.pem"))
    run("openssl", "genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256",
        "-out", str(d / "cli_genpkey.pem"))
    for name in ("cli_ecparam", "cli_noout", "cli_genpkey"):
        loaded = serialization.load_pem_private_key((d / f"{name}.pem").read_bytes(), None)
        lines.append(f"{name} {uncompressed(loaded.public_key()).hex()}\n")
    # Keys on the wrong curve and of the wrong type, which must be refused.
    (d / "wrong_p384.pem").write_bytes(ec.generate_private_key(ec.SECP384R1()).private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    from cryptography.hazmat.primitives.asymmetric import ed25519, rsa
    (d / "wrong_ed25519.pem").write_bytes(ed25519.Ed25519PrivateKey.generate().private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    (d / "wrong_rsa.pem").write_bytes(rsa.generate_private_key(65537, 2048).private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
        serialization.NoEncryption()))
    (d / "keys.txt").write_text("".join(lines))


def name(common_name, title=None, serial_number=None):
    attributes = []
    if serial_number:
        attributes.append(x509.NameAttribute(NameOID.SERIAL_NUMBER, serial_number))
    if title:
        attributes.append(x509.NameAttribute(NameOID.TITLE, title))
    attributes.append(x509.NameAttribute(NameOID.COMMON_NAME, common_name))
    return x509.Name(attributes)


def make_cert(subject, issuer, public_key, signer, serial, not_before, not_after, ca=None,
              path_length=None, key_usage=None, extensions=()):
    builder = (x509.CertificateBuilder().subject_name(subject).issuer_name(issuer)
               .public_key(public_key).serial_number(serial)
               .not_valid_before(not_before).not_valid_after(not_after))
    if ca is not None:
        builder = builder.add_extension(x509.BasicConstraints(ca=ca, path_length=path_length), True)
    if key_usage is not None:
        builder = builder.add_extension(key_usage, True)
    for extension, critical in extensions:
        builder = builder.add_extension(extension, critical)
    return builder.sign(signer, hashes.SHA256())


def ku(**kwargs):
    base = dict(digital_signature=False, content_commitment=False, key_encipherment=False,
                data_encipherment=False, key_agreement=False, key_cert_sign=False, crl_sign=False,
                encipher_only=False, decipher_only=False)
    base.update(kwargs)
    return x509.KeyUsage(**base)


class UnknownCritical(x509.UnrecognizedExtension):
    pass


def pem(cert) -> bytes:
    return cert.public_bytes(serialization.Encoding.PEM)


def scenario(d: pathlib.Path, label, root_not_after=date(2034, 1, 1), mid_not_before=date(2024, 1, 1),
             mid_not_after=date(2034, 1, 1), leaf_not_before=date(2024, 1, 1),
             leaf_not_after=date(2034, 1, 1), root_path_length=None, mid_ca=True,
             mid_key_usage=None, leaf_extensions=(), wrong_root=False, root_key_usage=None):
    """Writes chain_<label>_{root,mid,leaf}.pem and returns OpenSSL's verdict at NOW."""
    root_key = ec.generate_private_key(ec.SECP256R1())
    mid_key = ec.generate_private_key(ec.SECP256R1())
    leaf_key = ec.generate_private_key(ec.SECP256R1())
    root_name = name("Interop Master CA")
    mid_name = name("Interop Signing CA")
    root = make_cert(root_name, root_name, root_key.public_key(), root_key, 1, date(2024, 1, 1),
                     root_not_after, ca=True, path_length=root_path_length,
                     key_usage=root_key_usage or ku(key_cert_sign=True, crl_sign=True))
    mid = make_cert(mid_name, root_name, mid_key.public_key(), root_key, 2, mid_not_before,
                    mid_not_after, ca=mid_ca,
                    key_usage=mid_key_usage or ku(key_cert_sign=True, crl_sign=True))
    leaf = make_cert(name("ILok", "Code/Content Signer", "1798A2B5D62000000005"), mid_name,
                     leaf_key.public_key(), mid_key, LEAF_SERIAL, leaf_not_before, leaf_not_after,
                     key_usage=ku(digital_signature=True), extensions=leaf_extensions)
    anchor = root
    if wrong_root:
        other_key = ec.generate_private_key(ec.SECP256R1())
        anchor = make_cert(root_name, root_name, other_key.public_key(), other_key, 1,
                           date(2024, 1, 1), date(2034, 1, 1), ca=True,
                           key_usage=ku(key_cert_sign=True))
    (d / f"chain_{label}_root.pem").write_bytes(pem(anchor))
    (d / f"chain_{label}_mid.pem").write_bytes(pem(mid))
    (d / f"chain_{label}_leaf.pem").write_bytes(pem(leaf))
    verdict = run("openssl", "verify", "-attime", str(NOW),
                  "-CAfile", str(d / f"chain_{label}_root.pem"),
                  "-untrusted", str(d / f"chain_{label}_mid.pem"),
                  str(d / f"chain_{label}_leaf.pem"), check=False)
    return "accept" if verdict.returncode == 0 else "reject"


def generate_chains(d: pathlib.Path) -> None:
    verdicts = []

    def add(label, **kwargs):
        verdicts.append(f"{label} {scenario(d, label, **kwargs)}\n")

    add("good")
    add("expired_root", root_not_after=date(2025, 11, 13))
    add("expired_mid", mid_not_after=date(2025, 11, 13))
    add("expired_leaf", leaf_not_after=date(2025, 1, 1))
    add("future_mid", mid_not_before=date(2027, 1, 1))
    add("future_leaf", leaf_not_before=date(2027, 1, 1))
    add("wrong_root", wrong_root=True)
    add("mid_not_ca", mid_ca=False)
    add("mid_no_cert_sign", mid_key_usage=ku(digital_signature=True))
    add("root_pathlen_0", root_path_length=0)
    add("root_pathlen_1", root_path_length=1)
    add("unknown_critical_leaf",
        leaf_extensions=[(x509.UnrecognizedExtension(x509.ObjectIdentifier("1.2.3.4.5"), b"\x05\x00"), True)])
    add("unknown_noncritical_leaf",
        leaf_extensions=[(x509.UnrecognizedExtension(x509.ObjectIdentifier("1.2.3.4.5"), b"\x05\x00"), False)])
    (d / "chain_verdicts.txt").write_text("".join(verdicts))

    # The expected fields of the good chain's certificates, read by OpenSSL's parser, for the C++
    # reader to match: the serial, the three name attributes, the validity, the key and the
    # fingerprints in the form the command line prints them.
    lines = []
    for role in ("root", "mid", "leaf"):
        path = d / f"chain_good_{role}.pem"
        cert = x509.load_pem_x509_certificate(path.read_bytes())
        sha1 = run("openssl", "x509", "-in", str(path), "-noout", "-fingerprint", "-sha1").stdout
        sha256 = run("openssl", "x509", "-in", str(path), "-noout", "-fingerprint", "-sha256").stdout
        lines.append(f"{role} serial {serial_hex(cert.serial_number)}\n")
        lines.append(f"{role} not_before {int(cert.not_valid_before_utc.timestamp())}\n")
        lines.append(f"{role} not_after {int(cert.not_valid_after_utc.timestamp())}\n")
        lines.append(f"{role} pub {uncompressed(cert.public_key()).hex()}\n")
        lines.append(f"{role} sha1 {sha1.split('=', 1)[1].strip()}\n")
        lines.append(f"{role} sha256 {sha256.split('=', 1)[1].strip()}\n")
    (d / "chain_good_expected.txt").write_text("".join(lines))

    # A version 1 certificate, which `openssl x509 -req` writes when it is given no extensions.
    key = d / "v1.key"
    run("openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key))
    run("openssl", "req", "-new", "-key", str(key), "-subj", "/CN=Interop V1 Root", "-out", str(d / "v1.csr"))
    run("openssl", "x509", "-req", "-in", str(d / "v1.csr"), "-signkey", str(key), "-days", "3650",
        "-sha256", "-out", str(d / "v1_root.pem"))
    v1 = x509.load_pem_x509_certificate((d / "v1_root.pem").read_bytes())
    if v1.version != x509.Version.v1:
        raise RuntimeError("the openssl in use did not write a version 1 certificate")
    (d / "v1_expected.txt").write_text(f"serial {serial_hex(v1.serial_number)}\npub {uncompressed(v1.public_key()).hex()}\n")


def generate(d: pathlib.Path) -> None:
    generate_signatures(d)
    generate_keys(d)
    generate_chains(d)


# --- verify ------------------------------------------------------------------------------------

def read_rows(path: pathlib.Path):
    return [line.split() for line in path.read_text().splitlines() if line.strip()]


def verify(d: pathlib.Path) -> None:
    # 1. Signatures the library made verify under OpenSSL, and none verifies when it should not.
    rows = read_rows(d / "ours_signatures.txt")
    if len(rows) < 100:
        fail(f"ours_signatures.txt has only {len(rows)} rows")
    for pub, digest, der in rows:
        if not verifies(public_from_hex(pub), bytes.fromhex(digest), bytes.fromhex(der)):
            fail(f"OpenSSL rejects a signature the library made: key {pub[:16]}... digest {digest[:16]}...")
    for pub, digest, der in read_rows(d / "ours_tampered.txt"):
        if verifies(public_from_hex(pub), bytes.fromhex(digest), bytes.fromhex(der)):
            fail("OpenSSL accepts a signature the library test labelled as tampered")

    # 2. Keys the library wrote load in OpenSSL and give the public key it said.
    for kind, pub in read_rows(d / "ours_keys.txt"):
        for suffix, loader in (("pkcs8.pem", "pem"), ("sec1.pem", "pem"), ("pkcs8.der", "der"),
                               ("sec1.der", "der")):
            data = (d / f"ours_key_{kind}_{suffix}").read_bytes()
            try:
                key = (serialization.load_pem_private_key(data, None) if loader == "pem"
                       else serialization.load_der_private_key(data, None))
            except Exception as error:
                fail(f"OpenSSL cannot load ours_key_{kind}_{suffix}: {error}")
                continue
            if uncompressed(key.public_key()).hex() != pub:
                fail(f"ours_key_{kind}_{suffix} has another public key than the library said")
        spki = (d / f"ours_key_{kind}_public.der").read_bytes()
        try:
            loaded = serialization.load_der_public_key(spki)
            if uncompressed(loaded).hex() != pub:
                fail(f"ours_key_{kind}_public.der has another public key than the library said")
        except Exception as error:
            fail(f"OpenSSL cannot load ours_key_{kind}_public.der: {error}")

    # 3. The library's certificates: OpenSSL parses them, reads the fields the library set, finds
    # each signed by its issuer, and `openssl verify` accepts the chain.
    certs = {}
    for role in ("root", "mid", "leaf"):
        path = d / f"ours_{role}.pem"
        try:
            certs[role] = x509.load_pem_x509_certificate(path.read_bytes())
        except Exception as error:
            fail(f"OpenSSL cannot parse {path.name}: {error}")
    if len(certs) == 3:
        leaf = certs["leaf"]
        if leaf.serial_number != LEAF_SERIAL:
            fail(f"leaf serial reads as {leaf.serial_number:X}")
        attributes = {a.oid: a.value for a in leaf.subject}
        if attributes.get(NameOID.COMMON_NAME) != "ILok":
            fail("leaf common name is not ILok")
        if attributes.get(NameOID.TITLE) != "Code/Content Signer":
            fail("leaf title is not Code/Content Signer")
        if attributes.get(NameOID.SERIAL_NUMBER) != "1798A2B5D62000000005":
            fail("leaf subject serialNumber is not 1798A2B5D62000000005")
        try:
            certs["mid"].verify_directly_issued_by(certs["root"])
            certs["leaf"].verify_directly_issued_by(certs["mid"])
            certs["root"].verify_directly_issued_by(certs["root"])
        except Exception as error:
            fail(f"OpenSSL finds a signature of the library's certificates invalid: {error}")
        result = run("openssl", "verify", "-attime", str(NOW), "-CAfile", str(d / "ours_root.pem"),
                     "-untrusted", str(d / "ours_mid.pem"), str(d / "ours_leaf.pem"), check=False)
        if result.returncode != 0:
            fail(f"openssl verify rejects the library's chain: {result.stdout}{result.stderr}")
        # And `openssl x509 -text` can print every one of them (a parse that only some readers get).
        for role in ("root", "mid", "leaf"):
            shown = run("openssl", "x509", "-in", str(d / f"ours_{role}.pem"), "-noout", "-text", check=False)
            if shown.returncode != 0:
                fail(f"openssl x509 -text rejects ours_{role}.pem: {shown.stderr}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("phase", choices=["generate", "verify"])
    parser.add_argument("--dir", required=True, type=pathlib.Path)
    args = parser.parse_args()
    args.dir.mkdir(parents=True, exist_ok=True)
    try:
        (generate if args.phase == "generate" else verify)(args.dir)
    except RuntimeError as error:
        print(f"error: {error}")
        return 1
    if failures:
        print(f"{len(failures)} disagreement(s) with OpenSSL")
        return 1
    print(f"{args.phase}: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
