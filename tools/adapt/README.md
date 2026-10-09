# Adapting an open branch to the re-laid-out tree

The tree was re-laid out in stages (planning/layout.md, planning/consolidation.md,
planning/monorepo.md). The scripts that did it are retired (planning/monorepo.md, decision 13): they are
in the history at `8d2507bae`, the last commit that has `tools/n1b/`. What stays is what an open
branch that predates the moves needs.

## The move maps

Each stage moved its files in a commit of its own, before any text changed, every rename `R100`. The commit
is the map, so no map is kept as data: `moves.py <commit>` reads it back as `old<TAB>new`, or as
`{"moves": {...}}` with `--json`, and fails on a commit that is not renames alone.

| stage | commit | renames | what moved |
|---|---|---:|---|
| C1 | `bcf6c7f71` | 393 | the four AC-4 libraries into AC-3's layout, one library |
| C2 | `893f3c7bc` | 26 | arithmetic into base, admbridge into adm, signing split |
| C3 | `4fa28ebdd` | 45 | mp4, mpegts, matroska, iamf and iec61937 into containers |
| C4 names | `5a3dc0e81` | 66 | the AC-4 tests' files and helpers to names without a library's prefix |
| C4 | `9e42f3632` | 4 | AC-4's object metadata semantics and object rendering to oba |
| C5 | `971ec6e68` | 35 | AC-4's FFT, MDCT, KBD, QMF banks and tables into dsp/tiered |
| C6 | `895aaad7e` | 2 | the family's version template to base |
| C6 | `f83b18bac` | 6 | WAV reading and writing to base |
| C6 | `798d14636` | 4 | the level and loudness meters to base |
| M3 | `d3d61fb0f` | 2 | the complex type and the Stockham passes to dsp/detail |
| M3 | `2f04a1831` | 3 | base's arithmetic headers out of its installed include/ |
| C7-1 | `43ecd2401` | 1,199 | the libraries to libs/, tests and fuzz beside them, tests/support, external/ |
| C7-2 | `9aa9bfdfb` | 682 | the products to apps/, the notices to notices/ |
| C7-3 | `94c3bd8d6` | 290 | the bindings to bindings/, the firmware to firmware/ |
| C7-4 | `030af657f` | 201 | tests/golden to testdata/ |
| C7-5 | `8a0d74baf` | 1 | tools/checks/layering.json to projects.json |
| C7-7 | `ff2cf9fc7` | 12 | adapt_branch.ps1 and the scripts it runs to tools/adapt/ |

## Adapting a branch

A branch that changed files in the old places meets, when it merges the new tree, renames git detects
(every move is `R100`, so a file the branch edited follows its file) and lines the path passes rewrote
(a build file's paths, a page's links, a comment that names a directory). In this order:

1. Merge the tree before the stage into the branch, so that nothing the stage reverts is missing (never
   rebase a pushed branch).
2. Merge the stage's tip with directory renames on: `git -c merge.directoryRenames=true merge <tip>`. A
   file the branch added in a directory that moved goes to the new one. The conflicts that remain are
   lines the branch changed and a path pass changed too; resolve each to the new path.
3. For a file git could not follow (a rename with edits the stage's passes made, a moved directory
   spelt differently in every file), look the new path up with `moves.py <commit> | grep <old path>`.

`adapt_branch.ps1` is the older procedure, for a branch that predates stages S2 and S3 of
planning/layout.md: it runs those stages' own scripts (`n1b_apply.py`, `n1b_cmake.py`, `n1b_paths.py`,
`n1b_names.py`, here with the move map of S2 in `layoutdef.py`) on the branch, records their commits as
merged with `git merge -s ours`, and then merges the tip. The stages after S3 (S4, N1A, S5, S6, C0 to C7)
have no scripts here: their scripts are in the history, and a branch that predates them takes the merge
above.

The tests of what is here run in the static job (`python3 -m unittest discover -s tools/adapt`).
