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
transcription is in `libs/adm/src/coordinates.cpp` (`kZoneTable`).

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

TS 103 420 Annex B (the OAMD-to-ADM conversion) has no row for either.

**Divergence.** OAMD's `object_divergence` (§5.2.7, Tables 40 to 42) "converts one object into two
objects, where the energy is spread along the X-axis"; ADM's `objectDivergence` value (BS.2076-2 §10.5) is
the balance between the original and two objects spread either side of it, 0 to 1. The bridge reads the two
values as the same quantity and copies one into the other, quantized to Table 42. ADM also says where the two
objects go (`azimuthRange` for polar positions, `positionRange` for Cartesian ones); OAMD has no field for
that, so a block that sets either is listed in `BridgeResult::unmapped` as "objectDivergence range".

Evidence: **text** for each side. Nothing here has rendered a stream with divergence to compare the two
renderers' spreading.

**Screen reference.** ADM's `screenRef` is a flag; OAMD's `b_object_use_screen_ref` is followed by a
`screen_factor` (1/8 to 1) and `depth_factor` (1/4 to 2) that blend between room and screen anchoring
(§5.2.1.3). `screenRef` 1 becomes a factor of 1 and a depth factor of 1, the fully screen-anchored case. Back
the other way, a factor of one half or more becomes `screenRef` 1. Annex B.2.1.3 also writes an
`audioProgrammeReferenceScreen` whose width is `2 x ref_screen_ratio`, a quantity the OAMD syntax does not
carry (it is in no clause of the standard but that row), so the bridge reads and writes no reference screen.

Evidence: **text** (§5.5.11, §5.6.1.1.18 to .20, §5.2.1.3, Annex B.2.1.3).
