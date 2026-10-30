"""Tests for tools/checks/verify_truehd_local_streams.py.

The helpers run on the committed fixtures and on made-up bytes. The self-test of the check
(stand-ins built with FFmpeg's muxers and run through forge) needs ffmpeg, ffprobe and a forge
binary (ICLFORGE_CLI, or forge on PATH) and is skipped without them."""

import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import truehd_standins
import verify_truehd_local_streams as check

FIXTURES = pathlib.Path(__file__).resolve().parents[2] / "testdata" / "truehd"
FORGE = os.environ.get("ICLFORGE_CLI") or shutil.which("forge") or ""
HAVE_TOOLS = bool(
    shutil.which("ffmpeg") and shutil.which("ffprobe") and FORGE and pathlib.Path(FORGE).exists()
)


class Detect(unittest.TestCase):
    def test_a_raw_stream_has_its_major_sync_after_the_four_header_bytes(self):
        self.assertEqual(
            check.detect_container((FIXTURES / "truehd_st48_s24.thd").read_bytes()[:4096]), "raw"
        )

    def test_a_raw_stream_that_opens_with_an_ac3_frame_is_raw(self):
        self.assertEqual(check.detect_container(b"\x0b\x77" + bytes(4094)), "raw")

    def test_matroska_mp4_and_the_two_transport_grids(self):
        self.assertEqual(check.detect_container(b"\x1a\x45\xdf\xa3" + bytes(100)), "matroska")
        self.assertEqual(check.detect_container(b"\x00\x00\x00\x18ftypisom" + bytes(100)), "mp4")
        ts = b"".join(b"\x47" + bytes(187) for _ in range(5))
        self.assertEqual(check.detect_container(ts), "mpegts")
        bdav = b"".join(bytes(4) + b"\x47" + bytes(187) for _ in range(5))
        self.assertEqual(check.detect_container(bdav), "m2ts")
        self.assertEqual(check.detect_container(bytes(200)), "unknown")


class Digests(unittest.TestCase):
    def frames(self, channels, count):
        return b"".join(
            (c * 1000 + n).to_bytes(4, "little", signed=True)
            for n in range(count)
            for c in range(channels)
        )

    def test_the_digests_do_not_depend_on_how_the_words_are_delivered(self):
        words = self.frames(3, 1000)
        whole, pieces = check.Channels(3), check.Channels(3)
        whole.add(words)
        for at in range(0, len(words), 12 * 100):
            pieces.add(words[at : at + 12 * 100])
        self.assertEqual(whole.digests(), pieces.digests())

    def test_channels_are_matched_by_content_whatever_their_order(self):
        a = check.Channels(4)
        a.add(self.frames(4, 50))
        digests = a.digests()
        self.assertEqual(check.match_channels(digests, digests), [0, 1, 2, 3])
        self.assertEqual(check.match_channels(digests, [digests[i] for i in (0, 1, 2, 3, 3)]), None)
        shuffled = [digests[i] for i in (0, 1, 2, 3)][::-1]
        self.assertEqual(check.match_channels(digests, shuffled), [3, 2, 1, 0])
        self.assertIsNone(check.match_channels(digests, ["x", *digests[1:]]))

    def test_one_sample_that_differs_changes_the_digest(self):
        words = bytearray(self.frames(2, 200))
        a, b = check.Channels(2), check.Channels(2)
        a.add(bytes(words))
        words[4 * 2 * 100 + 4] ^= 1  # channel 1, frame 100
        b.add(bytes(words))
        self.assertEqual(a.digests()[0], b.digests()[0])
        self.assertNotEqual(a.digests()[1], b.digests()[1])

    def test_the_width_is_the_lowest_set_bit_of_the_24(self):
        # 16 bit samples in 24 bit words leave the 24 bit sample's low byte clear.
        words = b"".join((n << 16).to_bytes(4, "little", signed=True) for n in range(1, 300))
        c = check.Channels(1)
        c.add(words)
        self.assertEqual(c.low_bits_or, 0)


class Rates(unittest.TestCase):
    def test_mean_and_peak_of_a_fixture(self):
        es = FIXTURES / "truehd_st48_s24.thd"
        mean, peak = check.rates(es, 48000 / 40)
        size = es.stat().st_size
        self.assertAlmostEqual(mean, size * 8 / 0.25 / 1e6, delta=0.01)  # 300 units of 40 at 48 kHz
        self.assertGreaterEqual(peak, mean - 1e-9)


class Report(unittest.TestCase):
    def test_a_row_renders_with_its_notes(self):
        row = check.Row(name="f", container="m2ts", track="0", verdict="DIFFERENT", notes=["why"])
        text = check.render([row])
        self.assertIn("DIFFERENT", text)
        self.assertIn("f track 0: why", text)

    def test_without_a_directory_the_check_skips_and_succeeds(self):
        env = {k: v for k, v in os.environ.items() if k != check.ENV_DIR}
        result = subprocess.run(
            [sys.executable, check.__file__], capture_output=True, text=True, env=env, check=False
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("SKIP", result.stdout)


@unittest.skipUnless(HAVE_TOOLS, "needs ffmpeg, ffprobe and a forge binary (ICLFORGE_CLI)")
class StandIns(unittest.TestCase):
    def test_the_stand_ins_decode_identically(self):
        result = subprocess.run(
            [sys.executable, check.__file__, "--self-test", "--forge", FORGE],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout[-3000:] + result.stderr[-1000:])
        self.assertIn("stand-ins: all good", result.stdout)

    def test_a_transport_stream_with_ac3_frames_reads_as_the_fixture(self):
        import tempfile  # noqa: PLC0415

        source = FIXTURES / "truehd_m51_s24.thd"
        want = truehd_standins.manifest_entry("truehd_m51_s24")["pcm_sha256"]
        with tempfile.TemporaryDirectory() as name:
            for layout in ("separate", "joined"):
                path = pathlib.Path(name) / f"{layout}.m2ts"
                self.assertTrue(truehd_standins.build_core_transport(source, path, layout))
                digest, why = truehd_standins.forge_pcm_digest(FORGE, path, pathlib.Path(name))
                self.assertEqual(digest, want, why)
        self.assertEqual(hashlib.sha256(b"").hexdigest()[:2], "e3")


if __name__ == "__main__":
    unittest.main()
