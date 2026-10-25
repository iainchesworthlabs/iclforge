"""Unit tests for sign_index.py, which signs a firmware update index (planning/esp32-ota.md, O10).

They make fresh throwaway keys with the `cryptography` package and check what was signed
with index_verify.py, which verifies with its own arithmetic and nothing installed, so the
two implementations disagree here or they agree. Where `cryptography` is not installed (the
system python3 of the "Oracle unit tests" step) the suite skips; the signing job of the
firmware-channels workflow installs it, and test_index_verify.py, which needs nothing, runs
everywhere against fixtures this tool made.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import base64
import contextlib
import datetime as dt
import io
import json
import os
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import index_verify as v
import sign_index as s  # imports cryptography only when it signs

try:
    import cryptography  # noqa: F401

    HAVE_CRYPTO = True
except ImportError:  # pragma: no cover
    HAVE_CRYPTO = False


class NotAfter(unittest.TestCase):
    """The end of a leaf on a cryptography before and after 42 (CI's image may have either)."""

    def test_an_older_cryptography_gives_a_naive_utc_time(self) -> None:
        end = dt.datetime(2028, 10, 25, 1, 2, 3)

        class Old:
            not_valid_after = end

        got = s.not_after_utc(Old())
        self.assertEqual(got, end.replace(tzinfo=dt.timezone.utc))
        self.assertIsNotNone(got.utcoffset())

    def test_a_newer_one_is_used_as_it_is(self) -> None:
        end = dt.datetime(2028, 10, 25, tzinfo=dt.timezone.utc)

        class New:
            not_valid_after_utc = end
            not_valid_after = dt.datetime(2000, 1, 1)  # the deprecated one is not read

        self.assertIs(s.not_after_utc(New()), end)


@unittest.skipUnless(HAVE_CRYPTO, "the cryptography package is not installed")
class Signing(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.rsa = s.ThrowawayPki("rsa", 2048)
        cls.ec = s.ThrowawayPki("ecdsa")

    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.index = self.root / "hearth-update-beta.json"
        self.index.write_bytes(s.sample_index("beta"))
        self.out = self.root / "hearth-update-beta.sig.json"

    def tearDown(self) -> None:
        self.dir.cleanup()

    def anchors(self, pki) -> Path:
        path = self.root / f"anchors-{pki.kind}.pem"
        path.write_text(pki.anchor_pem(), "ascii")
        return path

    def sign(self, pki, channel: str = "beta", **kwargs):
        key, leaf = pki.leaves[channel]
        kwargs.setdefault("anchors", self.anchors(pki))
        kwargs.setdefault("leaf_name", s.LEAF_NAMES[channel])
        return s.sign_file(self.index, self.out, key, leaf, [pki.sub, pki.root], **kwargs)

    def verified(self, pki, **kwargs):
        return v.verify_pair(
            self.index.read_bytes(),
            self.out.read_bytes(),
            pki.anchor_pem(),
            now=dt.datetime.now(dt.timezone.utc),
            **kwargs,
        )

    def test_an_rsa_leaf_signs_and_the_other_implementation_agrees(self) -> None:
        self.sign(self.rsa)
        result = self.verified(self.rsa, leaf_name="Hearth Firmware Beta", channel="beta")
        self.assertEqual(result.algorithm, "rsa-pkcs1-sha256")
        self.assertEqual(result.chain_length, 2)

    def test_pss_is_available_for_an_rsa_key(self) -> None:
        self.sign(self.rsa, algorithm="rsa-pss-sha256")
        self.assertEqual(self.verified(self.rsa).algorithm, "rsa-pss-sha256")

    def test_an_ecdsa_leaf_signs_as_raw_r_and_s(self) -> None:
        self.sign(self.ec)
        document = json.loads(self.out.read_text())
        self.assertEqual(document["alg"], "ecdsa-p256-sha256")
        self.assertEqual(len(base64.b64decode(document["signature"])), 64)
        self.assertEqual(self.verified(self.ec).algorithm, "ecdsa-p256-sha256")

    def test_the_root_is_never_sent(self) -> None:
        self.sign(self.rsa)
        self.assertEqual(len(json.loads(self.out.read_text())["chain"]), 2)

    def test_the_file_is_the_documented_shape_and_small(self) -> None:
        self.sign(self.rsa)
        document = json.loads(self.out.read_text())
        self.assertEqual(sorted(document), ["alg", "chain", "format", "sha256", "signature"])
        self.assertLess(self.out.stat().st_size, v.MAX_SIG_BYTES)

    def test_a_signing_that_would_not_be_trusted_writes_nothing(self) -> None:
        with self.assertRaises(s.SignError):
            self.sign(self.rsa, anchors=self.anchors(self.ec))
        self.assertFalse(self.out.exists())
        with self.assertRaises(s.SignError):
            self.sign(self.rsa, leaf_name="Hearth Firmware Stable")
        self.assertFalse(self.out.exists())

    def test_an_index_a_board_would_not_read_is_not_signed(self) -> None:
        self.index.write_bytes(b'{"format": 2}')
        with self.assertRaises(s.SignError):
            self.sign(self.rsa)
        self.assertFalse(self.out.exists())

    def test_a_key_that_is_not_the_certificates_is_refused(self) -> None:
        stable_key, _ = self.rsa.leaves["stable"]
        _, beta_leaf = self.rsa.leaves["beta"]
        with self.assertRaises(s.SignError):
            s.sign_file(
                self.index,
                self.out,
                stable_key,
                beta_leaf,
                [self.rsa.sub],
                anchors=self.anchors(self.rsa),
            )

    def test_an_expired_leaf_is_refused(self) -> None:
        old = s.ThrowawayPki("rsa", 2048, now=dt.datetime(2020, 1, 1, tzinfo=dt.timezone.utc))
        with self.assertRaises(s.SignError) as caught:
            self.sign(old)
        self.assertIn("expired", str(caught.exception))

    def test_a_leaf_with_little_life_left_is_signed_with_a_warning(self) -> None:
        now = dt.datetime.now(dt.timezone.utc)
        key = s.new_key("rsa", 2048)
        short = s.issue(
            "Hearth Firmware Beta",
            key,
            self.rsa.sub.subject,
            self.rsa.sub_key,
            ca=False,
            days=20,
            now=now,
            code_signing=True,
        )
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            s.sign_file(
                self.index,
                self.out,
                key,
                short,
                [self.rsa.sub],
                anchors=self.anchors(self.rsa),
                leaf_name="Hearth Firmware Beta",
            )
        self.assertRegex(err.getvalue(), r"ends in 1[89] days")
        self.assertTrue(self.out.exists())

    def test_pkcs12_round_trip_and_a_wrong_password(self) -> None:
        data = self.rsa.pfx("beta", "pw")
        _key, leaf, extra = s.load_pfx(data, "pw")
        self.assertEqual(leaf.subject, self.rsa.leaves["beta"][1].subject)
        self.assertEqual(len(extra), 2)
        with self.assertRaises(s.SignError) as caught:
            s.load_pfx(data, "not-the-password")
        self.assertNotIn("not-the-password", str(caught.exception))

    def test_the_command_line_reads_the_leaf_from_secrets_in_the_environment(self) -> None:
        env = {
            "T_PFX": base64.b64encode(self.rsa.pfx("beta", "s3cret-pass-1")).decode(),
            "T_PW": "s3cret-pass-1",
        }
        with unittest.mock.patch.dict(os.environ, env):
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = s.main(
                    [
                        "sign",
                        "--index",
                        str(self.index),
                        "--out",
                        str(self.out),
                        "--pfx-env",
                        "T_PFX",
                        "--password-env",
                        "T_PW",
                        "--anchors",
                        str(self.anchors(self.rsa)),
                        "--leaf-name",
                        "Hearth Firmware Beta",
                    ]
                )
        self.assertEqual(code, 0, err.getvalue())
        self.assertIn("Hearth Firmware Beta", out.getvalue())
        self.assertNotIn("s3cret-pass-1", out.getvalue() + err.getvalue())
        self.verified(self.rsa)

    def test_an_empty_secret_is_named_and_nothing_is_written(self) -> None:
        with unittest.mock.patch.dict(os.environ, {"T_PFX": "", "T_PW": "x"}):
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code = s.main(
                    [
                        "sign",
                        "--index",
                        str(self.index),
                        "--out",
                        str(self.out),
                        "--pfx-env",
                        "T_PFX",
                        "--password-env",
                        "T_PW",
                    ]
                )
        self.assertEqual(code, 1)
        self.assertIn("T_PFX", err.getvalue())
        self.assertFalse(self.out.exists())

    def test_selftest_makes_a_pair_for_each_channel_that_verifies(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(
                s.main(["selftest", "--out", str(self.root / "t"), "--kind", "ecdsa"]), 0
            )
        for channel in ("stable", "beta"):
            v.verify_pair(
                (self.root / "t" / f"hearth-update-{channel}.json").read_bytes(),
                (self.root / "t" / f"hearth-update-{channel}.sig.json").read_bytes(),
                (self.root / "t" / "anchor.pem").read_text(),
                leaf_name=s.LEAF_NAMES[channel],
                now=dt.datetime.now(dt.timezone.utc),
            )


@unittest.skipUnless(HAVE_CRYPTO, "the cryptography package is not installed")
class LeavesThatAreNotForCodeSigning(unittest.TestCase):
    """A certificate from the same CA that is not a firmware key must not sign firmware."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.pki = s.ThrowawayPki("rsa", 2048)

    def attempt(self, **make) -> str:
        """Sign with a leaf made as `make` says and return the code a verifier refuses it with."""
        now = dt.datetime.now(dt.timezone.utc)
        key = s.new_key("rsa", make.pop("bits", 2048))
        eku = make.pop("eku", None)
        ca = make.pop("ca", False)
        if eku is not None:
            leaf = self.reissue_with_eku(key, eku, now)
        else:
            leaf = s.issue(
                "Hearth Firmware Beta",
                key,
                self.pki.sub.subject,
                self.pki.sub_key,
                ca=ca,
                days=365,
                now=now,
                path_len=0 if ca else None,
                **make,
            )
        with tempfile.TemporaryDirectory() as tmp:
            index = Path(tmp) / "i.json"
            index.write_bytes(s.sample_index("beta"))
            out = Path(tmp) / "o.json"
            with contextlib.redirect_stderr(io.StringIO()):  # the "no --anchors" warning
                s.sign_file(index, out, key, leaf, [self.pki.sub])
            with self.assertRaises(v.VerifyError) as caught:
                v.verify_pair(index.read_bytes(), out.read_bytes(), self.pki.anchor_pem(), now=now)
            return caught.exception.code

    def reissue_with_eku(self, key, oids, now):
        from cryptography import x509  # noqa: PLC0415
        from cryptography.hazmat.primitives import hashes  # noqa: PLC0415

        builder = (
            x509.CertificateBuilder()
            .subject_name(s._name("Hearth Firmware Beta"))
            .issuer_name(self.pki.sub.subject)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - dt.timedelta(minutes=5))
            .not_valid_after(now + dt.timedelta(days=30))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(
                x509.KeyUsage(True, False, False, False, False, False, False, False, False),
                critical=True,
            )
            .add_extension(
                x509.ExtendedKeyUsage([x509.ObjectIdentifier(o) for o in oids]), critical=False
            )
        )
        return builder.sign(self.pki.sub_key, hashes.SHA256())

    def test_a_ca_certificate_cannot_sign(self) -> None:
        self.assertEqual(self.attempt(ca=True), "leaf_usage")

    def test_a_certificate_with_no_code_signing_usage_cannot_sign(self) -> None:
        self.assertEqual(self.attempt(code_signing=False), "leaf_usage")

    def test_a_server_authentication_certificate_cannot_sign(self) -> None:
        self.assertEqual(self.attempt(eku=["1.3.6.1.5.5.7.3.1"]), "leaf_usage")

    def test_a_certificate_without_the_digital_signature_usage_cannot_sign(self) -> None:
        from cryptography import x509  # noqa: PLC0415

        usage = x509.KeyUsage(False, False, True, False, False, False, False, False, False)
        self.assertEqual(self.attempt(code_signing=True, key_usage_override=usage), "leaf_usage")

    def test_a_weak_rsa_key_is_refused(self) -> None:
        self.assertEqual(self.attempt(code_signing=True, bits=1024), "weak_key")


if __name__ == "__main__":
    unittest.main()
