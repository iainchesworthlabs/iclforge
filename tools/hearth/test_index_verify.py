"""Unit tests for index_verify.py, the check a board makes of a signed firmware update index
(planning/esp32-ota.md, O10).

stdlib unittest, as the "Oracle unit tests" step of _static.yml runs it, with nothing
installed. The signed pairs under testdata/update_index/ were made once by
`sign_index.py selftest` (a throwaway CA, a sub-CA under it, and a stable and a beta leaf,
RSA 2048 and ECDSA P-256) and are fixtures here: the signing used the `cryptography`
package and this module verifies them with its own arithmetic, so the two implementations
agree or a test fails. test_sign_index.py makes fresh keys when `cryptography` is there.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import base64
import contextlib
import datetime as dt
import hashlib
import io
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import index_verify as v

DATA = Path(__file__).resolve().parent / "testdata" / "update_index"
INSIDE = dt.datetime(2027, 6, 1, tzinfo=dt.timezone.utc)
AFTER = dt.datetime(2029, 1, 1, tzinfo=dt.timezone.utc)
BEFORE = dt.datetime(2026, 10, 1, tzinfo=dt.timezone.utc)

# The same cases are in libs/device/tests/test_firmware_update.cpp: the version rule is one
# rule in two languages. (a, b, order of a against b)
VERSION_CASES = [
    ("v0.10.0-beta.1", "v0.11.0-beta.1", -1),
    ("v0.10.0-beta.1-2039-gf5aded471", "v0.11.0-beta.1", -1),
    ("v0.10.0-beta.1", "v0.10.0-beta.1-2039-gf5aded471", -1),
    ("v0.10.0-beta.1-2039-gf5aded471", "v0.10.0-beta.1", 1),
    ("v0.10.0-beta.2", "v0.10.0-beta.1-2039-gabc", 1),
    ("v0.10.0-beta.1", "v0.10.0", -1),
    ("v0.10.0-rc.1", "v0.10.0-beta.9", 1),
    ("v0.10.0-alpha.3", "v0.10.0-beta.1", -1),
    ("0.10.0", "v0.10.0", 0),
    ("v0.9.9", "v0.10.0-alpha.1", -1),
    ("v1.0.0", "v0.99.99", 1),
    ("v0.10.0-beta.1-5-gabc-dirty", "v0.10.0-beta.1-5-gdef", 0),
]
NOT_VERSIONS = [
    "",
    "1",
    "fa3b9c2",
    "v1.2",
    "v1.2.3.4",
    "main",
    "v1.2.3-beta",
    "v1.2.3-gamma.1",
    "v1.2.3" + chr(10),
    "v1.2.3 ",
    "v1.2.3-5",
    "v1.2.3-5-x",
    "v1234567890.0.0",
    "v1.2.3-dirty-dirty",
]


def load(kind: str, name: str) -> bytes:
    return (DATA / kind / name).read_bytes()


def anchors(kind: str, which: str = "anchor.pem") -> str:
    return (DATA / kind / which).read_text("ascii")


def verify(kind: str = "rsa", channel: str = "beta", sig: str | None = None, **kwargs):
    kwargs.setdefault("now", INSIDE)
    return v.verify_pair(
        kwargs.pop("index", load(kind, f"hearth-update-{channel}.json")),
        kwargs.pop("sig_bytes", load(kind, sig or f"hearth-update-{channel}.sig.json")),
        kwargs.pop("anchors_pem", anchors(kind)),
        channel=kwargs.pop("expect", None),
        **kwargs,
    )


def edited_sig(kind: str, channel: str, change) -> bytes:
    doc = json.loads(load(kind, f"hearth-update-{channel}.sig.json"))
    change(doc)
    return json.dumps(doc).encode()


class Good(unittest.TestCase):
    def test_an_rsa_pair_verifies_with_the_pkcs1_signature(self) -> None:
        result = verify("rsa")
        self.assertEqual(result.algorithm, "rsa-pkcs1-sha256")
        self.assertEqual(result.leaf_name, "Hearth Firmware Beta")
        self.assertEqual(result.chain_length, 2)
        self.assertEqual(result.index["channel"], "beta")

    def test_the_pss_signature_verifies_too(self) -> None:
        self.assertEqual(
            verify("rsa", sig="hearth-update-beta.pss.sig.json").algorithm, "rsa-pss-sha256"
        )

    def test_an_ecdsa_pair_verifies(self) -> None:
        result = verify("ec", "stable")
        self.assertEqual(result.algorithm, "ecdsa-p256-sha256")
        self.assertEqual(result.leaf_name, "Hearth Firmware Stable")

    def test_the_name_pin_and_the_channel_are_held(self) -> None:
        verify("rsa", leaf_name="Hearth Firmware Beta", expect="beta")
        verify("ec", "stable", leaf_name="Hearth Firmware Stable", expect="stable")
        with self.assertRaises(v.VerifyError) as caught:
            verify("rsa", expect="stable")
        self.assertEqual(caught.exception.code, "bad_index")

    def test_a_build_with_no_clock_judges_no_dates(self) -> None:
        verify("rsa", now=None)
        verify("ec", "stable", now=None)

    def test_the_root_can_be_the_anchor_when_the_sub_ca_is_in_the_chain(self) -> None:
        self.assertEqual(verify("rsa", anchors_pem=anchors("rsa", "root.pem")).chain_length, 2)

    def test_the_command_line_says_who_signed(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = v.main(
                [
                    "--index",
                    str(DATA / "rsa" / "hearth-update-beta.json"),
                    "--sig",
                    str(DATA / "rsa" / "hearth-update-beta.sig.json"),
                    "--anchors",
                    str(DATA / "rsa" / "anchor.pem"),
                    "--leaf-name",
                    "Hearth Firmware Beta",
                    "--now",
                    "2027-06-01T00:00:00Z",
                ]
            )
        self.assertEqual(code, 0)
        self.assertIn("signed by 'Hearth Firmware Beta'", out.getvalue())

    def test_the_command_line_refuses_with_the_reason_and_exit_1(self) -> None:
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = v.main(
                [
                    "--index",
                    str(DATA / "rsa" / "hearth-update-beta.json"),
                    "--sig",
                    str(DATA / "rsa" / "hearth-update-stable.sig.json"),
                    "--anchors",
                    str(DATA / "rsa" / "anchor.pem"),
                    "--no-clock",
                ]
            )
        self.assertEqual(code, 1)
        self.assertIn("hash_mismatch", err.getvalue())


class Refusals(unittest.TestCase):
    def assertRefused(self, code: str, **kwargs) -> None:
        with self.assertRaises(v.VerifyError) as caught:
            verify(**kwargs)
        self.assertEqual(caught.exception.code, code, caught.exception)

    def test_an_index_that_was_changed_is_not_the_one_signed(self) -> None:
        index = load("rsa", "hearth-update-beta.json").replace(b"v0.11.0-beta.1", b"v0.12.0-beta.1")
        self.assertRefused("hash_mismatch", index=index)

    def test_a_changed_index_with_the_hash_field_fixed_up_still_fails_the_signature(self) -> None:
        index = load("rsa", "hearth-update-beta.json").replace(b"v0.11.0-beta.1", b"v0.12.0-beta.1")
        sig = edited_sig(
            "rsa", "beta", lambda d: d.update(sha256=hashlib.sha256(index).hexdigest())
        )
        self.assertRefused("bad_signature", index=index, sig_bytes=sig)

    def test_the_other_channels_signature_is_not_this_ones(self) -> None:
        self.assertRefused("hash_mismatch", sig="hearth-update-stable.sig.json")

    def test_a_flipped_signature_bit_fails(self) -> None:
        def flip(doc: dict) -> None:
            raw = bytearray(base64.b64decode(doc["signature"]))
            raw[10] ^= 1
            doc["signature"] = base64.b64encode(bytes(raw)).decode()

        for kind, channel in (("rsa", "beta"), ("ec", "stable")):
            with self.subTest(kind=kind), self.assertRaises(v.VerifyError) as caught:
                verify(kind, channel, sig_bytes=edited_sig(kind, channel, flip))
            self.assertEqual(caught.exception.code, "bad_signature")

    def test_the_stable_leaf_cannot_sign_for_the_beta_pin(self) -> None:
        index = load("rsa", "hearth-update-stable.json")
        sig = load("rsa", "hearth-update-stable.sig.json")
        self.assertRefused(
            "leaf_name", index=index, sig_bytes=sig, leaf_name="Hearth Firmware Beta"
        )

    def test_another_cas_anchor_does_not_trust_it(self) -> None:
        self.assertRefused("untrusted", anchors_pem=anchors("ec"))
        self.assertRefused("untrusted", kind="ec", channel="stable", anchors_pem=anchors("rsa"))

    def test_the_root_as_anchor_does_not_trust_a_leaf_sent_without_its_sub_ca(self) -> None:
        sig = edited_sig("rsa", "beta", lambda d: d.update(chain=d["chain"][:1]))
        self.assertRefused("untrusted", sig_bytes=sig, anchors_pem=anchors("rsa", "root.pem"))

    def test_the_sub_ca_alone_in_the_chain_cannot_sign_the_index(self) -> None:
        sig = edited_sig("rsa", "beta", lambda d: d.update(chain=d["chain"][1:]))
        with self.assertRaises(v.VerifyError) as caught:
            verify(sig_bytes=sig)
        self.assertIn(caught.exception.code, ("leaf_usage", "bad_signature"))

    def test_dates_are_judged_when_there_is_a_clock(self) -> None:
        self.assertRefused("cert_expired", now=AFTER)
        self.assertRefused("cert_not_yet_valid", now=BEFORE)

    def test_the_anchors_own_dates_are_not_judged(self) -> None:
        # The sub-CA is good for ten years, the leaf for two: a clock at year 5 refuses the leaf,
        # not the anchor. A pinned anchor that had expired would still anchor a leaf that had not.
        far = dt.datetime(2032, 1, 1, tzinfo=dt.timezone.utc)
        with self.assertRaises(v.VerifyError) as caught:
            verify(now=far)
        self.assertEqual(caught.exception.code, "cert_expired")


class SignatureFile(unittest.TestCase):
    def assertBad(self, code: str, sig: bytes) -> None:
        with self.assertRaises(v.VerifyError) as caught:
            verify(sig_bytes=sig)
        self.assertEqual(caught.exception.code, code)

    def test_a_malformed_file_is_refused_not_crashed_on(self) -> None:
        self.assertBad("bad_sig_file", b"not json")
        self.assertBad("bad_sig_file", b"[]")
        self.assertBad("bad_sig_file", b"\xff\xfe")
        self.assertBad("bad_sig_file", b"x" * (v.MAX_SIG_BYTES + 1))
        self.assertBad("bad_sig_file", edited_sig("rsa", "beta", lambda d: d.update(format=2)))
        self.assertBad("bad_sig_file", edited_sig("rsa", "beta", lambda d: d.update(alg="md5")))
        self.assertBad("bad_sig_file", edited_sig("rsa", "beta", lambda d: d.update(chain=[])))
        self.assertBad(
            "bad_sig_file", edited_sig("rsa", "beta", lambda d: d.update(chain=d["chain"] * 3))
        )
        self.assertBad(
            "bad_sig_file", edited_sig("rsa", "beta", lambda d: d.update(signature="***"))
        )
        self.assertBad("bad_sig_file", edited_sig("rsa", "beta", lambda d: d.update(chain=[5])))

    def test_a_chain_member_that_is_not_a_certificate_is_untrusted(self) -> None:
        junk = base64.b64encode(b"\x30\x03\x02\x01\x01").decode()
        with self.assertRaises(v.VerifyError):
            verify(sig_bytes=edited_sig("rsa", "beta", lambda d: d.update(chain=[junk])))

    def test_a_missing_hash_field_is_a_mismatch(self) -> None:
        self.assertBad("hash_mismatch", edited_sig("rsa", "beta", lambda d: d.pop("sha256")))

    def test_a_pem_with_no_certificate_is_refused(self) -> None:
        with self.assertRaises(v.VerifyError):
            verify(anchors_pem="nothing here")


class Index(unittest.TestCase):
    def good(self) -> dict:
        return json.loads(load("rsa", "hearth-update-beta.json"))

    def check(self, doc: dict, channel: str | None = None) -> None:
        v.check_index(json.dumps(doc).encode(), channel)

    def refuses(self, doc: dict) -> None:
        with self.assertRaises(v.VerifyError) as caught:
            self.check(doc)
        self.assertEqual(caught.exception.code, "bad_index")

    def test_the_sample_is_good_and_for_its_channel(self) -> None:
        self.check(self.good(), "beta")
        with self.assertRaises(v.VerifyError):
            self.check(self.good(), "stable")

    def test_each_field_is_held_to_its_shape(self) -> None:
        for change in (
            lambda d: d.update(format=2),
            lambda d: d.update(channel="nightly"),
            lambda d: d.update(sequence=0),
            lambda d: d.update(sequence=2**32),
            lambda d: d.update(sequence=True),
            lambda d: d.update(version="1"),
            lambda d: d.update(released="yesterday"),
            lambda d: d.update(images=[]),
            lambda d: d.update(images=[d["images"][0]] * 17),
            lambda d: d["images"][0].update(file="../evil.bin"),
            lambda d: d["images"][0].update(file="a/b.bin"),
            lambda d: d["images"][0].update(file=".hidden"),
            lambda d: d["images"][0].update(file="x" * 97),
            lambda d: d["images"][0].update(sha256=d["images"][0]["sha256"].upper()),
            lambda d: d["images"][0].update(sha256="abc"),
            lambda d: d["images"][0].update(size=0),
            lambda d: d["images"][0].update(size=17 * 1024 * 1024),
            lambda d: d["images"][0].update(flash_size="16 MB"),
            lambda d: d["images"][0].update(psram="yes"),
            lambda d: d["images"][0].update(chip_id=-1),
            lambda d: d["images"][0].pop("project"),
            lambda d: d["images"][0].pop("secure_version"),
        ):
            doc = self.good()
            change(doc)
            with self.subTest(doc=doc):
                self.refuses(doc)

    def test_an_index_over_the_boards_limit_is_refused(self) -> None:
        with self.assertRaises(v.VerifyError):
            v.check_index(b" " * (v.MAX_INDEX_BYTES + 1))

    def test_unknown_keys_are_ignored_so_a_later_format_one_can_add_floors(self) -> None:
        doc = self.good()
        doc["min_leaf_issued"] = "2026-11-01"
        doc["images"][0]["note"] = "x"
        self.check(doc)


class Versions(unittest.TestCase):
    def test_the_order_of_versions(self) -> None:
        for a, b, expected in VERSION_CASES:
            ka, kb = v.version_key(a), v.version_key(b)
            with self.subTest(a=a, b=b):
                self.assertIsNotNone(ka)
                self.assertIsNotNone(kb)
                self.assertEqual((ka > kb) - (ka < kb), expected)

    def test_text_that_is_not_a_version_is_not_one(self) -> None:
        for text in NOT_VERSIONS:
            with self.subTest(text=text):
                self.assertIsNone(v.version_key(text))


class Der(unittest.TestCase):
    def test_a_certificate_cut_short_is_refused(self) -> None:
        pem = anchors("rsa")
        der = v.read_pem_certificates(pem)[0]
        for cut in (0, 1, 5, 40, len(der) // 2, len(der) - 1):
            with self.subTest(cut=cut), self.assertRaises(v.VerifyError):
                v.parse_certificate(der[:cut])

    def test_random_garbage_never_raises_anything_but_a_verify_error(self) -> None:
        der = v.read_pem_certificates(anchors("rsa"))[0]
        for position in range(0, len(der), 17):
            mutated = bytearray(der)
            mutated[position] ^= 0xFF
            with self.subTest(position=position), contextlib.suppress(v.VerifyError):
                v.parse_certificate(bytes(mutated))

    def test_the_fixtures_are_all_there(self) -> None:
        for kind in ("rsa", "ec"):
            for name in (
                "anchor.pem",
                "root.pem",
                "hearth-update-beta.json",
                "hearth-update-stable.sig.json",
            ):
                self.assertTrue((DATA / kind / name).is_file(), name)


if __name__ == "__main__":
    unittest.main()
