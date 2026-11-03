#!/usr/bin/env python3
"""Does every libFuzzer harness have a build rule, seeds and a place in the run list?

A harness that is built and run by nothing finds nothing, and one that starts from an empty corpus
spends its first minutes on bytes its parser rejects at the first check. Three of the TrueHD
harnesses were once in exactly that state, so this reads the tree and fails on:

  * a libs/*/fuzz/fuzz_*.cpp that its library's fuzz/CMakeLists.txt does not register;
  * a harness that is in no run list: not in `BASE_TARGETS` of tools/fuzz/run.sh (which
    fuzz-regress, fuzz-short and fuzz-nightly run), not named by the `run.sh run` and `run.sh
    regress` lines of .github/workflows/fuzz.yml that run the differential harnesses twice (the
    fuzz-differential and fuzz-nightly jobs), and not an exception below;
  * a harness with no seeds: no file in seeds/<harness>/ or, where run.sh's `seed_source_for`
    names another harness as the source (a decoder that shares its parser's seeds, a differential
    harness that shares its sibling's), in that one's;
  * a dictionary (dictionaries/<harness>.dict) of no harness, or one that is not in libFuzzer's
    format (a line that is neither a comment nor `name="value"`);
  * an exception that no longer applies.

stdlib only, no build, well under a second.

    python tools/checks/check_fuzz_harnesses.py
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]

# Harnesses that are not in BASE_TARGETS, and why. An entry that is wrong fails the check, so that
# a gap recorded here is a gap somebody reads again.
NOT_IN_BASE_TARGETS = {
    "fuzz_adm_parse": (
        "opt-in: it needs vcpkg's adm feature and network access, so fuzz-adm-nightly runs it "
        "(run.sh adds it to the list under ICLFORGE_FUZZ_ADM)"
    ),
}
# Harnesses with a gap that is known and recorded, so that the check passes while the gap stands:
# harness -> what is missing. Each is reported on every run.
KNOWN_GAPS: dict[str, str] = {}
DIFFERENTIAL_PREFIX = "fuzz_differential_"
# The jobs of fuzz.yml that run every differential harness, each with a replay and a mutation step.
DIFFERENTIAL_JOBS = 2

DICT_LINE = re.compile(r'^\s*(#.*|([A-Za-z0-9_]+=)?"([^"\\]|\\x[0-9A-Fa-f]{2}|\\\\|\\")*")?\s*$')


def harnesses(repo: pathlib.Path) -> dict[str, pathlib.Path]:
    """Every harness source of the tree: name -> the fuzz/ directory that holds it."""
    return {path.stem: path.parent for path in sorted(repo.glob("libs/*/fuzz/fuzz_*.cpp"))}


def base_targets(run_sh: str) -> list[str]:
    match = re.search(r"readonly BASE_TARGETS=\((.*?)\)", run_sh, re.DOTALL)
    return match.group(1).split() if match else []


def seed_sources(run_sh: str) -> dict[str, str]:
    """run.sh's seed_source_for: the harness whose seeds another one reads."""
    body = re.search(r"seed_source_for\(\) \{(.*?)\n\}", run_sh, re.DOTALL)
    if not body:
        return {}
    return dict(re.findall(r"^\s*(fuzz_\w+)\)\s+echo\s+(fuzz_\w+)\s*;;", body.group(1), re.M))


def workflow_targets(workflow: str) -> dict[str, list[set[str]]]:
    """The harnesses each `tools/fuzz/run.sh run` and `regress` line of fuzz.yml names."""
    found: dict[str, list[set[str]]] = {"run": [], "regress": []}
    for line in workflow.splitlines():
        match = re.search(r"\brun\.sh\s+(run|regress)\s+(.*)$", line)
        if match:
            found[match.group(1)].append(set(re.findall(r"fuzz_\w+", match.group(2))))
    return found


def registered(directory: pathlib.Path) -> set[str]:
    cmake = directory / "CMakeLists.txt"
    if not cmake.is_file():
        return set()
    return set(
        re.findall(
            r"iclforge_add(?:_container)?_fuzzer\(\s*(fuzz_\w+)", cmake.read_text(encoding="utf-8")
        )
    )


def seed_count(directory: pathlib.Path, name: str) -> int:
    seeds = directory / "seeds" / name
    return sum(1 for path in seeds.rglob("*") if path.is_file()) if seeds.is_dir() else 0


def problems_in(
    repo: pathlib.Path,
    exceptions: dict[str, str] = NOT_IN_BASE_TARGETS,
    known_gaps: dict[str, str] = KNOWN_GAPS,
) -> tuple[list[str], list[str]]:
    """(problems, notices): what fails the check, and the known gaps it reports."""
    run_sh = (repo / "tools" / "fuzz" / "run.sh").read_text(encoding="utf-8")
    workflow = (repo / ".github" / "workflows" / "fuzz.yml").read_text(encoding="utf-8")
    found = harnesses(repo)
    listed = set(base_targets(run_sh))
    sources = seed_sources(run_sh)
    named = workflow_targets(workflow)
    problems: list[str] = []
    notices: list[str] = []

    for name in sorted(listed - found.keys()):
        problems.append(f"run.sh lists {name}, which no libs/*/fuzz/ holds")
    for name, directory in found.items():
        where = directory.relative_to(repo).as_posix()
        if name not in registered(directory):
            problems.append(f"{name}: {where}/CMakeLists.txt does not register it")
        gap = known_gaps.get(name)
        if gap:
            notices.append(f"{name}: {gap}")
        differential = name.startswith(DIFFERENTIAL_PREFIX)
        if name in listed:
            if name in exceptions or differential:
                problems.append(f"{name} is in BASE_TARGETS and is meant not to be")
        elif differential:
            for kind in ("run", "regress"):
                lines = sum(1 for targets in named[kind] if name in targets)
                if lines < DIFFERENTIAL_JOBS:
                    problems.append(
                        f"{name} is in {lines} `run.sh {kind}` line(s) of fuzz.yml, "
                        f"not the {DIFFERENTIAL_JOBS} (fuzz-differential, fuzz-nightly)"
                    )
        elif name in exceptions:
            if not any(name in targets for targets in named["run"]):
                problems.append(f"{name}: {exceptions[name]}, but fuzz.yml never runs it")
        elif not gap:
            problems.append(f"{name} is in no run list (BASE_TARGETS of tools/fuzz/run.sh)")

        source = sources.get(name, name)
        if source not in found:
            problems.append(f"{name}: run.sh takes its seeds from {source}, which is no harness")
        elif not gap and seed_count(found[source], source) == 0:
            problems.append(
                f"{name} has no seeds ({found[source].relative_to(repo)}/seeds/{source}/)"
            )

        dictionary = directory / "dictionaries" / f"{name}.dict"
        if dictionary.is_file():
            for number, text in enumerate(dictionary.read_text(encoding="utf-8").splitlines(), 1):
                if not DICT_LINE.match(text):
                    problems.append(
                        f"{dictionary.relative_to(repo)}:{number}: not libFuzzer syntax"
                    )

    for directory in set(found.values()):
        for dictionary in sorted((directory / "dictionaries").glob("*.dict")):
            if dictionary.stem not in found:
                problems.append(f"{dictionary.relative_to(repo)} belongs to no harness")
    for name in sorted(exceptions.keys() - found.keys()):
        problems.append(f"the exception for {name} is stale: no such harness")
    for name in sorted(known_gaps.keys() - found.keys()):
        problems.append(f"the known gap of {name} is stale: no such harness")
    for name in sorted(known_gaps.keys() & found.keys()):
        if name in listed and seed_count(found[name], name) > 0:
            problems.append(f"the known gap of {name} is closed: remove it from KNOWN_GAPS")
    return problems, notices


def main() -> int:
    problems, notices = problems_in(REPO)
    for notice in notices:
        print(f"::notice::check_fuzz_harnesses: known gap: {notice}")
    for problem in problems:
        print(f"::error::check_fuzz_harnesses: {problem}")
    print(
        f"check_fuzz_harnesses: {len(harnesses(REPO))} harnesses, "
        f"{len(notices)} known gap(s), {len(problems)} problem(s)"
    )
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
