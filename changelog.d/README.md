# Changelog fragments

A pull request does not edit `CHANGELOG.md`. It adds one file here for each change a person using
the software would notice, named `<slug>.<type>.md`, where `<type>` is `added`, `changed`,
`deprecated`, `removed`, `fixed` or `security`. The content is the entry: a **bold lead-in** that
says what changed, then the detail, wrapped as you like.

```markdown
**`forge qc` reads a TrueHD stream.** It was refused by name. The integrity gate counts ...
```

- **Slug.** Say what changed, in lowercase words and hyphens (`truehd-qc.added.md`). Two pull
  requests that pick the same slug conflict on the file, so be specific. A slug is not reused for
  a later change in the same release.
- **No type word in the lead-in.** The release puts the entry under `### Added`, `### Fixed` and
  so on; do not begin with "Added:".
- **More than one entry** in a pull request is more than one file.
- **A change nobody would notice** (tests, tooling, documentation, CI) needs no fragment.
- **Never edit `CHANGELOG.md`.** The static job of `PR Gate` fails a pull request that does,
  because two pull requests appending at the top of `## [Unreleased]` collide, and a merge to fix
  the collision starts CI again. The one pull request that edits it is the release's.

`python3 tools/release/changelog_tool.py pending` prints the entries as the release will write them.
`python3 tools/release/changelog_tool.py assemble --version X --date YYYY-MM-DD` is the release's
step ([Releasing](../docs/releasing.md)): it writes the section and deletes the fragments.

## A branch that already edited `CHANGELOG.md`

```bash
git merge github/main        # CHANGELOG.md may conflict: take either side, extract reads the branch's own diff
python3 tools/release/changelog_tool.py extract --base github/main            # show what it would write
python3 tools/release/changelog_tool.py extract --base github/main --restore  # write, and restore CHANGELOG.md
git add changelog.d CHANGELOG.md && git commit
```

`extract` finds what your branch added to `CHANGELOG.md` since it left `main`, turns each entry into
a fragment (the type word at the start of the lead-in decides the type and is dropped from it), and
`--restore` puts `CHANGELOG.md` back to the merge base so the branch no longer touches it. An entry
whose lead-in names no type needs `--default-type added` (or another type).
