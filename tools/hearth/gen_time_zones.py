#!/usr/bin/env python3
"""Writes the table of time zones a Hearth board offers.

The output is libs/device/include/iclforge/time_zone_data.hpp:

    python tools/hearth/gen_time_zones.py            write the header
    python tools/hearth/gen_time_zones.py --check    fail when the header is not what this writes
    python tools/hearth/gen_time_zones.py --verify   also hold every zone to tzdata's own offsets

A board has no time zone database (ESP-IDF ships none, and a database is more flash than a clock
is worth), so a zone is a POSIX string (`AEST-10AEDT,M10.1.0,M4.1.0/3`) that the board's C library
applies. This writes the other half: a list of the zones people choose by name
(`Australia/Melbourne`) with the string each one stands for, so that a screen or a web page can
offer a region and a city and a person never has to type a rule.

The data is IANA's tzdata (public domain), read from its compiled files, not from the rule
sources: every compiled zone file ends in the POSIX string for the rules in force from its last
explicit transition on, which is exactly what a board with no database can use. The names are the
ones in tzdata's own zone.tab, one per country and zone (418 in 2026e, Australia complete), plus
UTC and one alias that people look for by name and that zone.tab leaves out,
`Australia/Canberra` (a link to Sydney). zone.tab is a better list than zone1970.tab for this:
that one merges every zone that agreed since 1970, which takes Amsterdam and Oslo out of a list a
person reads.

The compiled files come from the `tzdata` package (`pip install tzdata`; it carries IANA's own
release, and the version is in the header's first line) or from a directory given with
--tzdata-dir (a system's /usr/share/zoneinfo will do). The header is committed with this script
and the version it was made from; nothing at build time reads the data. A new tzdata release that
changes a rule changes the header's diff, which is reviewed like any other.

--verify holds each zone's string to tzdata itself: the offsets Python's zoneinfo gives at every
transition from 2026-11-02 to 2036 (found by bisecting a scan to the second) against this script's
own reading of the string. It takes some seconds and needs zoneinfo to read the same data;
libs/device/tests/test_time_zones.cpp holds the board's code to the same rules at real
transitions.
"""

from __future__ import annotations

import argparse
import datetime
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "libs" / "device" / "include" / "iclforge" / "time_zone_data.hpp"

# Zones people look for by name that zone.tab, which lists only the canonical zone of each place,
# does not have. Each is a link in tzdata's `backward` file and gets the string of its target.
ALIASES = {"Australia/Canberra": "Australia/Sydney"}
# A zone no place owns, which zone.tab never lists: a person who wants UTC says so, rather than
# "no zone".
EXTRAS = ("UTC",)

# Where a POSIX string stands for several places (Melbourne, Sydney, Hobart and Canberra are
# one string), the place a board names when it is told the string and no name: the most
# populous of them, one for each string that is shared (by city, in 2026: Melbourne passed
# Sydney in the ABS's 2023 estimates). A person who set a zone through a page that stored only
# the string reads the place here on the screen, and a place that was not theirs has the same
# clock; they choose their own and it is stored. `preferred_indices` holds every shared string
# to exactly one of these.
PREFERRED = (
    "Africa/Abidjan",
    "Africa/Algiers",
    "Africa/Casablanca",
    "Africa/Dar_es_Salaam",
    "Africa/Johannesburg",
    "Africa/Khartoum",
    "Africa/Lagos",
    "Africa/Tripoli",
    "America/Anchorage",
    "America/Caracas",
    "America/Chicago",
    "America/Denver",
    "America/Halifax",
    "America/Lima",
    "America/Los_Angeles",
    "America/Mexico_City",
    "America/New_York",
    "America/Noronha",
    "America/Nuuk",
    "America/Panama",
    "America/Phoenix",
    "America/Santo_Domingo",
    "America/Sao_Paulo",
    "Asia/Bangkok",
    "Asia/Dhaka",
    "Asia/Dili",
    "Asia/Dubai",
    "Asia/Gaza",
    "Asia/Jakarta",
    "Asia/Seoul",
    "Asia/Shanghai",
    "Asia/Singapore",
    "Asia/Tashkent",
    "Asia/Vladivostok",
    "Asia/Yangon",
    "Atlantic/Madeira",
    "Australia/Adelaide",
    "Australia/Brisbane",
    "Australia/Melbourne",
    "Europe/Berlin",
    "Europe/Istanbul",
    "Europe/Kyiv",
    "Europe/London",
    "Europe/Moscow",
    "Pacific/Apia",
    "Pacific/Auckland",
    "Pacific/Fiji",
    "Pacific/Guam",
    "Pacific/Noumea",
    "Pacific/Pago_Pago",
    "Pacific/Tahiti",
)

# The longest a POSIX string may be (iclforge::wallclock::kTimeZoneMaxBytes) and a zone name
# (kTimeZoneNameMaxBytes).
POSIX_MAX = 63
NAME_MAX = 39

# What a POSIX zone string is made of, at the level this script can check without a copy of the
# grammar: test_time_zones.cpp is the check that every string parses with
# iclforge::wallclock::parse_time_zone.
POSIX_CHARS = re.compile(r"^[A-Za-z0-9<>+\-:.,/]+$")

# --verify checks from this instant (2026-11-02 00:00 UTC, the day after the latest change any zone
# has made or announced for 2026 in 2026e: Vancouver, Edmonton, Winnipeg and Inuvik settle on a
# permanent time on 2026-11-01) to the start of 2036.
VERIFY_FROM = 1793577600
VERIFY_TO = 2082758400


def zoneinfo_dir(explicit: str | None) -> Path:
    """The directory of compiled zone files: --tzdata-dir, the tzdata package, or the system's."""
    if explicit:
        return Path(explicit)
    try:
        import tzdata  # noqa: PLC0415 - optional, and its absence has a message of its own
    except ImportError:
        tzdata = None
    if tzdata is not None:
        return Path(tzdata.__file__).resolve().parent / "zoneinfo"
    system = Path("/usr/share/zoneinfo")
    if (system / "zone.tab").exists():
        return system
    sys.exit("no tzdata found: pip install tzdata, or give --tzdata-dir compiled zone files")


def footer(data: bytes) -> str:
    """The POSIX string a compiled (TZif version 2 or later) zone file ends with."""
    if data[:4] != b"TZif" or data[4:5] in (b"", b"\x00"):
        raise ValueError("not a TZif file of version 2 or later, so it has no POSIX footer")
    lines = data.rstrip(b"\n").split(b"\n")
    text = lines[-1].decode("ascii")
    if not text or not POSIX_CHARS.match(text):
        raise ValueError(f"the footer {text!r} is not a POSIX zone string")
    return text


def last_transition(data: bytes) -> int | None:
    """The instant of the last explicit transition in a compiled (TZif 2+) zone file, or None.

    The POSIX footer describes the zone only from there on: before it the file's own transitions
    rule (Vancouver's footer, `MST7`, is a permanent time that starts on 2026-11-01).
    """

    def counts(at: int) -> tuple[int, int, int, int, int, int]:
        return struct.unpack(">6l", data[at + 20 : at + 44])

    isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt = counts(0)
    second = 44 + timecnt * 4 + timecnt + typecnt * 6 + charcnt + leapcnt * 8 + isstdcnt + isutcnt
    timecnt = counts(second)[3]
    if timecnt == 0:
        return None
    at = second + 44 + (timecnt - 1) * 8
    return struct.unpack(">q", data[at : at + 8])[0]


def zone_names(tab: str) -> list[str]:
    """The zone names of a zone.tab (country, coordinates, name, comments; tab separated)."""
    names = []
    for line in tab.splitlines():
        if line and not line.startswith("#"):
            names.append(line.split("\t")[2])
    return names


def version_of(directory: Path) -> str:
    """The tzdata release, from the first line of tzdata.zi (`# version 2026e`)."""
    zi = directory / "tzdata.zi"
    if zi.exists():
        first = zi.read_text(encoding="ascii").splitlines()[0]
        match = re.match(r"# version (\S+)", first)
        if match:
            return match.group(1)
    sys.exit(f"{zi} has no version line: this does not look like IANA's tzdata")


def build(directory: Path) -> list[tuple[str, str]]:
    """Every zone offered, sorted by name, with its POSIX string."""
    names = sorted({*zone_names((directory / "zone.tab").read_text(encoding="utf-8")), *EXTRAS})
    table = {name: footer((directory / name).read_bytes()) for name in names}
    for alias, target in ALIASES.items():
        table[alias] = table[target]
    rows = sorted(table.items())
    for name, posix in rows:
        if len(posix) > POSIX_MAX or len(name) > NAME_MAX:
            raise ValueError(f"{name}: {posix!r} is longer than the board's fields")
    return rows


def preferred_indices(rows: list[tuple[str, str]], complete: bool = False) -> list[int]:
    """The index of the place each shared string means (PREFERRED), in table order: one for
    every string that more than one name has, and an error for a string with none or two. With
    `complete` (the real table, not a few rows made up for a test) a preferred name that is not
    offered is an error too."""
    groups: dict[str, list[int]] = {}
    for index, (_, posix) in enumerate(rows):
        groups.setdefault(posix, []).append(index)
    offered = {name for name, _ in rows}
    unknown = [name for name in PREFERRED if name not in offered]
    if complete and unknown:
        raise ValueError(f"PREFERRED names zones that are not offered: {unknown}")
    chosen = []
    for posix, members in groups.items():
        if len(members) < 2:
            continue
        among = [i for i in members if rows[i][0] in PREFERRED]
        if len(among) != 1:
            names = [rows[i][0] for i in members]
            raise ValueError(
                f"{posix!r} is shared by {names}, and PREFERRED holds {len(among)} of them, not one"
            )
        chosen.append(among[0])
    taken = {rows[i][0] for i in chosen}
    stray = [name for name in PREFERRED if name in offered and name not in taken]
    if stray:
        raise ValueError(f"PREFERRED has zones whose strings no other zone shares: {stray}")
    return sorted(chosen)


def render(rows: list[tuple[str, str]], version: str) -> str:
    preferred = preferred_indices(rows)
    out = [
        "#pragma once",
        "",
        f"// Generated by tools/hearth/gen_time_zones.py from IANA tzdata {version}: do not edit.",
        f"// {len(rows)} zones (zone.tab, Australia complete) and the POSIX string each one's",
        "// rules end in. See iclforge/time_zones.hpp.",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace iclforge::wallclock::data {",
        "",
        "struct ZoneRow {",
        "    const char* name;",
        "    const char* posix;",
        "};",
        "",
        f'inline constexpr char kTzdataVersion[] = "{version}";',
        "",
        "inline constexpr ZoneRow kZones[] = {",
    ]
    out += [f'    {{"{name}", "{posix}"}},' for name, posix in rows]
    out += [
        "};",
        "",
        "inline constexpr std::size_t kZoneCount = sizeof(kZones) / sizeof(kZones[0]);",
        "",
        "// The place a string stands for when several names share it, as indices into kZones",
        "// (PREFERRED in the generator): one for every shared string, in table order.",
        "inline constexpr std::uint16_t kPreferred[] = {",
        *[f"    {index}," for index in preferred],
        "};",
        "",
        "inline constexpr std::size_t kPreferredCount =",
        "    sizeof(kPreferred) / sizeof(kPreferred[0]);",
        "",
        "}  // namespace iclforge::wallclock::data",
        "",
    ]
    return "\n".join(out)


# --- --verify: each string against tzdata's own offsets, with a second reading of the string ---


class Rules:
    """A POSIX zone string read a second time, in Python, for --verify.

    The board's C++ reader is the one that ships; this is only what checks the strings against
    tzdata (the C++ is held to the same instants by test_time_zones.cpp).
    """

    def __init__(self, text: str):
        self.text = text
        self.at = 0
        self.std = self.dst = None
        self.start = self.end = None
        self.parse()

    def name(self) -> None:
        if self.text[self.at] == "<":
            self.at = self.text.index(">", self.at) + 1
        else:
            while self.at < len(self.text) and self.text[self.at].isalpha():
                self.at += 1

    def hms(self) -> int:
        match = re.compile(r"([+-]?)(\d{1,3})(?::(\d{2}))?(?::(\d{2}))?").match(self.text, self.at)
        self.at = match.end()
        sign = -1 if match.group(1) == "-" else 1
        seconds = int(match.group(2)) * 3600
        seconds += int(match.group(3) or 0) * 60 + int(match.group(4) or 0)
        return sign * seconds

    def rule(self) -> tuple:
        pattern = re.compile(r"(?:M(\d+)\.(\d)\.(\d)|J(\d+)|(\d+))(?:/([+-]?[\d:]+))?")
        match = pattern.match(self.text, self.at)
        self.at = match.end()
        time_s = 7200
        if match.group(6):
            saved = self.text, self.at
            self.text, self.at = match.group(6), 0
            time_s = self.hms()
            self.text, self.at = saved
        if match.group(1):
            return ("M", int(match.group(1)), int(match.group(2)), int(match.group(3)), time_s)
        if match.group(4):
            return ("J", int(match.group(4)), time_s)
        return ("N", int(match.group(5)), time_s)

    def parse(self) -> None:
        self.name()
        self.std = -self.hms()
        if self.at >= len(self.text):
            return
        self.name()
        self.dst = self.std + 3600
        if self.text[self.at] != ",":
            self.dst = -self.hms()
        self.at += 1
        self.start = self.rule()
        self.at += 1
        self.end = self.rule()

    @staticmethod
    def day(rule: tuple, year: int) -> int:
        """The day (a proleptic ordinal) a rule falls on."""
        new_year = datetime.date(year, 1, 1).toordinal()
        if rule[0] == "J":
            leap = year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)
            return new_year + rule[1] - 1 + (1 if leap and rule[1] >= 60 else 0)
        if rule[0] == "N":
            return new_year + rule[1]
        _, month, week, weekday, _ = rule
        first = datetime.date(year, month, 1)
        delta = (weekday - (first.weekday() + 1) % 7) % 7
        day = 1 + delta + (week - 1) * 7
        while True:
            try:
                return datetime.date(year, month, day).toordinal()
            except ValueError:
                day -= 7

    def offset_at(self, unix_s: int) -> int:
        if self.dst is None:
            return self.std
        epoch = datetime.date(1970, 1, 1).toordinal()
        year = datetime.datetime.fromtimestamp(unix_s + self.std, datetime.UTC).year
        start = (self.day(self.start, year) - epoch) * 86400 + self.start[-1] - self.std
        end = (self.day(self.end, year) - epoch) * 86400 + self.end[-1] - self.dst
        daylight = start <= unix_s < end if start < end else (unix_s >= start or unix_s < end)
        return self.dst if daylight else self.std


def verify(rows: list[tuple[str, str]], directory: Path) -> int:
    """Every zone's string against zoneinfo reading `directory`, at every instant its offset
    changes. Returns the number of failures."""
    import zoneinfo  # noqa: PLC0415

    zoneinfo.reset_tzpath([str(directory)])
    begin, finish = VERIFY_FROM, VERIFY_TO
    failures = 0
    checked = 0
    for name, posix in rows:
        source = ALIASES.get(name, name)
        zone = zoneinfo.ZoneInfo(source)
        rules = Rules(posix)
        # The string is the zone's rule from the file's last explicit transition on, and no
        # earlier. A zone with explicit transitions after the years checked (Gaza and Hebron have
        # them to 2086) is held to its string all the same: it is the board's only rule, and what
        # tzdata says there is the thing to compare it to.
        last = last_transition((directory / source).read_bytes())
        if last is not None and last > begin:
            print(f"{name}: explicit transitions run to {last}; held to the string to {finish}")

        def real(t: int, zone: zoneinfo.ZoneInfo = zone) -> int:
            return int(datetime.datetime.fromtimestamp(t, zone).utcoffset().total_seconds())

        t = begin
        previous = real(t)
        if rules.offset_at(t) != previous:
            says = rules.offset_at(t)
            print(f"{name}: {posix} says {says} at {t}, tzdata {previous}", file=sys.stderr)
            failures += 1
        while t < finish:
            step = min(t + 6 * 3600, finish)
            now = real(step)
            if now != previous:
                low, high = t, step
                while high - low > 1:
                    mid = (low + high) // 2
                    low, high = (mid, high) if real(mid) == previous else (low, mid)
                for instant, want in ((high - 1, previous), (high, now)):
                    checked += 1
                    got = rules.offset_at(instant)
                    if got != want:
                        failures += 1
                        print(
                            f"{name}: {posix} says {got} at {instant}, tzdata {want}",
                            file=sys.stderr,
                        )
                previous = now
            t = step
    print(f"{len(rows)} zones, {checked} transitions, {failures} disagreements")
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--tzdata-dir", help="a directory of compiled zone files (default: the tzdata package)"
    )
    parser.add_argument(
        "--check", action="store_true", help="fail when the header is not what this would write"
    )
    parser.add_argument(
        "--verify", action="store_true", help="hold every zone to tzdata's own offsets (slow)"
    )
    args = parser.parse_args(argv)

    directory = zoneinfo_dir(args.tzdata_dir)
    rows = build(directory)
    preferred_indices(rows, complete=True)
    text = render(rows, version_of(directory))
    status = 0
    if args.check:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            print(
                f"{OUT.relative_to(REPO)} is out of date; run tools/hearth/gen_time_zones.py",
                file=sys.stderr,
            )
            status = 1
    else:
        OUT.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {OUT.relative_to(REPO)}: {len(rows)} zones")
    if args.verify and verify(rows, directory):
        status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
