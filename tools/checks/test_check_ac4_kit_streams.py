"""Tests for tools/checks/check_ac4_kit_streams.py on synthetic input: the sync-frame walk, the
signature-marker scan, the forge output parsers, and the signal measurements.

None of the kit's streams is needed. The framing and parser tests run on the plain standard
library (tools/checks and tools/ci are discovered with the system python3); the measurements are
skipped where numpy is not installed."""

import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_ac4_kit_streams as checker

try:
    import numpy as np
except ImportError:  # the measurements need it and the framing does not
    np = None

RATE = 48000


def sync_frame(payload: bytes, crc: bool = True, long_size: bool = False) -> bytes:
    """One AC-4 sync frame; the CRC bytes are zero, the walk does not check them."""
    word = b"\xac\x41" if crc else b"\xac\x40"
    size = (
        struct.pack(">H", len(payload))
        if not long_size
        else b"\xff\xff" + len(payload).to_bytes(3, "big")
    )
    return word + size + payload + (b"\x00\x00" if crc else b"")


def id3_tag(body_size: int) -> bytes:
    sync_safe = bytes((body_size >> s) & 0x7F for s in (21, 14, 7, 0))
    return b"ID3\x04\x00\x00" + sync_safe + bytes(body_size)


class SyncWalk(unittest.TestCase):
    def test_walks_frames_with_and_without_crc_and_with_the_long_size(self):
        data = (
            sync_frame(b"aaaa")
            + sync_frame(b"bbbbbb", crc=False)
            + sync_frame(b"c" * 300, long_size=True)
        )
        walk = checker.walk_sync_frames(data)
        self.assertEqual([len(f) for f in walk.frames], [4, 6, 300])
        self.assertEqual(walk.leftover, 0)
        self.assertEqual(walk.forms, {(0x41, 4), (0x40, 4), (0x41, 7)})

    def test_the_payload_is_the_raw_frame_without_header_or_crc(self):
        walk = checker.walk_sync_frames(sync_frame(b"\x01\x02\x03"))
        self.assertEqual(walk.frames, [b"\x01\x02\x03"])

    def test_bytes_after_the_last_frame_are_leftover(self):
        # A signature trailer is not a sync frame, so the walk stops at it and counts its bytes.
        trailer = b"\x08\x00\x01" + bytes(40)
        walk = checker.walk_sync_frames(sync_frame(b"aaaa") + trailer)
        self.assertEqual(len(walk.frames), 1)
        self.assertEqual(walk.leftover, len(trailer))

    def test_a_truncated_last_frame_is_leftover(self):
        whole = sync_frame(b"abcdefgh")
        walk = checker.walk_sync_frames(sync_frame(b"aaaa") + whole[:-3])
        self.assertEqual(len(walk.frames), 1)
        self.assertEqual(walk.leftover, len(whole) - 3)

    def test_an_id3_tag_stops_the_walk_unless_asked_to_skip_it(self):
        data = id3_tag(20) + sync_frame(b"aaaa") + id3_tag(5) + sync_frame(b"bbbb")
        self.assertEqual(len(checker.walk_sync_frames(data).frames), 0)
        walk = checker.walk_sync_frames(data, skip_id3=True)
        self.assertEqual(len(walk.frames), 2)
        self.assertEqual(walk.id3_tags, 2)
        self.assertEqual(walk.leftover, 0)

    def test_empty_input(self):
        walk = checker.walk_sync_frames(b"")
        self.assertEqual((walk.frames, walk.leftover), ([], 0))


class Markers(unittest.TestCase):
    def test_finds_each_marker(self):
        self.assertEqual(checker.find_markers(b"..pace-ccjf1.."), ["pace-ccjf1"])
        self.assertEqual(checker.find_markers(b"xx ContentSignature xx"), ["ContentSignature"])
        uuid = bytes.fromhex("b715f9db6b41403d8bbe429f3209ca2e")
        self.assertEqual(checker.find_markers(b"..." + uuid), [uuid.hex()])

    def test_clean_data_has_none(self):
        self.assertEqual(checker.find_markers(sync_frame(b"\x00" * 64)), [])

    def test_check_framing_fails_a_stream_with_a_signature_trailer(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "clean.ac4").write_bytes(sync_frame(b"a" * 32) * 3)
            (directory / "signed.ac4").write_bytes(
                sync_frame(b"a" * 32) * 3 + b"\x08\x00\x01" + b"ContentSignature" + bytes(20)
            )
            report = checker.Report()
            kit = checker.Kit(directory, "forge", directory, report)
            with contextlib.redirect_stdout(io.StringIO()):
                checker.check_framing(kit, "clean.ac4")
                clean_failures = report.failed
                checker.check_framing(kit, "signed.ac4")
            self.assertEqual(clean_failures, 0)
            self.assertEqual(report.failed, 2)  # the leftover bytes and the marker


class FrameRate(unittest.TestCase):
    def test_ntsc_rates_are_restored_from_the_printed_value(self):
        self.assertAlmostEqual(checker.exact_frame_rate(29.97), 30000 / 1001, places=9)
        self.assertAlmostEqual(checker.exact_frame_rate(23.976), 24000 / 1001, places=9)
        self.assertAlmostEqual(checker.exact_frame_rate(59.94), 60000 / 1001, places=9)

    def test_integer_rates_are_left_alone(self):
        self.assertEqual(checker.exact_frame_rate(25.0), 25.0)
        self.assertEqual(checker.exact_frame_rate(30.0), 30.0)


class Subsequence(unittest.TestCase):
    def test_in_order_with_gaps(self):
        whole = ["L", "R", "C", "LFE", "Ls", "Rs", "Lb", "Rb"]
        self.assertTrue(checker.is_subsequence(["L", "R", "C", "LFE", "Ls", "Rs"], whole))
        self.assertTrue(checker.is_subsequence([], whole))

    def test_out_of_order_or_missing(self):
        whole = ["L", "R", "C", "LFE", "Ls", "Rs"]
        self.assertFalse(checker.is_subsequence(["R", "L"], whole))
        self.assertFalse(checker.is_subsequence(["L", "Tfl"], whole))


PROBE_TEXT = """\
access units        2128
CRC                 2128 of 2128 valid
frame rate          25.000 fps
presentation 0      id 0, md_compat 1, L R C LFE Ls Rs; English
loudness            -23.0 LKFS
dialnorm            -23 dBFS
"""


class ForgeOutput(unittest.TestCase):
    def test_parse_probe(self):
        probe = checker.parse_probe(PROBE_TEXT)
        self.assertEqual(probe.frames, 2128)
        self.assertEqual(probe.crc_ok, (2128, 2128))
        self.assertEqual(probe.fps, 25.0)
        self.assertEqual(probe.names, ["L", "R", "C", "LFE", "Ls", "Rs"])
        self.assertEqual(probe.declared_lkfs, -23.0)
        self.assertEqual(probe.dialnorm, -23)

    def test_parse_probe_leaves_missing_fields_unset(self):
        probe = checker.parse_probe("access units 3\n")
        self.assertEqual(probe.frames, 3)
        self.assertIsNone(probe.fps)
        self.assertEqual(probe.names, [])
        self.assertIsNone(probe.declared_lkfs)

    def test_parse_decode_layout(self):
        text = "decoded 2128 AC-4 frames -> /tmp/a.wav (L R C LFE Ls Rs, 48000 Hz)\n"
        self.assertEqual(
            checker.parse_decode_layout(text), (["L", "R", "C", "LFE", "Ls", "Rs"], 48000)
        )
        self.assertIsNone(checker.parse_decode_layout("decoded nothing"))


def wav_bytes(samples: "np.ndarray", bits: int, tag: int = 1) -> bytes:
    """A WAVE file of samples [frames, channels] in [-1, 1) as 16- or 24-bit PCM or 32-bit float."""
    channels = samples.shape[1]
    if tag == 3:
        payload = samples.astype("<f4").tobytes()
    elif bits == 16:
        payload = np.round(samples * 32768.0).astype("<i2").tobytes()
    else:
        ints = np.round(samples * 8388608.0).astype(np.int64) & 0xFFFFFF
        payload = b"".join(int(v).to_bytes(3, "little") for v in ints.reshape(-1))
    block = channels * bits // 8
    fmt = struct.pack("<HHIIHH", tag, channels, RATE, RATE * block, block, bits)
    body = (
        b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(payload))
    )
    return b"RIFF" + struct.pack("<I", len(body) + len(payload)) + body + payload


def tone(seconds: float, hz: float, level: float):
    n = np.arange(int(seconds * RATE))
    return level * np.sin(2.0 * np.pi * hz * n / RATE)


@unittest.skipIf(np is None, "numpy is not installed")
class ReadWav(unittest.TestCase):
    def read(self, data: bytes):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.wav"
            path.write_bytes(data)
            return checker.read_wav(path)

    def test_16_bit_pcm(self):
        samples = np.array([[0.5, -0.25], [0.0, 0.125]])
        x, rate, tag, bits, mask = self.read(wav_bytes(samples, 16))
        self.assertEqual((rate, tag, bits, mask), (RATE, 1, 16, 0))
        np.testing.assert_allclose(x, samples, atol=1 / 32768)

    def test_24_bit_pcm_keeps_the_sign(self):
        samples = np.array([[0.5], [-0.5], [-1.0]])
        x, _rate, _tag, bits, _mask = self.read(wav_bytes(samples, 24))
        self.assertEqual(bits, 24)
        np.testing.assert_allclose(x, samples, atol=1 / 8388608)

    def test_32_bit_float(self):
        samples = np.array([[0.25, -0.75], [1.5, 0.0]])
        x, _rate, tag, _bits, _mask = self.read(wav_bytes(samples, 32, tag=3))
        self.assertEqual(tag, 3)
        np.testing.assert_allclose(x, samples)

    def test_not_a_wave_file(self):
        with self.assertRaises(ValueError):
            self.read(b"RIFX" + bytes(40))


@unittest.skipIf(np is None, "numpy is not installed")
class Measurements(unittest.TestCase):
    def test_activity_follows_the_signal(self):
        loud = tone(1.0, 440.0, 0.5)
        quiet = tone(1.0, 440.0, 1e-6)  # 114 dB below the loud channel
        x = np.column_stack(
            [np.concatenate([loud, np.zeros(RATE)]), np.concatenate([quiet, quiet])]
        )
        active = checker.activity(x, RATE)
        self.assertEqual(active.shape, (20, 2))
        self.assertTrue(active[:10, 0].all())
        self.assertFalse(active[10:, 0].any())
        self.assertFalse(active[:, 1].any())

    def test_a_decode_shorter_than_one_window_has_no_windows(self):
        x = np.column_stack([tone(0.04, 440.0, 0.5)] * 2)
        self.assertEqual(checker.window_rms_db(x, RATE).shape, (0, 2))
        self.assertEqual(checker.activity(x, RATE).shape, (0, 2))

    def test_high_band_fraction_separates_an_lfe_from_a_full_band_signal(self):
        low = tone(1.0, 60.0, 0.5)
        mixed = low + tone(1.0, 5000.0, 0.5)
        self.assertLess(checker.high_band_fraction(low, RATE, 200.0), 1e-4)
        self.assertGreater(checker.high_band_fraction(mixed, RATE, 200.0), 0.4)

    def test_silence_has_no_high_band_energy(self):
        self.assertEqual(checker.high_band_fraction(np.zeros(4096), RATE, 200.0), 0.0)

    def test_best_lag_finds_the_shift(self):
        rng = np.random.default_rng(1)
        a = rng.standard_normal(20000)
        b = np.concatenate([np.zeros(137), a])[:20000]  # b is a delayed by 137
        self.assertEqual(checker.best_lag(b, a, 1000), 137)
        self.assertEqual(checker.best_lag(a, b, 1000), -137)

    def test_align_trims_both_to_the_shared_span(self):
        rng = np.random.default_rng(2)
        a = rng.standard_normal((20000, 2))
        b = np.concatenate([np.zeros((50, 2)), a])[:20000]
        aligned_b, aligned_a, lag = checker.align(b, a)
        self.assertEqual(lag, 50)
        self.assertEqual(aligned_b.shape, aligned_a.shape)
        np.testing.assert_array_equal(aligned_b, aligned_a)

    def test_correlation(self):
        t = tone(0.1, 300.0, 1.0)
        self.assertAlmostEqual(checker.correlation(t, t), 1.0, places=9)
        self.assertAlmostEqual(checker.correlation(t, -t), -1.0, places=9)
        self.assertEqual(checker.correlation(t, np.zeros_like(t)), 0.0)

    def test_level_db(self):
        self.assertAlmostEqual(checker.level_db(tone(1.0, 1000.0, 1.0)), -3.01, delta=0.01)

    def test_loudness_of_a_stereo_1khz_tone_is_its_peak_level(self):
        # EBU Tech 3341: a 1 kHz sine at -23 dBFS peak in both channels reads -23.0 LUFS.
        level = 10 ** (-23.0 / 20.0)
        x = np.column_stack([tone(5.0, 1000.0, level)] * 2)
        measured = checker.integrated_loudness(x, RATE, ["L", "R"])
        self.assertAlmostEqual(measured, -23.0, delta=0.15)

    def test_the_lfe_is_left_out_and_the_surrounds_are_weighted(self):
        level = 10 ** (-23.0 / 20.0)
        base = checker.integrated_loudness(
            np.column_stack([tone(5.0, 1000.0, level)] * 2), RATE, ["L", "R"]
        )
        with_lfe = np.column_stack([tone(5.0, 1000.0, level)] * 2 + [tone(5.0, 60.0, 0.5)])
        self.assertAlmostEqual(
            checker.integrated_loudness(with_lfe, RATE, ["L", "R", "LFE"]), base, delta=1e-6
        )
        surround = np.column_stack([tone(5.0, 1000.0, level)] * 2)
        weighted = checker.integrated_loudness(surround, RATE, ["Ls", "Rs"])
        self.assertAlmostEqual(weighted - base, 10 * np.log10(1.41), delta=0.01)

    def test_loudness_is_not_defined_for_heights_or_other_rates(self):
        x = np.column_stack([tone(2.0, 1000.0, 0.1)] * 2)
        self.assertIsNone(checker.integrated_loudness(x, RATE, ["L", "Tfl"]))
        self.assertIsNone(checker.integrated_loudness(x, 44100, ["L", "R"]))

    def test_silence_has_no_loudness(self):
        self.assertIsNone(checker.integrated_loudness(np.zeros((RATE * 2, 2)), RATE, ["L", "R"]))


class Main(unittest.TestCase):
    def run_main(self, *argv: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = checker.main(list(argv))
        return code, out.getvalue()

    def test_skips_when_the_kit_directory_is_not_given(self):
        code, text = self.run_main("--dir", "", "--forge", "forge")
        self.assertEqual(code, 0)
        self.assertTrue(text.startswith("SKIP"))

    def test_skips_when_the_directory_does_not_exist(self):
        code, text = self.run_main("--dir", "/nonexistent/kit", "--forge", "forge")
        self.assertEqual(code, 0)
        self.assertTrue(text.startswith("SKIP"))

    def test_skips_when_there_is_no_forge(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "a.ac4").write_bytes(sync_frame(b"aaaa"))
            code, text = self.run_main("--dir", tmp, "--forge", "/nonexistent/forge")
        self.assertEqual(code, 0)
        self.assertTrue(text.startswith("SKIP"))

    def test_skips_a_directory_with_no_streams(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, text = self.run_main("--dir", tmp, "--forge", sys.executable)
        self.assertEqual(code, 0)
        self.assertTrue(text.startswith("SKIP"))


if __name__ == "__main__":
    unittest.main()
