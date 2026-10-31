"""Unit tests for gen_time_zones.py, which writes the table of time zones a Hearth board offers
(libs/device/include/iclforge/time_zone_data.hpp).

stdlib unittest, as ci.yml's script-lint job runs it. The zone files are made from nothing: a
compiled (TZif version 2) file is a header, two empty blocks and the POSIX string, which is all
the generator reads. The one test that needs IANA's real data (the checked-in header is what the
generator writes from it) is skipped where the `tzdata` package is not installed.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import datetime
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_time_zones as gen


def tzif(footer: str, transitions: tuple[int, ...] = (), version: bytes = b"2") -> bytes:
    """A compiled zone file of the given version with these transitions and POSIX footer."""

    def block(times: tuple[int, ...], width: int) -> bytes:
        count = len(times)
        header = b"TZif" + version + b"\x00" * 15 + struct.pack(">6l", 0, 0, 0, count, 1, 4)
        body = b"".join(struct.pack(">q" if width == 8 else ">l", t) for t in times)
        body += bytes(count)  # which type each transition goes to
        body += struct.pack(">lBB", 0, 0, 0)  # one local time type, abbreviation at 0
        body += b"UTC\x00"
        return header + body

    return (
        block(tuple(t for t in transitions if -(2**31) <= t < 2**31), 4)
        + block(transitions, 8)
        + (b"\n" + footer.encode("ascii") + b"\n")
    )


ZONE_TAB = (
    "# comment\n"
    "AU\t-3749+14458\tAustralia/Melbourne\tVictoria\n"
    "AU\t-3352+15113\tAustralia/Sydney\tNew South Wales\n"
    "JP\t+353916+1394441\tAsia/Tokyo\t\n"
)


class Footer(unittest.TestCase):
    def test_the_footer_is_the_last_line_of_a_version_2_file(self) -> None:
        self.assertEqual(
            gen.footer(tzif("AEST-10AEDT,M10.1.0,M4.1.0/3")), "AEST-10AEDT,M10.1.0,M4.1.0/3"
        )
        self.assertEqual(gen.footer(tzif("<+0530>-5:30", transitions=(0, 100))), "<+0530>-5:30")

    def test_a_version_1_file_has_none_and_a_footer_that_is_not_a_zone_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            gen.footer(tzif("JST-9", version=b"\x00"))
        with self.assertRaises(ValueError):
            gen.footer(b"not a zone file")
        with self.assertRaises(ValueError):
            gen.footer(tzif("JST 9"))  # a space: a zone is typed on one line
        with self.assertRaises(ValueError):
            gen.footer(tzif(""))

    def test_the_last_transition_is_the_largest_in_the_second_block(self) -> None:
        self.assertIsNone(gen.last_transition(tzif("JST-9")))
        self.assertEqual(
            gen.last_transition(tzif("JST-9", transitions=(-5, 10, 1_793_577_600))), 1_793_577_600
        )


class Table(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        (self.root / "zone.tab").write_text(ZONE_TAB, encoding="utf-8")
        (self.root / "tzdata.zi").write_text("# version 2099z\nR x\n", encoding="ascii")
        (self.root / "Australia").mkdir()
        (self.root / "Asia").mkdir()
        (self.root / "Australia" / "Melbourne").write_bytes(tzif("AEST-10AEDT,M10.1.0,M4.1.0/3"))
        (self.root / "Australia" / "Sydney").write_bytes(tzif("AEST-10AEDT,M10.1.0,M4.1.0/3"))
        (self.root / "Asia" / "Tokyo").write_bytes(tzif("JST-9"))
        (self.root / "UTC").write_bytes(tzif("UTC0"))

    def tearDown(self) -> None:
        self.dir.cleanup()

    def test_zone_names_come_from_the_third_column_and_skip_comments(self) -> None:
        self.assertEqual(
            gen.zone_names(ZONE_TAB), ["Australia/Melbourne", "Australia/Sydney", "Asia/Tokyo"]
        )

    def test_the_version_is_read_from_tzdata_zi(self) -> None:
        self.assertEqual(gen.version_of(self.root), "2099z")

    def test_the_table_is_sorted_and_has_utc_and_the_aliases(self) -> None:
        # Canberra is a link to Sydney that zone.tab leaves out; UTC is no place's zone.
        (self.root / "Australia" / "Sydney").write_bytes(tzif("AEST-10AEDT,M10.1.0,M4.1.0/3"))
        rows = gen.build(self.root)
        self.assertEqual(
            rows,
            [
                ("Asia/Tokyo", "JST-9"),
                ("Australia/Canberra", "AEST-10AEDT,M10.1.0,M4.1.0/3"),
                ("Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"),
                ("Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"),
                ("UTC", "UTC0"),
            ],
        )

    def test_a_name_or_string_the_board_cannot_hold_is_refused(self) -> None:
        (self.root / "Asia" / "Tokyo").write_bytes(tzif("A" * 64))
        with self.assertRaises(ValueError):
            gen.build(self.root)

    def test_the_header_is_deterministic_and_says_where_it_came_from(self) -> None:
        rows = gen.build(self.root)
        text = gen.render(rows, "2099z")
        self.assertEqual(text, gen.render(gen.build(self.root), "2099z"))
        self.assertIn("IANA tzdata 2099z", text)
        self.assertIn('{"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},', text)
        self.assertIn('inline constexpr char kTzdataVersion[] = "2099z";', text)
        self.assertTrue(text.startswith("#pragma once\n"))
        self.assertTrue(text.endswith("}  // namespace iclforge::wallclock::data\n"))
        self.assertNotIn("\r", text)


class Rules(unittest.TestCase):
    """The second reading of a POSIX string that --verify holds to tzdata, apart from the C++."""

    @staticmethod
    def at(year: int, month: int, day: int, hour: int = 0, minute: int = 0, second: int = 0) -> int:
        return int(
            datetime.datetime(
                year, month, day, hour, minute, second, tzinfo=datetime.UTC
            ).timestamp()
        )

    def test_melbourne_changes_over_on_the_first_sunday_of_october_and_april(self) -> None:
        melbourne = gen.Rules("AEST-10AEDT,M10.1.0,M4.1.0/3")
        self.assertEqual((melbourne.std, melbourne.dst), (36000, 39600))
        self.assertEqual(melbourne.offset_at(self.at(2026, 10, 3, 15, 59, 59)), 36000)
        self.assertEqual(melbourne.offset_at(self.at(2026, 10, 3, 16)), 39600)
        self.assertEqual(melbourne.offset_at(self.at(2026, 4, 4, 15, 59, 59)), 39600)
        self.assertEqual(melbourne.offset_at(self.at(2026, 4, 4, 16)), 36000)
        self.assertEqual(melbourne.offset_at(self.at(2026, 1, 15, 12)), 39600)
        self.assertEqual(melbourne.offset_at(self.at(2026, 7, 15, 12)), 36000)

    def test_no_daylight_saving_is_one_offset_and_negative_and_overlong_rule_times_read(
        self,
    ) -> None:
        self.assertEqual(gen.Rules("JST-9").offset_at(self.at(2026, 7, 1)), 32400)
        nuuk = gen.Rules("<-02>2<-01>,M3.5.0/-1,M10.5.0/0")
        self.assertEqual(nuuk.start, ("M", 3, 5, 0, -3600))
        self.assertEqual(nuuk.offset_at(self.at(2027, 3, 28, 0, 59, 59)), -7200)
        self.assertEqual(nuuk.offset_at(self.at(2027, 3, 28, 1)), -3600)
        cairo = gen.Rules("EET-2EEST,M4.5.5/0,M10.5.4/24")
        self.assertEqual(cairo.end, ("M", 10, 5, 4, 86400))

    def test_the_last_such_day_is_the_fourth_when_the_month_has_no_fifth(self) -> None:
        # The last Sunday of February 2026 is the 22nd, and in the leap year 2032 the 29th.
        self.assertEqual(
            gen.Rules.day(("M", 2, 5, 0, 7200), 2026), datetime.date(2026, 2, 22).toordinal()
        )
        self.assertEqual(
            gen.Rules.day(("M", 2, 5, 0, 7200), 2032), datetime.date(2032, 2, 29).toordinal()
        )
        # Jn skips February 29 and n counts it.
        self.assertEqual(
            gen.Rules.day(("J", 60, 7200), 2028), datetime.date(2028, 3, 1).toordinal()
        )
        self.assertEqual(
            gen.Rules.day(("N", 59, 7200), 2028), datetime.date(2028, 2, 29).toordinal()
        )


class CheckedInHeader(unittest.TestCase):
    def test_it_is_what_the_generator_writes_from_the_tzdata_it_names(self) -> None:
        try:
            import tzdata  # noqa: F401, PLC0415 - only its presence is asked
        except ImportError:
            self.skipTest("the tzdata package is not installed")
        directory = gen.zoneinfo_dir(None)
        header = gen.OUT.read_text(encoding="utf-8")
        version = gen.version_of(directory)
        if f"IANA tzdata {version}:" not in header:
            self.skipTest(
                f"the installed tzdata is {version}, not the release the header was written from"
            )
        self.assertEqual(gen.render(gen.build(directory), version), header)


class Preferred(unittest.TestCase):
    """Where several zones share a string, one is the place a board names for it (PREFERRED)."""

    MELBOURNE = "AEST-10AEDT,M10.1.0,M4.1.0/3"

    def test_a_shared_string_has_its_preferred_place_and_a_string_one_zone_has_none(self) -> None:
        rows = [
            ("Asia/Tokyo", "JST-9"),
            ("Australia/Canberra", self.MELBOURNE),
            ("Australia/Melbourne", self.MELBOURNE),
            ("Australia/Sydney", self.MELBOURNE),
        ]
        self.assertEqual(gen.preferred_indices(rows), [2])

    def test_a_shared_string_with_no_preferred_place_or_two_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            gen.preferred_indices([("Europe/Rome", "CET"), ("Europe/Vienna", "CET")])
        with self.assertRaises(ValueError):
            gen.preferred_indices(
                [
                    ("Australia/Melbourne", self.MELBOURNE),
                    ("Australia/Brisbane", self.MELBOURNE),
                    ("Australia/Adelaide", self.MELBOURNE),
                ]
            )

    def test_a_preferred_zone_whose_string_is_not_shared_is_refused(self) -> None:
        rows = [("Australia/Melbourne", self.MELBOURNE), ("Asia/Tokyo", "JST-9")]
        with self.assertRaises(ValueError):
            gen.preferred_indices(rows)

    def test_the_real_table_has_exactly_one_for_each_shared_string(self) -> None:
        # Each name is listed once, and in the table's own (sorted) order.
        self.assertEqual(len(set(gen.PREFERRED)), len(gen.PREFERRED))
        self.assertEqual(gen.PREFERRED, tuple(sorted(gen.PREFERRED)))

    def test_the_header_lists_them_as_indices_in_table_order(self) -> None:
        rows = [
            ("Asia/Tokyo", "JST-9"),
            ("Australia/Canberra", self.MELBOURNE),
            ("Australia/Melbourne", self.MELBOURNE),
            ("UTC", "UTC0"),
        ]
        text = gen.render(rows, "2099z")
        self.assertIn("inline constexpr std::uint16_t kPreferred[] = {\n    2,\n};", text)
        self.assertIn("kPreferredCount", text)


if __name__ == "__main__":
    unittest.main()
