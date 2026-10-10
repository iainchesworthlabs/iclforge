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

## ITU-R BS.2076-3 §5.4.3.2 and §5.5.4: Matrix blocks and packs

libadm 0.14.0 does not read or write most of a Matrix. Its channel-format parser has the loop that
would build a Matrix block commented out (`parseAudioChannelFormat`), so a Matrix channel arrives with
no blocks; its formatter writes a Matrix block as an element holding only its ID, `rtime` and
`duration` (`formatBlockFormatMatrix`: "TODO: add missing matrix attributes and elements"); and its
Matrix block class lists `encodePackFormatIDRef`, `decodePackFormatIDRef`, `inputPackFormatIDRef` and
`outputPackFormatIDRef` among its "unsupported parameters". `libs/adm/src/adm_xml_extras.cpp` reads
and writes them on the `<axml>` text, as it does `zoneExclusion`. The shapes taken:

- **A block**: `outputChannelFormatIDRef` (0 or 1; the file may spell it `outputChannelIDRef`, which
  Table A1-15's footnote says a reader must also accept, and it is written as the former),
  `jumpPosition`, and one `matrix` holding `coefficient` elements, then `gain` and `importance`
  where they are not the defaults. That is the order of the standard's own sample code (§5.4.3.2.1).
  The standard does not say whether the order is binding, and no schema was available to check.
- **A coefficient**: the element's text is the `audioChannelFormatID` it reads; attributes `gain`
  (default 1.0, with `gainUnit` `linear` or `dB`), `phase` (degrees, default 0), `delay`
  (**milliseconds**, default 0) and a `*Var` form of each. The standard allows one of the constant and
  the variable, never both; the writer follows that. A negative linear gain inverts the signal.
- **A pack**: `encodePackFormatIDRef` and `decodePackFormatIDRef` (0 or more), `inputPackFormatIDRef`
  and `outputPackFormatIDRef` (0 or 1), written first in the pack as in §5.5.4.2.

**HOA pack defaults.** §5.5.5.1 Table A1-25 has `normalization`, `nfcRefDist` and `screenRef` as
sub-elements of the pack, and so does EAR's parser. libadm's parser reads them as XML *attributes*
of the same element and its formatter writes neither. Both spellings are read; sub-elements are
written. A pack's values are defaults that a block's own override (§5.5.5).

Evidence: **text** for every shape above (Tables A1-15, A1-16, A1-24, A1-25 and the sample code), and
the **EBU's renderer read as code** (EAR `ear/fileio/adm/xml.py`: `coefficient` as `HandleText` with
the `gain`, `phase`, `delay` and `*Var` attributes, `outputChannelFormatIDRef`, and the pack's
references and HOA defaults as sub-elements). Nothing here has run EAR or any other tool over a
file this module wrote, or written a Matrix pack of its own that another tool applied.
