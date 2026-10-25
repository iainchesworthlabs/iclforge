#!/usr/bin/env python3
"""Sign a firmware update index, and make throwaway keys to try it (planning/esp32-ota.md, O10).

    sign_index.py sign   --index hearth-update-beta.json --out hearth-update-beta.sig.json \
                         --pfx-env FIRMWARE_SIGN_PFX_BASE64 \
                         --password-env FIRMWARE_SIGN_PFX_PASSWORD \
                         --anchors firmware/keys/trust.pem --leaf-name "Hearth Firmware Beta"
    sign_index.py verify --index ... --sig ... --anchors ... [--leaf-name NAME]   (index_verify.py)
    sign_index.py selftest --out DIR [--kind rsa|ecdsa]

`sign` signs the index file's exact bytes with a code-signing leaf and writes the
signature file a board reads: the signature, the leaf and the intermediate (never the
root), and the index's SHA-256. The leaf comes from a PKCS#12 file held in an
environment variable (base64, as a GitHub secret holds it) or in a file, or from a key
file and a certificate. The key is read into memory and never written; nothing it holds
is printed. With `--anchors` the pair is verified, the way a board does, before the file
is written, so a signing that would not be trusted writes nothing.

`selftest` makes a throwaway CA, a sub-CA and a stable and a beta leaf, with a sample
index signed by each, in DIR. The tests use it, and the workflow's dry run signs with it
so that every dry run exercises the whole pipeline without a secret. The keys are
random each time and mean nothing.

Needs `cryptography` (requirements/requirements-firmware-sign.in); `verify` needs
nothing (index_verify.py). A key in the Windows certificate store is signed with by
sign_index.ps1, which writes the same file.
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import index_verify as verifier
from index_verify import VerifyError

WARN_DAYS = 60
LEAF_NAMES = {"stable": "Hearth Firmware Stable", "beta": "Hearth Firmware Beta"}


class SignError(Exception):
    """Why an index was not signed; main() prints it."""


def _crypto() -> Any:
    try:
        import cryptography  # noqa: PLC0415
    except ImportError as error:  # pragma: no cover - only without the package
        raise SignError(
            "sign_index.py needs the 'cryptography' package "
            "(requirements/requirements-firmware-sign.in)"
        ) from error
    return cryptography


def _b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def message(index_bytes: bytes) -> bytes:
    """What is signed: the index file's exact bytes."""
    return index_bytes


def load_pfx(pfx: bytes, password: str | None) -> tuple[Any, Any, list[Any]]:
    _crypto()
    from cryptography.hazmat.primitives.serialization import pkcs12  # noqa: PLC0415

    try:
        key, cert, extra = pkcs12.load_key_and_certificates(
            pfx, password.encode() if password else None
        )
    except ValueError as error:
        raise SignError(
            "the PKCS#12 file cannot be opened (a wrong password, or not a PKCS#12)"
        ) from error
    if key is None or cert is None:
        raise SignError("the PKCS#12 file holds no key and certificate")
    return key, cert, list(extra or [])


def order_chain(leaf: Any, others: list[Any]) -> list[Any]:
    """The leaf, then the certificates that issued it, in order, without a self-signed root."""
    chain = [leaf]
    remaining = list(others)
    while len(chain) < verifier.MAX_CHAIN:
        issuer = next(
            (c for c in remaining if c.subject == chain[-1].issuer and c != chain[-1]), None
        )
        if issuer is None or issuer.subject == issuer.issuer:  # the root is never sent
            break
        chain.append(issuer)
        remaining.remove(issuer)
    return chain


def choose_algorithm(key: Any, requested: str) -> str:
    from cryptography.hazmat.primitives.asymmetric import ec, rsa  # noqa: PLC0415

    if isinstance(key, rsa.RSAPrivateKey):
        if requested in ("auto", "rsa-pkcs1-sha256"):
            return "rsa-pkcs1-sha256"
        if requested == "rsa-pss-sha256":
            return requested
    elif isinstance(key, ec.EllipticCurvePrivateKey) and key.curve.name == "secp256r1":
        if requested in ("auto", "ecdsa-p256-sha256"):
            return "ecdsa-p256-sha256"
    raise SignError(f"the key cannot make a {requested!r} signature (RSA, or ECDSA P-256)")


def raw_signature(key: Any, algorithm: str, data: bytes) -> bytes:
    from cryptography.hazmat.primitives import hashes  # noqa: PLC0415
    from cryptography.hazmat.primitives.asymmetric import ec, padding, utils  # noqa: PLC0415

    if algorithm == "rsa-pkcs1-sha256":
        return key.sign(data, padding.PKCS1v15(), hashes.SHA256())
    if algorithm == "rsa-pss-sha256":
        return key.sign(data, padding.PSS(padding.MGF1(hashes.SHA256()), 32), hashes.SHA256())
    r, s = utils.decode_dss_signature(key.sign(data, ec.ECDSA(hashes.SHA256())))
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")  # raw r||s, as PSA and .NET make it


def signature_document(
    index_bytes: bytes, key: Any, chain: list[Any], requested: str = "auto"
) -> bytes:
    from cryptography.hazmat.primitives.serialization import Encoding  # noqa: PLC0415

    algorithm = choose_algorithm(key, requested)
    document = {
        "format": verifier.SIG_FORMAT,
        "alg": algorithm,
        "sha256": hashlib.sha256(index_bytes).hexdigest(),
        "chain": [_b64(cert.public_bytes(Encoding.DER)) for cert in chain],
        "signature": _b64(raw_signature(key, algorithm, message(index_bytes))),
    }
    return (json.dumps(document, separators=(",", ":")) + "\n").encode("ascii")


def annotate(level: str, text: str) -> None:
    prefix = f"::{level}::" if os.environ.get("GITHUB_ACTIONS") == "true" else f"{level}: "
    print(f"{prefix}{text}", file=sys.stderr)


def not_after_utc(cert: Any) -> dt.datetime:
    """When a certificate ends, as an aware UTC time on every `cryptography` that has x509.

    cryptography 42 added not_valid_after_utc and deprecated not_valid_after, a naive UTC time; a CI
    image may have either.
    """
    aware = getattr(cert, "not_valid_after_utc", None)
    if aware is not None:
        return aware
    return cert.not_valid_after.replace(tzinfo=dt.timezone.utc)


def check_leaf(leaf: Any, key: Any, now: dt.datetime, min_days: int) -> None:
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat  # noqa: PLC0415

    public = key.public_key().public_bytes(Encoding.DER, PublicFormat.SubjectPublicKeyInfo)
    if leaf.public_key().public_bytes(Encoding.DER, PublicFormat.SubjectPublicKeyInfo) != public:
        raise SignError("the certificate is not the key's")
    not_after = not_after_utc(leaf)
    if now > not_after:
        raise SignError(
            f"the signing certificate expired on {not_after:%Y-%m-%d}: issue a new leaf"
        )
    days = (not_after - now).days
    if days < min_days:
        annotate(
            "warning",
            f"the signing certificate ends in {days} days ({not_after:%Y-%m-%d}): renew it",
        )


def sign_file(
    index_path: Path,
    out_path: Path,
    key: Any,
    leaf: Any,
    others: list[Any],
    *,
    algorithm: str = "auto",
    anchors: Path | None = None,
    leaf_name: str | None = None,
    now: dt.datetime | None = None,
    min_days: int = WARN_DAYS,
) -> verifier.VerifyResult | None:
    now = now or dt.datetime.now(dt.timezone.utc)
    index_bytes = index_path.read_bytes()
    try:
        verifier.check_index(index_bytes)
    except VerifyError as error:
        raise SignError(f"the index is not one a board would read: {error.text}") from error
    check_leaf(leaf, key, now, min_days)
    chain = order_chain(leaf, others)
    document = signature_document(index_bytes, key, chain, algorithm)
    result = None
    if anchors is not None:
        try:
            result = verifier.verify_pair(
                index_bytes, document, anchors.read_text("utf-8"), leaf_name=leaf_name, now=now
            )
        except VerifyError as error:
            raise SignError(
                f"the result would not be trusted by a board, so nothing was written: {error}"
            ) from error
    else:
        annotate("warning", "no --anchors: the signature was not checked against a trust anchor")
    out_path.write_bytes(document)
    return result


# --- throwaway keys ------------------------------------------------------------------------------


def _name(common_name: str) -> Any:
    from cryptography import x509  # noqa: PLC0415
    from cryptography.x509.oid import NameOID  # noqa: PLC0415

    return x509.Name(
        [
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Throwaway Test Labs"),
            x509.NameAttribute(NameOID.COMMON_NAME, common_name),
        ]
    )


def new_key(kind: str, bits: int = 2048) -> Any:
    from cryptography.hazmat.primitives.asymmetric import ec, rsa  # noqa: PLC0415

    if kind == "ecdsa":
        return ec.generate_private_key(ec.SECP256R1())
    return rsa.generate_private_key(public_exponent=65537, key_size=bits)


def issue(
    subject: str,
    key: Any,
    issuer_name: Any,
    issuer_key: Any,
    *,
    ca: bool,
    days: int,
    now: dt.datetime,
    path_len: int | None = None,
    code_signing: bool = False,
    key_usage_override: Any = None,
) -> Any:
    from cryptography import x509  # noqa: PLC0415
    from cryptography.hazmat.primitives import hashes  # noqa: PLC0415
    from cryptography.x509.oid import ExtendedKeyUsageOID  # noqa: PLC0415

    builder = (
        x509.CertificateBuilder()
        .subject_name(_name(subject))
        .issuer_name(issuer_name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - dt.timedelta(minutes=5))
        .not_valid_after(now + dt.timedelta(days=days))
        .add_extension(
            x509.BasicConstraints(ca=ca, path_length=path_len if ca else None), critical=True
        )
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
    )
    if ca:
        usage = x509.KeyUsage(False, False, False, False, False, True, True, False, False)
    else:
        usage = key_usage_override or x509.KeyUsage(
            True, False, False, False, False, False, False, False, False
        )
    builder = builder.add_extension(usage, critical=True)
    if code_signing:
        builder = builder.add_extension(
            x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CODE_SIGNING]), critical=True
        )
    return builder.sign(issuer_key, hashes.SHA256())


class ThrowawayPki:
    """A root, a sub-CA under it (path length 0) and a code-signing leaf for each channel."""

    def __init__(
        self,
        kind: str = "rsa",
        bits: int = 2048,
        now: dt.datetime | None = None,
        leaf_kinds: dict[str, str] | None = None,
    ) -> None:
        _crypto()
        self.kind = kind
        now = now or dt.datetime.now(dt.timezone.utc)
        self.now = now
        self.root_key = new_key(kind, bits)
        self.root = issue(
            "Throwaway Root",
            self.root_key,
            _name("Throwaway Root"),
            self.root_key,
            ca=True,
            days=3650,
            now=now,
        )
        self.sub_key = new_key(kind, bits)
        self.sub = issue(
            "Throwaway Firmware CA",
            self.sub_key,
            self.root.subject,
            self.root_key,
            ca=True,
            days=3650,
            now=now,
            path_len=0,
        )
        self.leaves: dict[str, tuple[Any, Any]] = {}
        for channel, name in LEAF_NAMES.items():
            leaf_key = new_key((leaf_kinds or {}).get(channel, kind), bits)
            leaf = issue(
                name,
                leaf_key,
                self.sub.subject,
                self.sub_key,
                ca=False,
                days=730,
                now=now,
                code_signing=True,
            )
            self.leaves[channel] = (leaf_key, leaf)

    def anchor_pem(self) -> str:
        from cryptography.hazmat.primitives.serialization import Encoding  # noqa: PLC0415

        return self.sub.public_bytes(Encoding.PEM).decode("ascii")

    def root_pem(self) -> str:
        from cryptography.hazmat.primitives.serialization import Encoding  # noqa: PLC0415

        return self.root.public_bytes(Encoding.PEM).decode("ascii")

    def pfx(self, channel: str, password: str) -> bytes:
        from cryptography.hazmat.primitives.serialization import (  # noqa: PLC0415
            BestAvailableEncryption,
            pkcs12,
        )

        key, leaf = self.leaves[channel]
        return pkcs12.serialize_key_and_certificates(
            channel.encode(),
            key,
            leaf,
            [self.sub, self.root],
            BestAvailableEncryption(password.encode()),
        )


def sample_index(channel: str, sequence: int = 1793000000) -> bytes:
    document = {
        "format": 1,
        "channel": channel,
        "sequence": sequence,
        "version": "v0.11.0-beta.1",
        "released": "2026-11-05",
        "images": [
            {
                "name": "hearth-sink-esp32s3",
                "file": "hearth-sink-esp32s3-v0.11.0-beta.1.bin",
                "size": 1583104,
                "sha256": hashlib.sha256(b"sample image").hexdigest(),
                "project": "iclforge_hearth_sink",
                "chip_id": 9,
                "min_rev_full": 0,
                "max_rev_full": 99,
                "flash_size": "16MB",
                "psram": True,
                "secure_version": 0,
            }
        ],
    }
    return (json.dumps(document, separators=(",", ":")) + "\n").encode("ascii")


def selftest(out: Path, kind: str = "rsa", bits: int = 2048, mixed: bool = False) -> None:
    out.mkdir(parents=True, exist_ok=True)
    # mixed: an RSA CA and beta leaf and an ECDSA stable leaf, so that one board test uses both.
    pki = ThrowawayPki(kind, bits, leaf_kinds={"stable": "ecdsa", "beta": "rsa"} if mixed else None)
    (out / "anchor.pem").write_text(pki.anchor_pem(), "ascii")
    (out / "root.pem").write_text(pki.root_pem(), "ascii")
    for channel in ("stable", "beta"):
        index = out / f"hearth-update-{channel}.json"
        index.write_bytes(sample_index(channel))
        key, leaf = pki.leaves[channel]
        sign_file(
            index,
            out / f"hearth-update-{channel}.sig.json",
            key,
            leaf,
            [pki.sub, pki.root],
            anchors=out / "anchor.pem",
            leaf_name=LEAF_NAMES[channel],
        )
        (out / f"{channel}.pfx").write_bytes(pki.pfx(channel, "selftest"))
    print(
        f"selftest: a throwaway {kind} CA and two signed indexes are in {out} (password 'selftest')"
    )


# --- command line --------------------------------------------------------------------------------


def _leaf_from_args(args: argparse.Namespace) -> tuple[Any, Any, list[Any]]:
    _crypto()
    from cryptography import x509  # noqa: PLC0415
    from cryptography.hazmat.primitives.serialization import load_pem_private_key  # noqa: PLC0415

    password = os.environ.get(args.password_env) if args.password_env else None
    if args.pfx_env:
        text = os.environ.get(args.pfx_env, "")
        if not text:
            raise SignError(f"the environment variable {args.pfx_env} is empty: is the secret set?")
        if args.password_env and not password:
            raise SignError(
                f"the environment variable {args.password_env} is empty: is the secret set?"
            )
        return load_pfx(base64.b64decode(text), password)
    if args.pfx:
        return load_pfx(args.pfx.read_bytes(), password)
    if args.key and args.cert:
        key = load_pem_private_key(args.key.read_bytes(), password.encode() if password else None)
        certs = x509.load_pem_x509_certificates(args.cert.read_bytes())
        extra = x509.load_pem_x509_certificates(args.chain.read_bytes()) if args.chain else []
        return key, certs[0], certs[1:] + extra
    raise SignError("give --pfx-env, --pfx, or --key with --cert")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    sign = commands.add_parser("sign", help="sign an index")
    sign.add_argument("--index", type=Path, required=True)
    sign.add_argument("--out", type=Path, required=True)
    sign.add_argument("--pfx-env", help="environment variable holding the base64 of a PKCS#12 file")
    sign.add_argument("--pfx", type=Path, help="a PKCS#12 file")
    sign.add_argument("--key", type=Path, help="a PEM private key (with --cert)")
    sign.add_argument("--cert", type=Path)
    sign.add_argument("--chain", type=Path, help="PEM intermediates, with --key")
    sign.add_argument(
        "--password-env", help="environment variable holding the PKCS#12 or key password"
    )
    sign.add_argument("--alg", default="auto", choices=("auto", *verifier.SIGNATURE_ALGS))
    sign.add_argument(
        "--anchors", type=Path, help="check the result against these anchors before writing"
    )
    sign.add_argument("--leaf-name", help="the common name the leaf must have")
    sign.add_argument("--min-days", type=int, default=WARN_DAYS)
    sign.add_argument("--now", help="the time to judge dates at (ISO 8601), for tests")
    check = commands.add_parser("verify", help="verify a signed index (index_verify.py)")
    check.add_argument("rest", nargs=argparse.REMAINDER)
    test = commands.add_parser("selftest", help="make throwaway keys and signed indexes")
    test.add_argument("--out", type=Path, required=True)
    test.add_argument("--kind", choices=("rsa", "ecdsa"), default="rsa")
    test.add_argument("--bits", type=int, default=2048)
    test.add_argument(
        "--mixed", action="store_true", help="an RSA CA, an RSA beta leaf and an ECDSA stable leaf"
    )
    args = parser.parse_args(argv)
    try:
        if args.command == "verify":
            return verifier.main(args.rest)
        if args.command == "selftest":
            selftest(args.out, args.kind, args.bits, args.mixed)
            return 0
        key, leaf, others = _leaf_from_args(args)
        now = verifier.parse_time(args.now) if args.now else None
        result = sign_file(
            args.index,
            args.out,
            key,
            leaf,
            others,
            algorithm=args.alg,
            anchors=args.anchors,
            leaf_name=args.leaf_name,
            now=now,
            min_days=args.min_days,
        )
        name = result.leaf_name if result else leaf.subject.rfc4514_string()
        print(f"sign_index: wrote {args.out} (signed by {name!r})")
    except (SignError, OSError) as error:
        print(f"sign_index: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
