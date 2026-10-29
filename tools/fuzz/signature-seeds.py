#!/usr/bin/env python3
"""Seeds for the two content-signature harnesses, from files `forge` signs with throwaway keys.

  signature-seeds.py OUT_DIR --forge PATH

writes OUT_DIR/fuzz_content_signature/ and OUT_DIR/fuzz_ac4_content_signature/ (tools/fuzz/
generate-seeds.sh calls it; run.sh merge reduces what it writes). fuzz_content_signature takes a
mode byte first (see the harness): 0 Thrift, 1 DER, 2 text, 3 blob, 4 blob verified,
5 certificates, 6 base64.

Every key and certificate is made here with openssl, for a root that signs nothing real, and is
thrown away with the scratch directory: the keys are synthetic, no private key is written to a
seed (a PEM private key in the tree is what a secret scanner is for), and nothing here reads or is
derived from anyone's signed file. What the seeds vary is what the parsers branch on:

  * the chain a signature carries: leaf, root and an intermediate CA in order, leaf and root, the
    leaf alone, and the root before the leaf;
  * the stream it signs: stereo, mono and 5.1, a signed date, a leaf title other than the default;
  * the certificates as DER and as PEM: a root, an intermediate with a path length, a leaf with and
    without a key usage, with a subject alternative name and an extended key usage, an ECDSA P-384
    key and an RSA key (the verifier takes P-256 only, so they reach the algorithm refusals);
  * the Thrift blob alone, as base64 (the form a secret holds it in), and cut inside its
    certificate list.
"""

import argparse
import base64
import datetime
import math
import pathlib
import struct
import subprocess
import sys
import tempfile
import wave

# The mode bytes of fuzz_content_signature.
THRIFT, DER, TEXT, BLOB, BLOB_VERIFIED, CERTIFICATES, BASE64 = range(7)
LEAF_SUBJECT = "/CN=Seed Content Signer/title=Code\\/Content Signer"


def run(*command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, **kwargs)


def openssl(*args):
    run("openssl", *args)


class Authority:
    """Throwaway keys and certificates in a scratch directory."""

    def __init__(self, directory):
        self.dir = directory

    def path(self, name):
        return self.dir / name

    def key(self, name, algorithm="ec_p256"):
        out = self.path(f"{name}.key")
        if algorithm == "ec_p256":
            openssl("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", out)
        elif algorithm == "ec_p384":
            openssl("ecparam", "-name", "secp384r1", "-genkey", "-noout", "-out", out)
        else:
            openssl("genrsa", "-out", out, "2048")
        return out

    def root(self, name="root", ca=True):
        key = self.key(name)
        extensions = ["-addext", f"basicConstraints=critical,CA:{'TRUE' if ca else 'FALSE'}"]
        extensions += ["-addext", "keyUsage=critical,keyCertSign"]
        openssl(
            "req",
            "-new",
            "-x509",
            "-key",
            key,
            "-sha256",
            "-days",
            "30",
            "-subj",
            f"/CN=Seed {name}",
            *extensions,
            "-out",
            self.path(f"{name}.pem"),
        )
        return name

    def issue(self, name, issuer, subject, extensions, algorithm="ec_p256"):
        key = self.key(name, algorithm)
        csr = self.path(f"{name}.csr")
        openssl("req", "-new", "-key", key, "-sha256", "-subj", subject, "-out", csr)
        extension_file = self.path(f"{name}.ext")
        extension_file.write_text(extensions + "\n", encoding="utf-8")
        openssl(
            "x509",
            "-req",
            "-in",
            csr,
            "-CA",
            self.path(f"{issuer}.pem"),
            "-CAkey",
            self.path(f"{issuer}.key"),
            "-CAcreateserial",
            "-sha256",
            "-days",
            "30",
            "-extfile",
            extension_file,
            "-out",
            self.path(f"{name}.pem"),
        )
        return name

    def pem(self, name):
        return self.path(f"{name}.pem").read_bytes()

    def der(self, name):
        return run("openssl", "x509", "-in", self.path(f"{name}.pem"), "-outform", "DER").stdout

    def chain(self, *names, to):
        pem = b"".join(self.pem(name) for name in names)
        self.path(f"{to}.pem").write_bytes(pem)
        return self.path(f"{to}.pem")


def tone(path, channels, frames=6144):
    with wave.open(str(path), "wb") as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(48000)
        data = bytearray()
        for n in range(frames):
            for c in range(channels):
                value = int(5000 * math.sin(2 * math.pi * (330 + 110 * c) * n / 48000))
                data += struct.pack("<h", value)
        out.writeframes(bytes(data))


def write(directory, name, data):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / name).write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("--forge", required=True)
    args = parser.parse_args()
    forge = args.forge
    base = args.out / "fuzz_content_signature"
    streams = args.out / "fuzz_ac4_content_signature"

    with tempfile.TemporaryDirectory() as scratch:
        ca = Authority(pathlib.Path(scratch))
        ca.root("root")
        ca.issue(
            "inter",
            "root",
            "/CN=Seed Intermediate CA",
            "basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign",
        )
        leaf_usage = "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature"
        ca.issue("leaf", "inter", LEAF_SUBJECT, leaf_usage)
        ca.issue("leaf_direct", "root", LEAF_SUBJECT, leaf_usage)
        ca.issue("leaf_no_usage", "root", LEAF_SUBJECT, "basicConstraints=critical,CA:FALSE")
        ca.issue(
            "leaf_names",
            "root",
            LEAF_SUBJECT,
            leaf_usage + "\nsubjectAltName=DNS:signer.invalid,email:seed@signer.invalid"
            "\nextendedKeyUsage=codeSigning,emailProtection",
        )
        ca.issue("leaf_p384", "root", LEAF_SUBJECT, leaf_usage, algorithm="ec_p384")
        ca.issue("leaf_rsa", "root", LEAF_SUBJECT, leaf_usage, algorithm="rsa")

        # --- streams, plain and signed ---
        plain_streams = {}
        for name, channels in (("stereo", 2), ("mono", 1), ("51", 6)):
            wav = ca.path(f"{name}.wav")
            tone(wav, channels, 6144 if channels < 6 else 3072)
            plain = ca.path(f"{name}.ac4")
            run(forge, "ac4-encode", wav, plain, "64" if channels < 6 else "192")
            plain_streams[name] = plain

        def sign(plain, chain, key, *options):
            signed = ca.path(f"signed-{len(list(ca.dir.glob('signed-*')))}.ac4")
            run(
                forge,
                "ac4-sign",
                plain,
                signed,
                f"content-key={ca.path(key + '.key')}",
                f"content-chain={chain}",
                *options,
            )
            return signed

        chains = {
            "leaf-inter-root": (ca.chain("leaf", "inter", "root", to="c3"), "leaf"),
            "leaf-root": (ca.chain("leaf_direct", "root", to="c2"), "leaf_direct"),
            "leaf-only": (ca.chain("leaf_direct", to="c1"), "leaf_direct"),
            "root-first": (ca.chain("root", "leaf_direct", to="c2r"), "leaf_direct"),
        }
        plain = plain_streams["stereo"]
        plain_size = plain.stat().st_size
        blobs = {}
        for name, (chain, key) in chains.items():
            try:
                signed = sign(plain, chain, key)
            except subprocess.CalledProcessError:
                continue  # an arrangement forge refuses is not a seed
            write(streams, f"ac4-signed-stereo-{name}.ac4", signed.read_bytes())
            blobs[name] = signed.read_bytes()[plain_size:]
        # forge refuses a signature that would not verify (a title other than the leaf's, a date
        # outside the certificates' validity), so the variants are valid ones: a leaf with another
        # title, and a signing date a few days after the keys were made.
        ca.issue("leaf_other", "inter", "/CN=Seed Other Signer/title=Seed Other Title", leaf_usage)
        other_chain = ca.chain("leaf_other", "inter", "root", to="c3o")
        today = datetime.datetime.now(datetime.timezone.utc)
        for name, key, chain, extra in (
            ("title-other", "leaf_other", other_chain, ["leaf-title=Seed Other Title"]),
            (
                "date-in-three-days",
                "leaf",
                chains["leaf-inter-root"][0],
                [
                    "signed-date="
                    + (today + datetime.timedelta(days=3)).strftime("%Y-%m-%dT%H:%M:%S")
                ],
            ),
            (
                "date-in-twenty-days",
                "leaf",
                chains["leaf-inter-root"][0],
                [
                    "signed-date="
                    + (today + datetime.timedelta(days=20)).strftime("%Y-%m-%dT%H:%M:%S")
                ],
            ),
        ):
            signed = sign(plain, chain, key, *extra)
            write(streams, f"ac4-signed-stereo-{name}.ac4", signed.read_bytes())
            blobs[name] = signed.read_bytes()[plain_size:]
        for name in ("mono", "51"):
            chain, key = chains["leaf-root"]
            signed = sign(plain_streams[name], chain, key)
            write(streams, f"ac4-signed-{name}.ac4", signed.read_bytes())
        write(streams, "ac4-unsigned-mono.ac4", plain_streams["mono"].read_bytes())
        write(streams, "ac4-unsigned-51.ac4", plain_streams["51"].read_bytes())
        first = blobs["leaf-inter-root"]
        signed_three = plain.read_bytes() + first
        write(
            streams, "ac4-signed-cut-in-certificates.ac4", signed_three[: len(signed_three) - 900]
        )
        write(streams, "ac4-signed-trailer-twice.ac4", signed_three + first)

        # --- the blob and the certificates, as the first harness reads them ---
        for name, blob in blobs.items():
            write(base, f"thrift-blob-{name}.bin", bytes([THRIFT]) + blob)
            write(base, f"blob-parse-{name}.bin", bytes([BLOB]) + blob)
        write(
            base,
            "blob-verify-three-certificates.bin",
            bytes([BLOB_VERIFIED]) + blobs["leaf-inter-root"],
        )
        write(base, "blob-verify-leaf-only.bin", bytes([BLOB_VERIFIED]) + blobs["leaf-only"])
        write(
            base,
            "blob-base64-three-certificates.bin",
            bytes([BASE64]) + base64.b64encode(blobs["leaf-inter-root"]),
        )
        write(
            base,
            "blob-base64-wrapped.bin",
            bytes([BASE64]) + base64.encodebytes(blobs["leaf-root"]),
        )
        for name in (
            "root",
            "inter",
            "leaf",
            "leaf_no_usage",
            "leaf_names",
            "leaf_p384",
            "leaf_rsa",
        ):
            write(base, f"der-{name.replace('_', '-')}.bin", bytes([DER]) + ca.der(name))
        write(
            base,
            "certificates-der-three.bin",
            bytes([CERTIFICATES]) + ca.der("root") + ca.der("inter") + ca.der("leaf"),
        )
        write(
            base,
            "certificates-der-out-of-order.bin",
            bytes([CERTIFICATES]) + ca.der("leaf") + ca.der("root") + ca.der("inter"),
        )
        write(
            base,
            "certificates-pem-three.bin",
            bytes([CERTIFICATES]) + ca.pem("root") + ca.pem("inter") + ca.pem("leaf"),
        )
        write(
            base,
            "text-pem-chain-three.bin",
            bytes([TEXT]) + ca.pem("leaf") + ca.pem("inter") + ca.pem("root"),
        )
        write(
            base,
            "text-pem-chain-crlf.bin",
            bytes([TEXT]) + (ca.pem("leaf") + ca.pem("root")).replace(b"\n", b"\r\n"),
        )
        write(
            base,
            "text-pem-with-prose.bin",
            bytes([TEXT])
            + b"Subject: seed\nissuer: seed\n"
            + ca.pem("leaf")
            + b"\ntrailing text\n",
        )
        write(base, "text-date.bin", bytes([TEXT]) + b"2026-10-29T12:34:56")
    return 0


if __name__ == "__main__":
    sys.exit(main())
