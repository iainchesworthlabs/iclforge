"""The moves and include spellings of a consolidation stage (planning/consolidation.md, C1 to C3).

    consol_apply.py --root <worktree> --stage c1|c2|c3 --phase plan|all [--json <plan file>]

n1b_apply.py's executor, pointed at consoldef.py's moves instead of layoutdef.py's L2. `all` stages
the renames (`git mv`) and edits the includes in the working tree, so that a `git commit` with
nothing added is the commit of the renames alone; the plan is resolved on the tree as it was before
the moves, so a second run on the finished tree finds nothing to do. `--json` writes the plan for
n1b_cmake.py and n1b_paths.py, which read its moves.

What it adds to n1b_apply.py's rewrite:

- A private header that moves is spelled from its library's private root, `src/<library>/src`
  (`"decoder/syntax/asf.hpp"`, `"core/dsp/mdct.hpp"`), where the old spelling went through an
  include directory of a library that is gone; an include that sits beside its header and moves
  with it keeps its text.
- The export header and macro of a library that merges follow the library the file is in after the
  stage (`iclforge/ac4dec/export.hpp` and ICLFORGE_AC4DEC_EXPORT become `iclforge/ac4/export.hpp`
  and ICLFORGE_AC4_EXPORT; a file of the signing library that moves to base takes base's), or, for
  a file outside the merged libraries, the library consoldef.LIBRARY_MAP names.
"""

from __future__ import annotations

import json
import posixpath
import re
from collections import Counter, defaultdict
from pathlib import Path

import consoldef
import layoutdef
from include_graph import build_index, resolve
from n1b_apply import do_moves, rewrite_docs_and_scripts
from n1b_lib import CPP_EXT, INCLUDE_RE, Repo, base_parser


def library_of(path: str) -> str | None:
    p = path.split("/")
    if p[0] == "src" and len(p) > 2:
        return p[1]
    return None


def private_spelling(path: str) -> str | None:
    """The spelling of a private header from its library's private root, `src/<library>/src/`."""
    m = re.match(r"^src/[^/]+/src/(.+)$", path)
    return m.group(1) if m else None


def new_spelling(path: str) -> str | None:
    return layoutdef.spelling_of(path) or private_spelling(path)


def export_target(stage: str, old_lib: str, file_new: str) -> str:
    """The library whose export header and macro a file uses after the stage, for `old_lib`'s."""
    lib = library_of(file_new)
    targets = set(consoldef.LIBRARY_MAP[stage].values())
    if lib in targets:
        return lib
    return consoldef.LIBRARY_MAP[stage][old_lib]


def plan(repo: Repo, stage: str, index) -> dict:
    moves = consoldef.moves(stage, repo.files)
    merged = consoldef.LIBRARY_MAP[stage]
    export_spellings = {f"iclforge/{old}/export.hpp": old for old in merged if old != merged[old]}
    edits: dict[str, dict[str, str]] = defaultdict(dict)
    macros: dict[str, dict[str, str]] = defaultdict(dict)
    stats: Counter = Counter()
    problems = []
    global_map: dict[str, str] = {}
    for old, new in moves.items():
        so, sn = layoutdef.spelling_of(old), layoutdef.spelling_of(new)
        if so and sn and so != sn:
            global_map[so] = sn
    for f in repo.files:
        if repo.ext(f) not in CPP_EXT and not f.endswith((".hpp.in", ".h.in")):
            continue
        text = repo.read(f)
        f_new = moves.get(f, f)
        own_lib = library_of(f) or ""
        for m in INCLUDE_RE.finditer(text):
            sp = m.group(2).strip()
            if sp in edits[f]:
                continue
            if sp in export_spellings:
                lib = export_target(stage, export_spellings[sp], f_new)
                edits[f][sp] = f"iclforge/{lib}/export.hpp"
                stats["export header"] += 1
                continue
            _owner, target, _public = resolve(repo, index, f, sp, own_lib)
            if target is None:
                continue
            t_new = moves.get(target, target)
            if t_new == target and f_new == f:
                continue
            here = posixpath.normpath(posixpath.join(posixpath.dirname(f), sp))
            still = posixpath.normpath(posixpath.join(posixpath.dirname(f_new), sp))
            if here == target and still == t_new:
                continue  # beside its header before and after
            sn = new_spelling(t_new)
            if layoutdef.spelling_of(t_new):
                if sn != sp:
                    edits[f][sp] = sn
                    stats["public"] += 1
                continue
            if t_new == target and here != target:
                continue  # an unmoved private header reached through an include directory
            if sn is None:
                problems.append((f, sp, target, t_new))
                continue
            if library_of(f_new) not in (None, library_of(t_new)):
                problems.append((f, sp, target, t_new))
            if sn != sp:
                edits[f][sp] = sn
                stats["private"] += 1
        for old in merged:
            if old == merged[old]:
                continue
            mac = f"ICLFORGE_{old.upper()}_EXPORT"
            if re.search(rf"\b{mac}\b", text):
                lib = export_target(stage, old, f_new)
                macros[f][mac] = f"ICLFORGE_{lib.upper()}_EXPORT"
                stats["export macro"] += 1
    return {
        "stage": stage,
        "moves": moves,
        "removed": list(consoldef.REMOVED[stage]),
        "edits": {k: v for k, v in edits.items() if v},
        "macros": {k: v for k, v in macros.items() if v},
        "problems": problems,
        "spellings": global_map,
        "stats": dict(stats),
    }


def apply_edits(root: Path, made: dict) -> int:
    moves, edits, macros = made["moves"], made["edits"], made["macros"]
    changed = 0
    for old in sorted(set(edits) | set(macros)):
        path = root / moves.get(old, old)
        text = path.read_bytes().decode("utf-8")
        rewrites = edits.get(old, {})

        def sub(mo, rewrites=rewrites):
            sp = mo.group(2).strip()
            new = rewrites.get(sp)
            return mo.group(0).replace(mo.group(2), new) if new else mo.group(0)

        out = INCLUDE_RE.sub(sub, text)
        for mac, new in macros.get(old, {}).items():
            out = re.sub(rf"\b{mac}\b", new, out)
        if out != text:
            path.write_bytes(out.encode("utf-8"))
            changed += 1
    return changed


def run(root: Path, stage: str, phase: str, json_path: str | None) -> dict:
    repo = Repo(str(root))
    index, _ = build_index(repo)
    made = plan(repo, stage, index)
    by_area = Counter(old.split("/")[0] for old in made["moves"])
    print(
        f"{stage}: {len(made['moves'])} moves ({', '.join(f'{k} {v}' for k, v in sorted(by_area.items()))}); "
        f"{sum(len(v) for v in made['edits'].values())} include rewrites in {len(made['edits'])} files, "
        f"export macros in {len(made['macros'])}; kinds {made['stats']}; "
        f"{len(made['problems'])} includes of a moved private header from outside its library"
    )
    for f, sp, t, tn in made["problems"][:40]:
        print("  PROBLEM", f, "includes", repr(sp), "->", t, "now", tn)
    if json_path:
        Path(json_path).write_text(json.dumps(made, indent=1), encoding="utf-8")
    if phase == "plan":
        return made
    do_moves(root, made["moves"])
    n = apply_edits(root, made)
    d = rewrite_docs_and_scripts(root, repo, made["moves"], made["spellings"])
    print(f"rewrote includes and macros in {n} C/C++ files and includes in {d} other text files")
    return made


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--stage", required=True, choices=sorted(consoldef.STAGES))
    ap.add_argument("--phase", choices=["plan", "all"], default="plan")
    ap.add_argument("--json", default=None, help="write the plan to this file")
    a = ap.parse_args()
    run(Path(a.root), a.stage, a.phase, a.json)


if __name__ == "__main__":
    main()
