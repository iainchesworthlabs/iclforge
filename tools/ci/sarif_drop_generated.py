"""Take the results that sit under a directory out of a SARIF file, in place.

msvc-analysis.yml runs it between the analysis and the upload, on the CMake
build directory. Qt's tools write C++ into that directory while it builds
(qmlcachegen's `.rcc/qmlcache/*_qml.cpp`, moc's `mocs_compilation.cpp`), the
analyzer compiles it with the rest of the target, and what it finds there is
not ours to fix: PREfast C6262 reported 17,696 and 39,200 bytes of stack in two
generated `Main_qml.cpp`, alerts 9750 and 9751. The action's `ignoredPaths`
leaves out whole CMake targets, and the generated files are in the same target
as the code beside them, so the way to leave them out is to leave their results
out. A result gone from the next upload closes its alert by itself, with no
dismissal to repeat when the file moves.

Only a result whose every location is under the directory goes: one with a
location in our source stays, whatever else it names.

usage: sarif_drop_generated.py FILE.sarif DIR [DIR ...]

A DIR is a path from the repository root with forward slashes
(`build/config-windows-msvc`); it matches the same segments in a relative URI
and in the absolute `file:///` ones the analyzer writes. Prints how many
results it dropped and from which directory, and exits 1 when FILE is not a SARIF
file it can read.
"""

import json
import sys
import urllib.parse


def _normal(uri):
    path = urllib.parse.unquote(urllib.parse.urlsplit(uri).path or uri)
    return "/" + path.replace("\\", "/").lstrip("/").lower()


def _under(uri, dirs):
    path = _normal(uri)
    return any(d in path for d in dirs)


def _uris(result):
    for location in result.get("locations", []):
        yield location.get("physicalLocation", {}).get("artifactLocation", {}).get("uri", "")


def drop(sarif, directories):
    """Remove the results that are only under `directories`; return how many went."""
    dirs = ["/" + d.replace("\\", "/").strip("/").lower() + "/" for d in directories]
    dropped = 0
    for run in sarif.get("runs", []):
        results = run.get("results", [])
        kept = []
        for result in results:
            uris = list(_uris(result))
            if uris and all(uri and _under(uri, dirs) for uri in uris):
                dropped += 1
            else:
                kept.append(result)
        run["results"] = kept
    return dropped


def main(argv):
    if len(argv) < 3:
        print(__doc__.split("usage: ")[1].split("\n")[0], file=sys.stderr)
        return 2
    path, directories = argv[1], argv[2:]
    try:
        with open(path, encoding="utf-8") as handle:
            sarif = json.load(handle)
    except (OSError, ValueError) as error:
        print(f"{path}: not a SARIF file: {error}", file=sys.stderr)
        return 1
    dropped = drop(sarif, directories)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(sarif, handle)
    print(f"{path}: dropped {dropped} result(s) under {', '.join(directories)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
