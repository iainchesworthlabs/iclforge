"""Holds cenc_make_protected.py's AES-CTR keystream to a published answer.

The keystream is the counter of ISO/IEC 23001-7's `cenc` and `cens` schemes: AES encrypts a 16-byte
counter block that grows by one a block. These are NIST SP 800-38A F.5.1 (CTR-AES128.Encrypt), whose
counter block is the whole 16 bytes, so an IV of that length makes the generator's counter the same;
and a counter that wraps past 2**128, where the stream must carry the way a block cipher mode does.

Needs the `cryptography` package, as the generator does, and is skipped without it; run it with
    python3 -m unittest tools/generators/test_cenc_make_protected.py
"""

import importlib.util
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
HAVE_CRYPTOGRAPHY = importlib.util.find_spec("cryptography") is not None

# NIST SP 800-38A F.5.1: key, initial counter block, and the four blocks of keystream XOR plaintext.
KEY = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
COUNTER = bytes.fromhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff")
PLAINTEXT = bytes.fromhex(
    "6bc1bee22e409f96e93d7e117393172a"
    "ae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52ef"
    "f69f2445df4f9b17ad2b417be66c3710"
)
CIPHERTEXT = bytes.fromhex(
    "874d6191b620e3261bef6864990db6ce"
    "9806f66b7970fdff8617187bb9fffdff"
    "5ae4df3edbd5d35e5b4f09020db03eab"
    "1e031dda2fbe03d1792170a0f3009cee"
)


@unittest.skipUnless(HAVE_CRYPTOGRAPHY, "the cryptography package is not installed")
class CtrKeystream(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(HERE))
        import cenc_make_protected  # noqa: PLC0415 - needs the path above, and cryptography

        cls.module = cenc_make_protected

    def test_matches_nist_sp_800_38a(self):
        stream = self.module._ctr_keystream(KEY, COUNTER, 4)
        self.assertEqual(self.module._xor(PLAINTEXT, stream), CIPHERTEXT)

    def test_no_blocks_is_no_stream(self):
        self.assertEqual(self.module._ctr_keystream(KEY, COUNTER, 0), b"")

    def test_an_eight_byte_iv_is_the_high_half_and_the_low_half_counts_from_zero(self):
        iv = bytes.fromhex("0102030405060708")
        short = self.module._ctr_keystream(KEY, iv, 3)
        whole = self.module._ctr_keystream(KEY, iv + bytes(8), 3)
        self.assertEqual(short, whole)
        # the second block is the encryption of the counter block that ends in 1
        second = self.module._ctr_keystream(KEY, iv + bytes(7) + b"\x01", 1)
        self.assertEqual(short[16:32], second)

    def test_the_counter_wraps_at_two_to_the_128(self):
        last = b"\xff" * 16
        stream = self.module._ctr_keystream(KEY, last, 2)
        self.assertEqual(stream[16:], self.module._ctr_keystream(KEY, bytes(16), 1))


if __name__ == "__main__":
    unittest.main()
