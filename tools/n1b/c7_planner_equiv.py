#!/usr/bin/env python3
"""Do the CI planners select the same jobs after a move as before it?

OLD is the tree before (its tools/ci planners), NEW the tree after. Every file tracked at OLD's
commit is a one-file change: the old planners get its old path, the new planners get the path it
moved to (git's renames between the commits and the working tree). Then, for a sample of past
commits, the files each changed that exist at OLD are a many-file change, in the same way. The
answers are compared key by key; `reason` and `gui_reason` name the path itself, so only whether
they are empty is compared.

    c7_planner_equiv.py --old <worktree> --old-rev <rev> --new <worktree> [--commits 80]
"""

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path


def load(root: Path, name: str):
    spec = importlib.util.spec_from_file_location(
        f"{name}_{abs(hash(str(root)))}", root / "tools" / "ci" / f"{name}.py"
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, check=True, text=True
    ).stdout


def answers(gate, classify, paths: list[str]) -> dict:
    out = {}
    for gui_on_build in (False, True):
        p = gate.plan(paths, gui_on_build=gui_on_build)
        out[f"gate/{gui_on_build}"] = {
            k: (bool(v) if k in ("reason", "gui_reason") else v) for k, v in p.items()
        }
    for direct in (False, True):
        out[f"lanes/{direct}"] = classify.classify(paths, satellites_direct=direct)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True)
    ap.add_argument("--old-rev", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--commits", type=int, default=80)
    a = ap.parse_args()
    old, new = Path(a.old), Path(a.new)
    og, oc = load(old, "plan_gate"), load(old, "classify_changes")
    ng, nc = load(new, "plan_gate"), load(new, "classify_changes")

    moved: dict[str, str] = {}
    for line in git(new, "diff", "-M", "--name-status", a.old_rev).splitlines():
        parts = line.split("\t")
        if parts[0][0] == "R":
            moved[parts[1]] = parts[2]
    tracked = [f for f in git(old, "ls-files").splitlines() if f]

    def to_new(p: str) -> str | None:
        if p in moved:
            return moved[p]
        return p if (new / p).exists() else None

    bad = []
    n = 0
    for p in tracked:
        q = to_new(p)
        if q is None:
            continue
        n += 1
        if answers(og, oc, [p]) != answers(ng, nc, [q]):
            bad.append(([p], [q]))
    print(f"one-file changes: {n} files compared, {len([1 for b in bad if len(b[0]) == 1])} differ")

    commits = git(old, "log", f"-{a.commits}", "--no-merges", "--format=%H", a.old_rev).split()
    m = 0
    many = 0
    for c in commits:
        files = [f for f in git(old, "show", "--name-only", "--format=", c).splitlines() if f]
        mapped = [(f, to_new(f)) for f in files]
        keep = [(f, q) for f, q in mapped if q is not None]
        if not keep:
            continue
        m += 1
        ps, qs = [f for f, _ in keep], [q for _, q in keep]
        if answers(og, oc, ps) != answers(ng, nc, qs):
            many += 1
            bad.append((ps, qs))
    print(f"past commits: {m} compared (last {a.commits} on {a.old_rev[:9]}), {many} differ")

    for ps, qs in bad[:30]:
        ao, an = answers(og, oc, ps), answers(ng, nc, qs)
        print(f"\nDIFFERENT for {ps[:3]} -> {qs[:3]}")
        for k in ao:
            if ao[k] != an[k]:
                print(f"  {k}: old {ao[k]}\n  {' ' * len(k)}  new {an[k]}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
