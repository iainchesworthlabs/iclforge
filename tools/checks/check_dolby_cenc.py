#!/usr/bin/env python3
"""Dolby's tools against protected (Common Encryption) Dolby audio files.

planning/cenc-conformance.md says what ISO/IEC 23001-7 asks of a protected Dolby audio track and
that the project implements none of it. This script asks Dolby's own programs what they make of the
test files `tools/generators/cenc_make_protected.py` writes (and of GPAC's `MP4Box -crypt`, when
installed) for AC-3, E-AC-3, E-AC-3 JOC and AC-4, in the four schemes cenc, cbc1, cens and cbcs.

Leg 1, boxes (mp4drop): a protected sample entry (`enca`), `frma` naming the original format, `schm`
naming the scheme, `tenc` with the default key id, IV size, constant IV and pattern the generator
wrote, and `senc` with one entry per sample where the scheme sends IVs. This says Dolby's parser
reads the layout the generator writes. It says nothing about whether the cipher output is right.

Leg 2, decryption (streamdemuxer --decryption-keys): the elementary stream the tool writes, given
the test key, must equal the clear stream. Only this leg is evidence about the cipher. It is gated:
when the tool answers "Need a key for decryption" for every protected file although the matching key
was supplied (the 1.0.0.5519896 tool of September 2020 does this for every file tried, FFmpeg's
included), the legs are summarised as one skip, because a result that cannot be told from "the tool
ignores the key" proves nothing. --require-decrypt turns that skip into an error.

    python3 tools/checks/check_dolby_cenc.py --cli build/bin/forge [--quick] [--require-decrypt]

Needs the tools (tools/checks/dolby_odk.py) and python's `cryptography` package for the generator;
exits 77 without the tools. The keys are fixed test values. Exit status: 0 when nothing disagreed,
1 otherwise. A Dolby tool decrypting a file is evidence about the layout of that file; it is not a
certificate of conformance with ISO/IEC 23001-7.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import dolby_odk as odk
import dolby_odk_corpus as corpus

KID1 = "a1b2c3d4e5f60718293a4b5c6d7e8f90"
KEY1 = "00112233445566778899aabbccddeeff"
GENERATOR = corpus.REPO / "tools" / "generators" / "cenc_make_protected.py"
SCHEMES = ("cenc", "cbc1", "cens", "cbcs")
VARIANTS = ("core", "leader_entry", "leader_entry_rev", "leader_seig", "rotate")
NEEDS_KEY = "Need a key for decryption"


@dataclass(frozen=True)
class Case:
    """One protected file to make."""

    codec: str
    scheme: str
    variant: str = "core"
    extra: tuple[str, ...] = ()

    @property
    def label(self) -> str:
        extra = "".join("_" + e.strip("-").replace(":", "x") for e in self.extra)
        return f"{self.codec}_{self.scheme}_{self.variant}{extra}"


def cases(quick: bool) -> list[Case]:
    out: list[Case] = []
    for codec in corpus.ENTRY_OF:
        for scheme in SCHEMES:
            for variant in ("core",) if quick else VARIANTS:
                out.append(Case(codec, scheme, variant))
            if not quick and scheme == "cenc":
                out.append(Case(codec, scheme, "core", ("--iv-size", "16")))
            if not quick and scheme in ("cens", "cbcs"):
                out.append(Case(codec, scheme, "core", ("--pattern", "1:9")))
    return out


def expected_tenc(case: Case) -> dict[str, int | str]:
    """What the generator writes into tenc, from its documented behaviour (not from the tool)."""
    want: dict[str, int | str] = {"default_isProtected": 1, "default_KID": KID1.upper()}
    if case.scheme == "cenc":
        want["default_Per_Sample_IV_Size"] = 16 if "16" in case.extra else 8
    elif case.scheme == "cbc1":
        want["default_Per_Sample_IV_Size"] = 16
    elif case.scheme == "cens":
        want["default_Per_Sample_IV_Size"] = 8
    else:
        want["default_Per_Sample_IV_Size"] = 0
        want["default_constant_IV_size"] = 16
    return want


def find_protection(boxes: list[odk.Box]) -> dict[str, odk.Box]:
    """The first `enca` entry's frma, schm, tenc and the moov-level list, from an mp4drop dump."""
    found: dict[str, odk.Box] = {}
    for top in boxes:
        for _, box in top.walk():
            if box.kind in ("enca", "frma", "schm", "tenc") and box.kind not in found:
                found[box.kind] = box
    return found


def trafs_with_senc(boxes: list[odk.Box]) -> tuple[int, int, list[str]]:
    """(fragments, fragments with senc, senc sample counts that differ from the trun's)."""
    total = with_senc = 0
    problems: list[str] = []
    for top in boxes:
        if top.kind != "moof":
            continue
        for traf in top.find_all("traf"):
            total += 1
            senc, trun = traf.find("senc"), traf.find("trun")
            if senc is None:
                continue
            with_senc += 1
            in_senc = senc.int_value("sample_count")
            if trun is not None and in_senc != trun.int_value("sample_count"):
                problems.append(f"senc {in_senc} vs trun {trun.int_value('sample_count')}")
    return total, with_senc, problems


def check_boxes(subject: str, case: Case, entry_name: str, boxes: list[odk.Box]) -> odk.Result:
    found = find_protection(boxes)
    missing = [k for k in ("enca", "frma", "schm", "tenc") if k not in found]
    if missing:
        return odk.Result(subject, odk.DISAGREE, "mp4drop shows no " + ", ".join(missing))
    wrong: list[str] = []
    if found["frma"].value("data_format") != entry_name:
        wrong.append(f"frma {found['frma'].value('data_format')} (want {entry_name})")
    if found["schm"].value("scheme_type") != case.scheme:
        wrong.append(f"schm {found['schm'].value('scheme_type')} (want {case.scheme})")
    for name, want in expected_tenc(case).items():
        got = found["tenc"].value(name)
        if str(got).upper() != str(want).upper():
            wrong.append(f"tenc {name} {got} (want {want})")
    total, with_senc, problems = trafs_with_senc(boxes)
    wrong += problems
    if case.scheme == "cbcs" and with_senc:
        wrong.append("senc present although the scheme uses a constant IV")
    if case.scheme != "cbcs" and total and not with_senc:
        wrong.append("no fragment carries senc")
    if wrong:
        return odk.Result(subject, odk.DISAGREE, "; ".join(wrong))
    return odk.Result(subject, odk.PASS)


def make_protected(clear_mp4: Path, case: Case, out: Path) -> str | None:
    argv = [
        sys.executable,
        str(GENERATOR),
        str(clear_mp4),
        str(out),
        case.scheme,
        "--variant",
        case.variant,
        *case.extra,
    ]
    done = subprocess.run(argv, capture_output=True, text=True, check=False)
    return None if done.returncode == 0 and out.exists() else done.stderr.strip()[-160:]


def gpac_protected(clear_mp4: Path, scheme: str, work: Path) -> Path | None:
    if shutil.which("MP4Box") is None:
        return None
    head = '<CrypTrack trackID="1" IsEncrypted="1" '
    tail = ' saiSavedBox="senc">'
    iv8 = head + 'IV_size="8" first_IV="0x0001020304050607"' + tail
    iv16 = head + 'IV_size="16" first_IV="0x000102030405060708090a0b0c0d0e0f"' + tail
    cbcs = (
        '<CrypTrack trackID="1" IsEncrypted="1" constant_IV="0x000102030405060708090a0b0c0d0e0f" '
        'crypt_byte_block="1" skip_byte_block="0" saiSavedBox="senc">'
    )
    cens = (
        '<CrypTrack trackID="1" IsEncrypted="1" IV_size="8" first_IV="0x0001020304050607" '
        'crypt_byte_block="1" skip_byte_block="0" saiSavedBox="senc">'
    )
    track = {"cenc": iv8, "cbc1": iv16, "cens": cens, "cbcs": cbcs}[scheme]
    xml = work / f"gpac_{scheme}.xml"
    key = f'<key KID="0x{KID1}" value="0x{KEY1}"/>'
    xml.write_text(f'<GPACDRM type="{scheme}">{track}{key}</CrypTrack></GPACDRM>')
    out = work / f"{clear_mp4.stem}_gpac_{scheme}.mp4"
    done = subprocess.run(
        ["MP4Box", "-crypt", str(xml), str(clear_mp4), "-out", str(out)],
        capture_output=True,
        check=False,
    )
    return out if done.returncode == 0 and out.exists() else None


def decrypt_leg(
    subject: str, kind: str, protected: Path, clear: bytes, work: Path, root: str | None
) -> odk.Result:
    out = work / "dolby" / subject
    done = odk.streamdemuxer(protected, out, keys={KID1: KEY1}, root=root)
    files = [p for p in done.files if p.suffix in corpus.ES_SUFFIXES]
    text = done.text + done.err_text
    if files:
        if corpus.same_stream(kind, files[0].read_bytes(), clear):
            return odk.Result(subject, odk.PASS)
        return odk.Result(
            subject, odk.DISAGREE, "the tool wrote a stream that is not the clear one"
        )
    if NEEDS_KEY in text:
        return odk.Result(
            subject,
            odk.SKIP,
            "tool answered '" + NEEDS_KEY + "' although the matching key was given",
        )
    if done.returncode < 0:
        return odk.Result(subject, odk.SKIP, f"the tool crashed (signal {-done.returncode})")
    return odk.Result(subject, odk.SKIP, f"no stream written (exit {done.returncode})")


def run_checks(
    cli: str,
    work: Path,
    quick: bool,
    require_decrypt: bool,
    root: str | None,
    keep_legs: bool = False,
) -> list[odk.Result]:
    results: list[odk.Result] = []
    streams, notes = corpus.make_streams(cli, work / "streams")
    results += [odk.Result("corpus", odk.ERROR, note) for note in notes]
    clear: dict[str, tuple[Path, bytes]] = {}
    for stream in streams:
        mp4 = work / "clear" / f"{stream.name}.mp4"
        mp4.parent.mkdir(parents=True, exist_ok=True)
        demuxed = work / "clear" / f"{stream.name}.demux{corpus.SUFFIX_OF[stream.name]}"
        if (
            corpus.run([cli, "mp4", stream.path, mp4]).returncode == 0
            and corpus.run([cli, "demux", mp4, demuxed]).returncode == 0
        ):
            clear[stream.name] = (mp4, demuxed.read_bytes())
    protected: list[tuple[str, str, Path, Case | None]] = []
    for case in cases(quick):
        if case.codec not in clear:
            continue
        out = work / "protected" / f"{case.label}.mp4"
        out.parent.mkdir(parents=True, exist_ok=True)
        problem = make_protected(clear[case.codec][0], case, out)
        if problem:
            results.append(odk.Result(case.label, odk.ERROR, "generator failed: " + problem))
        else:
            protected.append((case.label, case.codec, out, case))
    for codec, (mp4, _) in clear.items():
        for scheme in SCHEMES:
            made = gpac_protected(mp4, scheme, work / "protected")
            if made is not None:
                protected.append((f"gpac_{codec}_{scheme}", codec, made, Case(codec, scheme)))
    for subject, codec, path, case in protected:
        try:
            boxes = odk.mp4drop_boxes(path, root=root)
        except (RuntimeError, ValueError) as error:
            results.append(odk.Result(subject + " boxes", odk.ERROR, str(error)))
            continue
        if case is not None and not subject.startswith("gpac_"):
            results.append(check_boxes(subject + " boxes", case, corpus.ENTRY_OF[codec], boxes))
        elif case is not None:
            found = find_protection(boxes)
            ok = {"enca", "frma", "schm", "tenc"} <= set(found) and found["schm"].value(
                "scheme_type"
            ) == case.scheme
            results.append(
                odk.Result(
                    subject + " boxes",
                    odk.PASS if ok else odk.DISAGREE,
                    "" if ok else "protection boxes not as GPAC was told",
                )
            )
        results.append(decrypt_leg(subject + " decrypt", codec, path, clear[codec][1], work, root))
    legs = [r for r in results if r.subject.endswith(" decrypt")]
    if legs and all(r.status == odk.SKIP for r in legs):
        reasons = {r.detail for r in legs}
        summary = f"every decrypt leg was skipped ({len(legs)}); reasons: {sorted(reasons)}"
        if not keep_legs:
            results = [r for r in results if r not in legs]
        results.append(
            odk.Result("decrypt legs", odk.ERROR if require_decrypt else odk.SKIP, summary)
        )
    return results


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--cli", default=corpus.default_cli(), help="forge binary (default $ICLFORGE_CLI)"
    )
    parser.add_argument("--quick", action="store_true", help="the core layout of each scheme only")
    parser.add_argument(
        "--require-decrypt", action="store_true", help="a tool that cannot decrypt is an error"
    )
    parser.add_argument("--work", help="working directory (default: a temporary one)")
    parser.add_argument(
        "--verbose", action="store_true", help="list every leg, not only the failures and skips"
    )
    args = parser.parse_args(argv)
    reason = odk.skip_reason("mp4drop", "streamdemuxer")
    if reason:
        print(f"skip: {reason}")
        return odk.SKIP_EXIT
    if not args.cli:
        parser.error("--cli or ICLFORGE_CLI is required")
    with tempfile.TemporaryDirectory(prefix="dolby-cenc-") as tmp:
        work = Path(args.work or tmp)
        results = run_checks(args.cli, work, args.quick, args.require_decrypt, None, args.verbose)
        return odk.print_report("Dolby tools against protected Dolby audio", results, args.verbose)


if __name__ == "__main__":
    sys.exit(main())
