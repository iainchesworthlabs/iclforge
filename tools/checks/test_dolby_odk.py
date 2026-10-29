"""Unit tests for dolby_odk.py and the check_dolby_* scripts (Dolby's Online Delivery Kit tools).

No Dolby program is needed: the tools are replaced by small shell scripts written here, and the
mp4drop dumps are a few lines written in the same shape. stdlib `unittest`, like the other tests in
this directory.

Run: python3 -m unittest discover -s tools/checks -p 'test_dolby_odk.py'
"""

import contextlib
import io
import os
import stat
import struct
import sys
import tempfile
import unittest
import unittest.mock as mock
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_dolby_cenc as cenc
import check_dolby_demux_roundtrip as roundtrip
import check_dolby_mp4_boxes as boxes_check
import dolby_odk as odk
import dolby_odk_corpus as corpus

POSIX = os.name == "posix"

DEC3_DUMP = """<?xml version="1.0" encoding="ISO-8859-1"?>
<mp4drop>
<moov size="100">
  <trak size="90">
    <mdia size="80">
      <minf size="70">
        <stbl size="60">
          <stsd size="50">
            <entry_count type="unsigned int(32)">1</entry_count>
            <ec-3 size="43">
              <channelcount type="unsigned int(16)">2</channelcount>
              <samplerate type="unsigned int(16)">48000</samplerate>
              <dec3 size="7">
                <data_rate type="unsigned int(13)">640</data_rate>
                <num_ind_sub type="unsigned int(3)">0</num_ind_sub>
                <fscod type="unsigned int(2)">0</fscod>
                <bsid type="unsigned int(5)">16</bsid>
                <bsmod type="unsigned int(3)">0</bsmod>
                <acmod type="unsigned int(3)">7</acmod>
                <lfeon type="unsigned int(1)">1</lfeon>
                <flag_ec3_extension_type_a type="unsigned int(1)">1</flag_ec3_extension_type_a>
                <complexity_index_type_a type="unsigned int(8)">16</complexity_index_type_a>
              </dec3>
            </ec-3>
          </stsd>
        </stbl>
      </minf>
    </mdia>
  </trak>
</moov>
</mp4drop>
"""


def make_tool(directory: Path, name: str, script: str) -> Path:
    path = directory / name
    path.write_text("#!/bin/sh\n" + script)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class FindAndSkipTests(unittest.TestCase):
    def test_unset_environment_skips_with_a_reason(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertIn(odk.ENV_VAR, odk.skip_reason())
            self.assertIsNone(odk.find_tool("mp4drop"))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(odk.main(["where"]), odk.SKIP_EXIT)

    @unittest.skipUnless(POSIX, "shell-script tools")
    def test_finds_a_tool_in_each_supported_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_tool(root, "mp4drop", "exit 0\n")
            self.assertEqual(odk.find_tool("mp4drop", root), root / "mp4drop")
            self.assertIsNone(odk.find_tool("streamdemuxer", root))
            self.assertIn("streamdemuxer", odk.skip_reason("mp4drop", "streamdemuxer", root=root))
            folder = odk.platform_folder()
            if folder:
                nested = root / "streamdemuxer" / folder
                nested.mkdir(parents=True)
                make_tool(nested, "streamdemuxer", "exit 0\n")
                self.assertEqual(odk.find_tool("streamdemuxer", root), nested / "streamdemuxer")
                self.assertIsNone(odk.skip_reason("mp4drop", "streamdemuxer", root=root))


@unittest.skipUnless(POSIX, "shell-script tools")
class RunToolTests(unittest.TestCase):
    def test_tool_gets_an_empty_environment_and_the_requested_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_tool(
                root,
                "mp4drop",
                'echo "args=$*"; echo "secret=${ICLFORGE_TEST_SECRET:-unset}"; '
                'echo "home=${HOME:-unset}"\n',
            )
            with mock.patch.dict(
                os.environ, {"ICLFORGE_TEST_SECRET": "leak", "HOME": "/nonexistent"}
            ):
                result = odk.run_tool("mp4drop", ["a b", "-m", "1"], root=root)
            self.assertEqual(result.returncode, 0)
            self.assertIn("args=a b -m 1", result.text)
            self.assertIn("secret=unset", result.text)
            self.assertIn("home=unset", result.text)

    def test_output_is_capped_and_a_hang_is_stopped(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_tool(
                root,
                "mp4drop",
                'i=0; while [ $i -lt 2000 ]; do echo "0123456789"; i=$((i+1)); done\n',
            )
            make_tool(root, "streamdemuxer", "sleep 30\n")
            capped = odk.run_tool("mp4drop", [], root=root, max_output=1000)
            self.assertTrue(capped.truncated)
            self.assertEqual(len(capped.stdout), 1000)
            hung = odk.run_tool("streamdemuxer", [], root=root, timeout=0.5)
            self.assertTrue(hung.timed_out)

    def test_missing_tool_is_an_error_not_a_skip(self):
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(FileNotFoundError):
            odk.run_tool("mp4drop", [], root=tmp)

    def test_streamdemuxer_lists_what_the_tool_wrote(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "tools"
            root.mkdir()
            make_tool(
                root,
                "streamdemuxer",
                'out=""; while [ $# -gt 0 ]; do [ "$1" = "-o" ] && out="$2"; shift; done; '
                'echo x > "$out/a_track_id_1.out.ec3"\n',
            )
            result = odk.streamdemuxer(Path(tmp) / "in.mp4", Path(tmp) / "out", root=root)
            self.assertEqual([p.name for p in result.files], ["a_track_id_1.out.ec3"])


class Mp4dropParsingTests(unittest.TestCase):
    def test_sample_entry_and_config_box_values(self):
        top = odk.parse_mp4drop_xml(DEC3_DUMP)
        entries = odk.audio_sample_entries(top)
        self.assertEqual(len(entries), 1)
        track, entry = entries[0]
        self.assertEqual((track, entry.kind, entry.int_value("channelcount")), (1, "ec-3", 2))
        dec3 = boxes_check.config_box(entry)
        self.assertEqual(dec3.kind, "dec3")
        self.assertEqual(dec3.int_value("complexity_index_type_a"), 16)
        self.assertIsNone(dec3.int_value("not_there"))

    def test_malformed_dump_is_a_value_error(self):
        with self.assertRaises(ValueError):
            odk.parse_mp4drop_xml("<mp4drop><moov size='1'>")


class ExpectedConfigTests(unittest.TestCase):
    def joc_probe(self):
        return {
            "stream": {
                "sample_rate_hz": 48000,
                "bsid": 16,
                "bsmod": 0,
                "acmod": 7,
                "lfeon": True,
                "bitrate_kbps": 640.0,
                "nominal_bitrate_kbps": None,
                "substreams_per_access_unit": 1,
                "objects": {"joc": True, "complexity_index": 16},
            }
        }

    def test_dec3_values_for_a_joc_stream_match_the_dump(self):
        want = boxes_check.expected_config("joc", self.joc_probe())
        self.assertEqual(want["data_rate"], 640)
        self.assertEqual(want["num_ind_sub"], 0)
        self.assertEqual(want["flag_ec3_extension_type_a"], 1)
        dec3 = boxes_check.config_box(
            boxes_check.odk.audio_sample_entries(odk.parse_mp4drop_xml(DEC3_DUMP))[0][1]
        )
        self.assertEqual(boxes_check.compare_config("joc", want, dec3)[0].status, odk.PASS)

    def test_a_wrong_field_is_a_disagreement_naming_it(self):
        want = boxes_check.expected_config("joc", self.joc_probe())
        want["bsid"] = 6
        dec3 = boxes_check.config_box(
            boxes_check.odk.audio_sample_entries(odk.parse_mp4drop_xml(DEC3_DUMP))[0][1]
        )
        result = boxes_check.compare_config("joc", want, dec3)[0]
        self.assertEqual(result.status, odk.DISAGREE)
        self.assertIn("bsid", result.detail)

    def test_ac3_bit_rate_code_indexes_the_rate_table(self):
        probe = {
            "stream": {
                "sample_rate_hz": 48000,
                "bsid": 8,
                "bsmod": 0,
                "acmod": 2,
                "lfeon": False,
                "bitrate_kbps": 192.0,
                "nominal_bitrate_kbps": None,
            }
        }
        self.assertEqual(boxes_check.expected_config("ac3", probe)["bit_rate_code"], 10)


class StreamComparisonTests(unittest.TestCase):
    @staticmethod
    def frame(payload, sync=0xAC40, long_form=False, crc=False):
        header = struct.pack(">H", sync)
        header += (
            struct.pack(">H", 0xFFFF) + len(payload).to_bytes(3, "big")
            if long_form
            else struct.pack(">H", len(payload))
        )
        return header + payload + (b"\x00\x00" if crc else b"")

    def test_ac4_framings_that_carry_the_same_frames_compare_equal(self):
        a = self.frame(b"abc") + self.frame(b"defg")
        b = self.frame(b"abc", 0xAC41, crc=True) + self.frame(b"defg", long_form=True)
        self.assertEqual(corpus.ac4_frames(a), [b"abc", b"defg"])
        self.assertTrue(corpus.same_stream("ac4", a, b))
        self.assertFalse(corpus.same_stream("ac4", a, self.frame(b"abc") + self.frame(b"XXXX")))

    def test_junk_and_truncation_are_not_streams(self):
        self.assertIsNone(corpus.ac4_frames(b"\x00\x01\x02\x03\x04"))
        self.assertIsNone(corpus.ac4_frames(self.frame(b"abcdef")[:-2]))
        self.assertFalse(corpus.same_stream("ac4", b"junk!", b"junk!"))

    def test_other_codecs_compare_byte_for_byte(self):
        self.assertTrue(corpus.same_stream("eac3", b"\x0b\x77abc", b"\x0b\x77abc"))
        self.assertFalse(corpus.same_stream("eac3", b"\x0b\x77abc", b"\x0b\x77abd"))


class DemuxResultTests(unittest.TestCase):
    def test_failure_text_names_a_crash_a_timeout_and_a_message(self):
        crash = odk.ToolResult(-11, b"", b"")
        self.assertIn("signal 11", roundtrip.tool_failure(crash))
        self.assertEqual(
            roundtrip.tool_failure(odk.ToolResult(-1, b"", b"", timed_out=True)), "timed out"
        )
        self.assertIn(
            "exit 1: ERROR: no", roundtrip.tool_failure(odk.ToolResult(1, b"ERROR: no\n", b""))
        )

    def test_only_audio_streams_are_taken_from_the_tool_output(self):
        result = odk.ToolResult(0, b"", b"", files=[Path("a.264"), Path("a.ec3"), Path("b.ac4")])
        self.assertEqual([p.name for p in roundtrip.dolby_elementary(result)], ["a.ec3", "b.ac4"])


class CencCheckTests(unittest.TestCase):
    @staticmethod
    def dump(scheme="cenc", iv=8, kid=None, frma="ec-3", senc_count=16, trun_count=16):
        def field(name, value):
            return f'<{name} type="x">{value}</{name}>'

        kid = kid or cenc.KID1.upper()
        iv_size = iv if scheme != "cbcs" else 0
        senc = (
            f'<senc size="9">{field("sample_count", senc_count)}</senc>' if scheme != "cbcs" else ""
        )
        tenc = (
            field("default_isProtected", 1)
            + field("default_Per_Sample_IV_Size", iv_size)
            + field("default_KID", kid)
            + field("default_constant_IV_size", 16)
        )
        sinf = (
            f'<frma size="4">{field("data_format", frma)}</frma>'
            f'<schm size="12">{field("scheme_type", scheme)}</schm>'
            f'<schi size="1"><tenc size="1">{tenc}</tenc></schi>'
        )
        stsd = f'<enca size="1">{field("channelcount", 2)}<sinf size="1">{sinf}</sinf></enca>'
        moov = (
            '<moov size="1"><trak size="1"><mdia size="1"><minf size="1"><stbl size="1">'
            f'<stsd size="1">{stsd}</stsd></stbl></minf></mdia></trak></moov>'
        )
        trun = f'<trun size="1">{field("sample_count", trun_count)}</trun>'
        moof = f'<moof size="1"><traf size="1">{trun}{senc}</traf></moof>'
        return odk.parse_mp4drop_xml(f"<mp4drop>{moov}{moof}</mp4drop>")

    def test_a_file_that_matches_what_the_generator_wrote_passes(self):
        self.assertEqual(
            cenc.check_boxes("x", cenc.Case("eac3", "cenc"), "ec-3", self.dump()).status, odk.PASS
        )
        self.assertEqual(
            cenc.check_boxes("x", cenc.Case("eac3", "cbcs"), "ec-3", self.dump("cbcs")).status,
            odk.PASS,
        )

    def test_each_kind_of_mismatch_is_reported(self):
        case = cenc.Case("eac3", "cenc")
        for label, dump, text in (
            ("scheme", self.dump("cbc1", 16), "schm"),
            ("original format", self.dump(frma="ac-3"), "frma"),
            ("key id", self.dump(kid="00" * 16), "default_KID"),
            ("iv size", self.dump(iv=16), "default_Per_Sample_IV_Size"),
            ("senc count", self.dump(senc_count=15), "senc 15 vs trun 16"),
        ):
            with self.subTest(label):
                result = cenc.check_boxes("x", case, "ec-3", dump)
                self.assertEqual(result.status, odk.DISAGREE)
                self.assertIn(text, result.detail)

    def test_decrypt_legs_are_classified_by_what_the_tool_did(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            clear = b"\x0b\x77" + b"a" * 20
            out = work / "dolby" / "case" / "audio.out.ec3"
            out.parent.mkdir(parents=True)

            def fake(text, code, data=None):
                if data is not None:
                    out.write_bytes(data)
                files = [out] if data is not None else []
                return odk.ToolResult(code, text.encode(), b"", files=files)

            cases = (
                (lambda: fake("ok", 0, clear), odk.PASS),
                (lambda: fake("ok", 0, clear[:-1] + b"X"), odk.DISAGREE),
                (lambda: fake("ERROR: Need a key for decryption", 1), odk.SKIP),
                (lambda: fake("", -11), odk.SKIP),
                (lambda: fake("Succeed", 0), odk.SKIP),
            )
            for make_result, status in cases:
                tool_result = make_result()
                with self.subTest(status=status, rc=tool_result.returncode):
                    with mock.patch.object(odk, "streamdemuxer", return_value=tool_result):
                        got = cenc.decrypt_leg("case", "eac3", Path("p.mp4"), clear, work, None)
                    self.assertEqual(got.status, status)

    def test_expected_tenc_follows_the_scheme_and_options(self):
        self.assertEqual(
            cenc.expected_tenc(cenc.Case("ac3", "cbcs"))["default_Per_Sample_IV_Size"], 0
        )
        self.assertEqual(
            cenc.expected_tenc(cenc.Case("ac3", "cenc", "core", ("--iv-size", "16")))[
                "default_Per_Sample_IV_Size"
            ],
            16,
        )
        self.assertEqual(len(cenc.cases(True)), 16)
        self.assertGreater(len(cenc.cases(False)), 90)


class ExtractionTests(unittest.TestCase):
    def archive(self, names):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as zf:
            for name in names:
                zf.writestr(name, b"#!/bin/sh\nexit 0\n")
        return buffer.getvalue()

    def test_extracts_only_the_platform_member(self):
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(odk, "platform_folder", return_value="Linux64"),
            mock.patch.object(odk, "tool_filename", side_effect=lambda n: n),
        ):
            path = odk.extract_platform_member(
                self.archive(["mp4drop/Linux64/mp4drop", "mp4drop/Mac/mp4drop"]),
                "mp4drop",
                Path(tmp),
            )
            self.assertEqual(path, Path(tmp) / "mp4drop")
            self.assertEqual([p.name for p in Path(tmp).iterdir()], ["mp4drop"])

    def test_unsafe_member_names_are_refused(self):
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(odk, "platform_folder", return_value="Linux64"),
        ):
            for bad in ("../evil", "/abs/evil", "mp4drop/../../evil"):
                with self.subTest(bad), self.assertRaises(RuntimeError):
                    odk.extract_platform_member(
                        self.archive(["mp4drop/Linux64/mp4drop", bad]), "mp4drop", Path(tmp)
                    )

    def test_recorded_digests_are_sha256_hex(self):
        for digest in odk.TOOL_ARCHIVES.values():
            self.assertRegex(digest, r"^[0-9a-f]{64}$")


class ReportTests(unittest.TestCase):
    def test_counts_and_exit_status(self):
        results = [
            odk.Result("a", odk.PASS),
            odk.Result("b", odk.SKIP, "why"),
            odk.Result("c", odk.NOTE, "n"),
        ]
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(odk.print_report("t", results), 0)
        self.assertIn("1 pass, 0 disagree, 0 error, 1 note, 1 skip", out.getvalue())
        with contextlib.redirect_stdout(io.StringIO()):
            failing = [*results, odk.Result("d", odk.DISAGREE, "x")]
            self.assertEqual(odk.print_report("t", failing), 1)


if __name__ == "__main__":
    unittest.main()
