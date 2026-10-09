#!/usr/bin/env python3
"""Do two versions of the CI planners choose the same jobs?

A change to tools/ci/classify_changes.py or plan_gate.py changes which jobs a pull request runs,
and a job that is wrongly skipped is skipped silently. This replays the two versions over the
changes the repository has seen and says where they differ, and in which direction:

  - every tracked file of the new tree, as a one-file change;
  - the last N pull requests merged to the base branch, each as its file list (the paths a past
    merge touched, followed to where they are now by git's renames from the merge's tree).

For each, both planners are asked everything the workflows ask: the gate's plan (with and without
the queue's `--gui-on-build`) and the lane classification (with and without `--satellites-direct`).

    compare_planners.py --old <worktree whose tools/ci is the old version> [--new .]
                        [--base github/main] [--prs 150] [--show 25]

A difference is a SUPERSET when the new answer is true wherever the old one was (it runs everything
the old one ran, and more), NARROWER when it is false somewhere the old one was true, and MIXED
when both. A superset costs minutes; a narrower answer is a claim that the old one ran a job for
nothing, and has to be argued path by path. The exit status is 1 when any answer is narrower or
mixed, so that a change that was meant to be a pure refactoring fails the first time it is not.
"""

from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


def load(root: Path, name: str):
    """A planner module of `root`, under a name of its own so two versions can be loaded."""
    tools_ci = str((root / "tools" / "ci").resolve())
    sys.path.insert(0, tools_ci)
    try:
        spec = importlib.util.spec_from_file_location(
            f"{name}_{abs(hash(str(root.resolve())))}", root / "tools" / "ci" / f"{name}.py"
        )
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(tools_ci)
    return module


def git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, check=True, text=True
    ).stdout


def answers(gate, classify, paths: list[str]) -> dict[str, dict[str, object]]:
    out: dict[str, dict[str, object]] = {}
    for gui_on_build in (False, True):
        plan = gate.plan(paths, gui_on_build=gui_on_build)
        # `reason` and `gui_reason` name the path itself: only whether they are set is compared.
        out[f"gate/gui_on_build={gui_on_build}"] = {
            k: (bool(v) if k in ("reason", "gui_reason") else v) for k, v in plan.items()
        }
    for direct in (False, True):
        out[f"lanes/satellites_direct={direct}"] = dict(
            classify.classify(paths, satellites_direct=direct)
        )
    return out


def truthy(value: object) -> bool:
    return value is True or value == "true"


def direction(old: dict, new: dict) -> str:
    more = less = False
    for key, o in old.items():
        for field, ov in o.items():
            nv = new[key][field]
            if field in ("reason", "gui_reason") or ov == nv:
                continue
            if field == "docs_only":  # true means less is built
                more, less = more or truthy(ov), less or truthy(nv)
            elif truthy(nv) and not truthy(ov):
                more = True
            elif truthy(ov) and not truthy(nv):
                less = True
    if more and less:
        return "MIXED"
    return "SUPERSET" if more else "NARROWER" if less else "SAME"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--old", required=True, help="a worktree holding the old tools/ci")
    ap.add_argument("--new", default=".", help="the worktree holding the new tools/ci")
    ap.add_argument("--base", default="github/main", help="the branch whose merges are replayed")
    ap.add_argument("--prs", type=int, default=150, help="how many merged pull requests")
    ap.add_argument("--show", type=int, default=25, help="how many differences to print")
    ap.add_argument(
        "--allow-narrower",
        action="append",
        default=[],
        metavar="PREFIX",
        help="a path prefix the new planner knows and the old one did not: a narrower answer\n"
        "for paths under it is EXPLAINED and does not fail the run (repeatable)",
    )
    a = ap.parse_args()
    old, new = Path(a.old), Path(a.new)
    og, oc = load(old, "plan_gate"), load(old, "classify_changes")
    ng, nc = load(new, "plan_gate"), load(new, "classify_changes")

    cases: list[tuple[str, list[str]]] = [(f, [f]) for f in git(new, "ls-files").splitlines() if f]
    n_files = len(cases)

    merges = git(
        new, "log", "--first-parent", "--merges", f"-{a.prs}", "--format=%H", a.base
    ).split()
    n_prs = 0
    for merge in merges:
        parent = git(new, "rev-parse", f"{merge}^1").strip()
        changed = git(new, "diff", "--name-only", parent, merge).splitlines()
        # Where the paths of that merge are now: git's renames from its tree to this one.
        moved: dict[str, str] = {}
        for line in git(new, "diff", "-M", "--name-status", merge, "HEAD").splitlines():
            parts = line.split("\t")
            if parts[0][0] == "R":
                moved[parts[1]] = parts[2]
        paths = []
        for f in changed:
            target = moved.get(f, f)
            if (new / target).exists():
                paths.append(target)
        if paths:
            n_prs += 1
            cases.append((f"PR {merge[:9]} ({len(paths)} files)", paths))

    allowed = tuple(a.allow_narrower)
    tally: dict[str, int] = defaultdict(int)
    examples: dict[str, list[tuple[str, dict, dict]]] = defaultdict(list)
    for label, paths in cases:
        o, n = answers(og, oc, paths), answers(ng, nc, paths)
        d = direction(o, n)
        if d in ("NARROWER", "MIXED") and allowed:
            # Narrower for the paths that were unknown to the old planner and are known now is
            # the point of a change that teaches the planner a tree; only those are excused.
            culprits = [
                p
                for p in paths
                if direction(answers(og, oc, [p]), answers(ng, nc, [p])) in ("NARROWER", "MIXED")
            ]
            if culprits and all(p.startswith(allowed) for p in culprits):
                d = "EXPLAINED"
        tally[d] += 1
        if d != "SAME":
            examples[d].append((label, o, n))

    print(f"{n_files} one-file changes and {n_prs} past pull requests compared")
    for d in ("SAME", "SUPERSET", "EXPLAINED", "NARROWER", "MIXED"):
        if tally[d]:
            note = f" (narrower only under {', '.join(allowed)})" if d == "EXPLAINED" else ""
            print(f"  {d}: {tally[d]}{note}")
    shown = 0
    for d in ("MIXED", "NARROWER", "SUPERSET", "EXPLAINED"):
        for label, o, n in examples[d]:
            if shown >= a.show:
                break
            shown += 1
            print(f"\n{d}: {label}")
            for key in o:
                for field, ov in o[key].items():
                    nv = n[key][field]
                    if ov != nv and field not in ("reason", "gui_reason"):
                        print(f"  {key} {field}: old {ov} -> new {nv}")
    return 1 if tally["NARROWER"] or tally["MIXED"] else 0


if __name__ == "__main__":
    sys.exit(main())
