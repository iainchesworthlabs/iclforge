# ADM bridge: errata and readings

Where a standard the bridge implements prints a defect or leaves a case open, and the reading taken.

## TS 103 420 V1.2.1, Annex B, Table B.19: `ZM3_SideRight` `minX`

The table gives the zone bounds that Table B.18 names for each zone constraint. Every left/right pair
in it is symmetric about X = 0 except one:

| Zone | minX | maxX |
|---|---|---|
| `ZM3_SideLeft` | -1 | -0.51611 |
| `ZM3_SideRight` | 0.5611 | 1 |

`0.5611` is read as a dropped digit for `0.51611`, the mirror of `ZM3_SideLeft`'s `maxX`. The
transcription is in `src/admbridge/src/coordinates.cpp` (`kZoneTable`).

- **Writing** uses `0.51611`.
- **Reading** recognises a zone by its label first, and by its bounds only when it has none. The bounds
  match within `kZoneBoundTolerance` (0.05), which accepts either spelling: the two differ by 0.045.

Evidence: **text**. No file or tool available here carries this table's values to compare against.

## TS 103 420 V1.2.1, Annex B.2.6: zones that are not a preset

Table B.18 lists the zone elements for six zone constraints. An ADM `zoneExclusion` can name any
cuboid, and OAMD can say only the six presets plus the Top-Bottom switch (`b_enable_elevation`), so
the annex gives no mapping for anything else. `adm_zone_exclusion_to_constraint()` maps the part that
matches a preset and reports the rest as inexact (`AdmZoneMapping::exact`), which `build()` surfaces in
`BridgeResult::unmapped`. In particular:

- `ZU` without `ZB`, or `ZB` without `ZU`, leaves elevation enabled. The switch is one bit.
- Two different horizontal presets at once map to no constraint.

## ADM `objectDivergence` and `screenRef`

TS 103 420 Annex B (the OAMD-to-ADM conversion) lists no mapping for either. OAMD's divergence
(§5.2.7, Tables 40 to 42) splits an object in two along X; ADM's is a value with an azimuth or position
range. `screenRef` is mapped in B.2.1.3 only together with an `audioProgrammeReferenceScreen` built from
`ref_screen_ratio`, which the decoded program model does not carry. The bridge reads both from ADM and
does not carry either into the Atmos encode, and `write()` does not emit them.
