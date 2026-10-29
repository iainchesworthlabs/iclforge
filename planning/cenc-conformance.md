# Common Encryption (ISO/IEC 23001-7): conformance audit

Working draft. This first commit holds the static inventory only (part A of the audit). The
measurements, the standard's requirements, the gap register and the verdict follow in later commits.

## Static inventory so far

Searching the whole tree for `cenc`, `cbcs`, `cens`, `cbc1`, `tenc`, `senc`, `saiz`, `saio`, `pssh`,
`seig`, `sinf`, `frma`, `schm`, `schi`, `enca`, `encv`, `ContentProtection`, `EXT-X-KEY` and the DRM
system names finds only IAMF code and its tests and docs:

- `libs/containers/src/iamf/container.cpp:681-702` reads `enca` > `sinf` > `frma` and notes a protected
  IAMF track; `:1207-1211` returns `kUnsupported` for a file whose IA tracks are all protected, with the
  comment "Common Encryption is not implemented".
- `docs/library/iamf.md:141` and `docs/library/development-status.md:256,390` say the same for IAMF.
- `ROADMAP.md` (Out of scope, AC-4 beyond the plan's scope) lists "protected (encrypted) tracks" for AC-4.
- The `key_id` hits in `libs/objects`, `libs/ac3` and `libs/ac4` are EMDF's authentication key id, not a
  CENC key identifier.

The MP4 reader (`libs/containers/src/mp4/reader.cpp`) selects a track only if its first matching sample
entry is `ac-3`, `ec-3` or `ac-4` (`:573`); an `enca` entry is not recognised (`:610`), so the track is
listed as skipped and the file ends in `kNoAudioTrack` (`:1041`).
