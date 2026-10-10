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

## ST 2098-2:2022 Table 19: where the channels with no bed label sit

Table 19 names each `ChannelID` by its ST 428-12, ST 2098-5 or BS.2051-2 channel name and gives no
coordinates. ST 2098-5 Annex B (informative) says in prose where each immersive loudspeaker is "typically"
placed; ST 428-12, which holds the base layer's, could not be read here (pub.smpte.org answered 403).
`build_iab()` carries a bed channel as a position, so each code that has no `BedLabel` is placed from that
prose and from the conventions `bed_label_position()` already has (a surround pair halfway back, the rear pair
at the rear wall, a height at the ceiling):

| Code | Channel | x, y, z | From |
|---|---|---|---|
| `0x1` / `0x3` | Left / Right Center | 0.25 / 0.75, 0, 0 | The name: between Left and Center, Center and Right |
| `0xB` / `0xC` | Left / Right Top Surround | 0.25 / 0.75, 0.5, 1 | "laterally between the Center screen Loudspeaker and the Left screen Loudspeaker", at the ceiling; the array's middle in depth |
| `0x10` | Center Height | 0.5, 0, 1 | "directly above the Center Loudspeaker, at the top center of the screen" |
| `0x11` / `0x12` | Left / Right Surround Height | 0 / 1, 2/3, 1 | "along the left side and left rear ... starting approximately 1/3 of the distance from the screen to the rear wall": the middle of that span |
| `0x13` / `0x14` | Left / Right Side Surround Height | 0 / 1, 0.5, 1 | "directly above the Lss array", which sits where the surround pair does |
| `0x15` / `0x16` | Left / Right Rear Surround Height | 0 / 1, 1, 1 | "directly above the Lrs Loudspeaker(s)" |
| `0x17` | Top Surround | 0.5, 0.5, 1 | "on the ceiling over the center of the seating area", read as the middle of the room |

`0x5` / `0x9` (Left / Right Side Surround) are placed where `0x6` / `0xA` (Left / Right Surround) are. A
bed has one or the other, and the annex puts the Lss array on the side wall as the Ls array is.
`0xE` / `0xF` (Left / Right Height) are the top front pair: "behind the screen directly above the Left
Loudspeaker, at the left top corner of the screen".

The usual cinema bed, L C R Lss Rss Lrs Rrs LFE Lts Rts (ST 2098-5 Table 2's 9.1OH), bridges as a result.
The codes `0x18`-`0x7F` ("Reserved for D-Cinema") and everything above `0x89` are Reserved and refused.

Evidence: **text** for the names and for the descriptions quoted, **reading** for every coordinate. No
renderer has placed a stream's channels to compare them against.

## ST 2098-2 §10.5.11-14 and TS 103 420 Table 20: a zone pattern that is not a preset

ST 2098-2 gives each of nine zones (nineteen in `ObjectZoneDefinition19`) a gain from 0 to 1. TS 103 420
Table 20 gives `zone_constraints_idx`, six presets, and `b_enable_elevation` beside it. Neither text says
how to carry a pattern the presets lack. The bridge's reading:

1. A gain of 0.5 or more counts as the zone being included, since a preset includes or excludes a zone.
2. A horizontal pattern that is exactly a preset's maps to it.
3. Any other pattern takes the preset that includes every zone the pattern includes, and among those the one
   that lets the object into the nearest extra zones, summed by distance in the room plan between the
   loudspeaker positions the zones are named for. The preset therefore never excludes a zone the author
   included, and everything it excludes the author excluded. The rest is the approximation, listed in
   `IabBridgeResult::unmapped`.
4. The presets treat the rear as one group, so a `ObjectZoneDefinition19` rear with any zone included
   asks for all of it, and a rear with some zones in and some out is never exact.
5. A pattern with no horizontal zone included has nothing to cover and is left `kNone`.

Evidence: **reading**. The earlier reading left such an object unconstrained, which also excludes no
included zone but applies none of the author's exclusions. No renderer has been compared.

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
