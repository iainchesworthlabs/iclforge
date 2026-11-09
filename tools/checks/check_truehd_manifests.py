#!/usr/bin/env python3
"""Does every committed TrueHD and MLP stream have a manifest entry, and every entry its stream?

The decoder's tests, the gold-reference gate, the codec matrix and the interop script all find the
streams of testdata/truehd through its manifest.json, and the hand-built streams of
testdata/truehd/handbuilt through that directory's own. A stream added without an entry is read by
none of them (tools/checks/check_truehd_coverage.py counts it, which is not the same as holding it
to FFmpeg's decode), and an entry whose stream was regenerated without the manifest is caught
only by the legs that run an oracle. This reads the directories and the manifests and fails on:

  * a .thd or .mlp file no entry names (one stream can have several entries: the hand-built
    sixteen-channel stream is decoded in two presentations);
  * an entry whose file is not there;
  * a file whose SHA-256 is not the entry's stream_sha256.

The manifests' other fields (the PCM each stream decodes to) are held by the legs that decode
them. stdlib only, no build, well under a second.

    python tools/checks/check_truehd_manifests.py
"""

import hashlib
import json
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = REPO / "testdata" / "truehd"

# Each manifest's lists of entries.
SECTIONS = {
    "manifest.json": ("fixtures", "spliced"),
    "handbuilt/manifest.json": ("fixtures", "refused", "supplied"),
}


def problems_in(directory: pathlib.Path, manifest_name: str, sections) -> list[str]:
    """The problems of one manifest against the directory it sits in."""
    manifest_path = directory / manifest_name
    base = manifest_path.parent
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    problems = []
    named: set[str] = set()
    for section in sections:
        for entry in manifest.get(section, []):
            name = entry["file"]
            named.add(name)
            path = base / name
            if not path.is_file():
                problems.append(f"{manifest_name}: {section} entry {name} has no file")
            elif hashlib.sha256(path.read_bytes()).hexdigest() != entry["stream_sha256"]:
                problems.append(f"{manifest_name}: {name} is not the stream its entry names")
    present = {path.name for path in [*base.glob("*.thd"), *base.glob("*.mlp")]}
    for name in sorted(present - named):
        problems.append(f"{manifest_name}: {name} is in the directory and in no entry")
    return problems


def main() -> int:
    problems = []
    checked = 0
    for manifest_name, sections in SECTIONS.items():
        problems += problems_in(FIXTURES, manifest_name, sections)
        manifest = json.loads((FIXTURES / manifest_name).read_text(encoding="utf-8"))
        checked += sum(len(manifest.get(section, [])) for section in sections)
    for problem in problems:
        print(f"::error::check_truehd_manifests: {problem}")
    print(f"check_truehd_manifests: {checked} streams, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
