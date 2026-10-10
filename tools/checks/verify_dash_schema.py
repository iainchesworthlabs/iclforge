"""Hold the MPDs `forge fmp4` writes against ISO/IEC 23009-1's own XML schema.

Until this check the DASH side of `forge fmp4` was read back through FFmpeg's
`dash` demuxer and nothing else, and FFmpeg's demuxer takes the elements it
needs and ignores the rest, so a manifest could be schema-invalid and still
play. This validates the whole document: element order and nesting, attribute
types and which attributes are required, the URI and duration patterns - the
parts of a manifest a strict consumer (a packager, a CDN's manifest rewriter,
DASH-IF's own validator) rejects on.

What it does NOT establish is what the Dolby descriptors mean. The schema
leaves a descriptor's scheme_id_uri and value as free strings, so
`EC3_ExtensionType`, `EC3_ExtensionComplexityIndex` and the
`audio_channel_configuration` value are checked here only for being well-formed
descriptors in the right place. Reading them the way a JOC-aware player does
needs that player, which docs/verification.md records as not available.

The schema set is fetched, never committed: ISO publishes it under the ISO/IEC
Directives rather than a software licence, and the pin below is what makes a
silent upstream change fail the run instead of moving the answer.

    DASH-MPD.xsd, DASH-MPD-UP.xsd   github.com/MPEGGroup/DASHSchema at a commit
    xlink.xsd, xml.xsd              W3C, which both of the above import

Two of those import a W3C schema by absolute URL; the copies in the cache have
that location rewritten to the sibling file so validation never touches the
network. The SHA-256 pins are of the files as fetched, before that rewrite.

Streams come from `forge` itself - an AC-3 5.1, an E-AC-3 stereo, a Dolby Atmos
JOC stream with and without the 5.1 fallback rendition, and an AC-4 from the
committed external baseline - so the manifests are the ones the CLI writes today.

Usage:
    ICLFORGE_CLI=build/config-linux-llvm/bin/forge python3 tools/checks/verify_dash_schema.py

    --cli PATH         forge, overriding $ICLFORGE_CLI
    --cache-dir DIR    where the schema set is kept (default $DASH_SCHEMA_CACHE_DIR,
                       else a temporary directory removed on exit)
    --mpd FILE...      also validate these manifests
    --only-mpd         validate just --mpd files; generate nothing (no --cli needed)
    --validator NAME   xmllint | dotnet (default: whichever is available, xmllint first)
    --require          fail rather than skip when no validator is available
    --list             print the pinned schema set and exit, fetching nothing

Exits non-zero on the first manifest the schema rejects, and when the validator
accepts a manifest it was meant to reject (a validator that cannot fail proves
nothing, so one corrupted copy is checked every run).
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
POWERSHELL_SCRIPT = Path(__file__).resolve().parent / "validate_mpd.ps1"

# MPEGGroup/DASHSchema, branch 6th-Ed, the commit current on 2026-10-10.
MPEG_COMMIT = "cc941bd4e2e4fb3ea6c7b1c37d3d5edf2ef3fbfe"
MPEG_URL = "https://raw.githubusercontent.com/MPEGGroup/DASHSchema/" + MPEG_COMMIT + "/"

PINS = (
    ("DASH-MPD.xsd", MPEG_URL + "DASH-MPD.xsd",
     "d2cb1b300c6f45e902d129bdaf13496dca21aca879ae2b996d4886e9a2513466"),
    ("DASH-MPD-UP.xsd", MPEG_URL + "DASH-MPD-UP.xsd",
     "7a3c7820b6ead75abfe338d5cdf151a1e4cc1fa8ee4a6d43ad7d71e6605477ea"),
    ("xlink.xsd", "https://www.w3.org/1999/xlink.xsd",
     "c3dbbaa28b884377ecd4a6c49d1f12566d8227cb0dce8ced10c0f66f8fa266e3"),
    ("xml.xsd", "https://www.w3.org/2001/xml.xsd",
     "61960fb3131e38022caad5360e2f33a3382578ab3c80cd58bd74320ede61b20c"),
)

# Where the schemas name another schema by absolute URL, the local sibling.
LOCATION_REWRITES = (
    ("DASH-MPD-UP.xsd", "http://www.w3.org/XML/2008/06/xlink.xsd", "xlink.xsd"),
    ("xlink.xsd", "http://www.w3.org/2001/xml.xsd", "xml.xsd"),
)

EXTERNAL_BASELINE = REPO / "testdata" / "external-baseline"


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch_schemas(cache: Path) -> Path:
    """Fetch every pinned file into cache/original, rewrite into cache/schema."""
    original = cache / "original"
    schema = cache / "schema"
    original.mkdir(parents=True, exist_ok=True)
    schema.mkdir(parents=True, exist_ok=True)
    for name, url, pin in PINS:
        target = original / name
        if not target.exists() or sha256_of(target) != pin:
            with urllib.request.urlopen(url, timeout=60) as response:  # noqa: S310 - fixed https URLs
                target.write_bytes(response.read())
        got = sha256_of(target)
        if got != pin:
            sys.exit(f"{name}: SHA-256 {got} does not match the pin {pin}\n  from {url}")
        text = target.read_text(encoding="utf-8")
        for owner, old, new in LOCATION_REWRITES:
            if owner == name:
                if old not in text:
                    sys.exit(f"{name}: expected an import of {old}")
                text = text.replace(old, new)
        (schema / name).write_text(text, encoding="utf-8", newline="")
    return schema


def find_validator(choice: str | None) -> str | None:
    if choice in (None, "xmllint") and shutil.which("xmllint"):
        return "xmllint"
    if choice in (None, "dotnet") and powershell() is not None:
        return "dotnet"
    return None


def powershell() -> str | None:
    for name in ("pwsh", "powershell"):
        found = shutil.which(name)
        if found:
            return found
    return None


def validate(backend: str, schema_dir: Path, mpds: list[Path]) -> dict[Path, str]:
    """Return {mpd: "" if valid else the validator's complaint}."""
    results: dict[Path, str] = {}
    if backend == "xmllint":
        for mpd in mpds:
            run = subprocess.run(
                ["xmllint", "--noout", "--schema", str(schema_dir / "DASH-MPD.xsd"), str(mpd)],
                capture_output=True, text=True, check=False)
            results[mpd] = "" if run.returncode == 0 else (run.stderr or run.stdout).strip()
        return results
    with tempfile.TemporaryDirectory() as scratch:
        listing = Path(scratch) / "mpds.txt"
        listing.write_text("\n".join(str(m) for m in mpds) + "\n", encoding="utf-8")
        run = subprocess.run(
            [powershell(), "-NoProfile", "-NonInteractive", "-File", str(POWERSHELL_SCRIPT),
             "-SchemaDir", str(schema_dir), "-MpdList", str(listing)],
            capture_output=True, text=True, check=False)
    current: Path | None = None
    for line in run.stdout.splitlines():
        if line.startswith("VALID   "):
            current = Path(line[len("VALID   "):].strip())
            results[current] = ""
        elif line.startswith("INVALID "):
            current = Path(line[len("INVALID "):].strip())
            results[current] = ""
        elif current is not None and line.startswith("    "):
            results[current] += line.strip() + "\n"
    if not results and run.returncode != 0:
        sys.exit("the .NET validator produced no verdicts:\n" + run.stdout + run.stderr)
    return results


def forge(cli: str, *args: str) -> None:
    run = subprocess.run([cli, *args], capture_output=True, text=True, check=False)
    if run.returncode != 0:
        sys.exit(f"{cli} {' '.join(args)} exited {run.returncode}:\n{run.stdout}{run.stderr}")


def generate(cli: str, work: Path) -> list[Path]:
    """Write the manifests `forge fmp4` produces for the shapes it signals."""
    sources = {
        "ac3-51": ("sine", ["{src}.ac3", "1", "448", "440", "60", "51"], ".ac3", []),
        "eac3-stereo": ("eac3-sine", ["{src}.ec3", "1", "192", "440", "50", "stereo"], ".ec3", []),
        "atmos-joc": ("atmos", ["{src}.ec3", "1", "448", "2", "4", "objects"], ".ec3", []),
        "atmos-joc-fallback": ("atmos", ["{src}.ec3", "1", "448", "2", "4", "objects"], ".ec3",
                               ["fallback-51"]),
    }
    manifests: list[Path] = []
    for label, (command, template, suffix, extra) in sources.items():
        source = work / (label + suffix)
        forge(cli, command, *[a.replace("{src}" + suffix, str(source)) for a in template])
        out_dir = work / (label + "-out")
        forge(cli, "fmp4", str(source), str(out_dir), "48", *extra)
        manifests.append(out_dir / "manifest.mpd")

    ac4 = EXTERNAL_BASELINE / "ac4-20-tones-192" / "dee.ac4"
    if ac4.exists():
        out_dir = work / "ac4-out"
        forge(cli, "fmp4", str(ac4), str(out_dir), "48")
        manifests.append(out_dir / "manifest.mpd")
    return manifests


def corrupted_copy(mpd: Path, target: Path) -> Path:
    text = mpd.read_text(encoding="utf-8")
    broken, count = re.subn(r'bandwidth="[0-9]+"', 'bandwidth="fast"', text, count=1)
    if count != 1:
        sys.exit(f"{mpd}: no bandwidth attribute to corrupt for the negative control")
    target.write_text(broken, encoding="utf-8")
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--cli", default=os.environ.get("ICLFORGE_CLI"))
    parser.add_argument("--cache-dir", default=os.environ.get("DASH_SCHEMA_CACHE_DIR"))
    parser.add_argument("--mpd", nargs="*", default=[])
    parser.add_argument("--only-mpd", action="store_true")
    parser.add_argument("--validator", choices=("xmllint", "dotnet"))
    parser.add_argument("--require", action="store_true")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    if args.list:
        for name, url, pin in PINS:
            print(f"{pin}  {name}\n    {url}")
        return 0

    backend = find_validator(args.validator)
    if backend is None:
        message = "no XSD validator: install libxml2-utils (xmllint) or run where PowerShell is"
        if args.require:
            print("FAIL " + message)
            return 1
        print("SKIP " + message)
        return 0

    with tempfile.TemporaryDirectory() as temporary:
        cache = Path(args.cache_dir) if args.cache_dir else Path(temporary) / "schema-cache"
        work = Path(temporary) / "work"
        work.mkdir()
        schema_dir = fetch_schemas(cache)

        manifests = [Path(m).resolve() for m in args.mpd]
        if not args.only_mpd:
            if not args.cli:
                print("error: --cli or $ICLFORGE_CLI names the forge binary", file=sys.stderr)
                return 2
            manifests = generate(str(Path(args.cli).resolve()), work) + manifests
        if not manifests:
            print("error: no manifests to validate", file=sys.stderr)
            return 2

        # The control goes through the same validator in the same call as the
        # real manifests, so a validator that quietly accepts everything is
        # caught by the one file it must reject.
        control = corrupted_copy(manifests[0], work / "negative-control.mpd")
        verdicts = validate(backend, schema_dir, [*manifests, control])

        failed = 0
        for mpd in manifests:
            complaint = verdicts.get(mpd)
            if complaint is None:
                print(f"NO VERDICT {mpd}")
                failed += 1
            elif complaint:
                print(f"INVALID {mpd}\n    " + complaint.strip().replace("\n", "\n    "))
                failed += 1
            else:
                print(f"valid   {mpd.parent.name}/{mpd.name}")
        if not verdicts.get(control):
            print(f"the validator ({backend}) accepted a manifest with bandwidth=\"fast\"; "
                  "it cannot be trusted")
            failed += 1
        else:
            print(f"control rejected as it must be ({backend})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
