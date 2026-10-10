# Fitting a large object master into an encode: split the cap, then cluster

Status: **proposal, not started. For review before any code.** Written 2026-10-10 against `main`.

An IAB (ST 2098-2) or ADM master is bridged to an encode as one `ObjectPath` plus one mono buffer per
bed channel or object. `build_iab()` and the ADM `build()` refuse more than 15 of them
(`BridgeError::kTooManyChannels`), and `forge atmos-iab` and `atmos-adm` inherit that. A cinema master
commonly carries more: ST 2098-2 §10.2.5 itself uses a 9.1 bed and 118 objects, a `MaxRendered` of 128,
as its example. So the largest practical gap in the IAB bridge is not a missing field but a missing
reduction. This note says what the cap is, what is cheap, and what a clustering feature would have to
decide.

## What the cap is

| Target | Limit | Where it comes from |
|---|---|---|
| E-AC-3 JOC (`AtmosEncoder`) | 15 objects, and the bed's LFE is the 16th | TS 103 420 §8.3.2.2 caps `complexity_index_type_a` at 16 |
| AC-4 objects (`encode_ac4_objects`) | 64 | `kAc4MaxObjects`: the writer's `object_layout_of` and the decoder's storage (`libs/ac4/ERRATA.md`); Table 55 of TS 103 190-2 ties the standard's own count to `md_compat` (17 and an LFE at 3) |

Both bridges apply the **15 before the codec is known**: `load_iab_atmos_source()` calls `build_iab()`,
and only then does `run_atmos_iab()` branch on `codec=ac4`. So a 40-element master that the AC-4
object path would accept is refused by an E-AC-3 limit.

## Step 0, before any clustering: the cap follows the codec

Not clustering, and small. `build_iab()` and `build()` take a maximum channel count (an options overload,
so the existing symbol stays), and the CLI passes the codec's: 15 for E-AC-3, the AC-4 writer's own
limit for `codec=ac4`. This alone takes every master of 16 to 64 elements to AC-4. The open point is which
number is safe for AC-4: 64 is what this project's writer and decoder hold, and Table 55 says a receiving
decoder's allowance depends on `md_compat`. Read Table 55 and the encoder's `md_compat` choice before
fixing the number.

## What is left for clustering

A master above the codec's limit: above 15 to E-AC-3, above 64 to AC-4. The reduction has to choose which
sources share an output element and what that element's position, gain and size are.

### Where it lives

A codec-blind function in `libs/objects` (namespace `iclforge::objects::oba`), next to `ObjectPath`:

```cpp
struct ClusterOptions { std::size_t max_outputs; /* LFE excluded */ std::uint64_t seed = 0; ... };
struct ClusterResult  { std::vector<ObjectPath> paths; std::vector<std::vector<float>> pcm;
                        std::vector<std::vector<std::size_t>> members;   // per output, per segment
                        ClusterReport report; };
std::expected<ClusterResult, ClusterError> cluster(std::span<const ObjectPath>,
                                                   std::span<const std::span<const float>>,
                                                   std::uint32_t sample_rate, const ClusterOptions&);
```

The bridges call it after their identity pass and before returning, when a limit is given and exceeded;
`cluster=on|off` (default decided below) on `forge atmos-iab` and `atmos-adm`. Nothing in
`libs/objects` learns about IAB or ADM, which is what lets both bridges and the AC-4 path share one.

### What it must keep out of the mix

- **The LFE** is carried through as its own output, as the bridges already route it (`lfe_send`).
- **Dialog.** IAB's `AudioDescription` flags an element as dialog (`dialog`, also music, effects, foley,
  ambience), and the model keeps it. ADM's `audioContent` has a `dialogue` element, which the bridge does
  not read today. A dialog object is not clustered while an output remains for it: summing correlated
  dialog with other material is where clustering is audible. The flag is a hint with a priority, not a
  guarantee, and a master that does not set it clusters on loudness and position alone.
- **Snapped objects** (`snap`) and objects with a size above a threshold stay singletons first: both
  say the object's place matters.

### The method

1. **Importance** per source per window: its loudness over the window (RMS in dB, gated) times its gain,
   so a silent or fully attenuated source costs no output.
2. **Segments.** Reassign over windows of a fixed number of encode frames (the E-AC-3 and AC-4
   metadata cadence is one update a frame), not once for the programme and not every frame. A static
   assignment is wrong for any object that crosses the room; a per-frame one makes membership flicker.
   The windows overlap by one frame and the output crossfades its members over that frame.
3. **Cost.** Importance-weighted squared distance between a source's position and its cluster's
   centroid in the room cuboid, with the height and depth axes weighted as the renderer's panner
   perceives them (to be fixed by listening, not guessed here). Seeded k-means++ with a fixed seed, so
   the same input gives the same bytes, which the project's other writers also promise.
4. **Output element.** Position is the importance-weighted centroid per frame; gain 1 with the members'
   gains applied to their samples; size grows to cover the members' extent, capped; zone constraint is
   the union of the members' (the least restrictive), elevation on when any member's is. The mix is a
   plain sum (not power-normalised: the members are different signals and the encode's own loudness
   handling is downstream), with a headroom check that reports a clip rather than hiding it.
5. **A report.** Per output: its members, the largest member-to-centroid distance, and any clip. Printed
   by the CLI as warnings the way `unmapped` already is.

### What it does not try to be

Not a model of how Dolby's tools cluster: their algorithm is not public and nothing here can be
compared to it. Not perceptual coding: no masking model, no loudness-matching of outputs. Not
reversible: `members` says what went where, and the original cannot be recovered from the mix.

## Validation

There is no external oracle for this, which has to be said where the feature is described, as it is for
object decode ([Object quality trend](../docs/object-quality-trend.md)). What can be checked:

- **Unit and property tests**: no reduction when the input fits and the output equals the input;
  output count at most the limit; the LFE untouched; determinism for a seed; every source's samples
  accounted for exactly once per window (the sum of squares of the members' contributions equals the
  output's before the crossfade); a scene with two well-separated groups gives two outputs whose centroids
  are the groups'.
- **A render comparison**: the original scene and the clustered one through the project's own renderer
  (`libs/render`) to the layouts it already renders, per-speaker level and correlation against the
  original. The same self-consistency footing as the object quality series: a regression tripwire, not
  a conformance result.
- **An end-to-end CLI test**: a generated 20-element IAB through `forge atmos-iab` to E-AC-3, decoded
  with the in-repo decoder, the bed's energy per channel against the unclustered render.

## Phases

| | What | Size |
|---|---|---|
| P0 | The cap follows the codec: options overload on both bridges, the CLI passes the codec's limit, Table 55 read | Small, its own PR |
| P1 | `cluster()` and its tests, used by nothing | Medium |
| P2 | Bridge option, CLI `cluster=`, report as warnings, docs and status row | Small |
| P3 | The render comparison harness; decide the default from what it shows | Medium |

## Decisions for the owner

1. **Default.** Off until P3 has numbers (a master above the limit keeps failing loudly), or on with a
   warning (it encodes, and says what it merged). This note recommends off first.
2. **Beds.** May a bed channel share an output with an object? A 7.1.2 bed alone is ten of the fifteen.
   Keeping beds as written leaves five outputs for every object; merging them costs the bed's
   speaker-anchoring. This note recommends keeping beds up to a share of the budget and merging the
   remainder only when the objects alone do not fit.
3. **Headroom.** Use all 15 outputs or leave some for the encode's bitrate (more objects cost more
   metadata at a fixed rate; the E-AC-3 default is 448 kbps)? To be measured in P3.
4. **Whether to build it at all.** Step 0 may be enough for the masters this project's users have. If the
   masters are above 64, or E-AC-3 is the target, it is not.
