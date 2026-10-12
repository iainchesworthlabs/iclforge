# Karaoke in AC-4: options

!!! note "Status as of 2026-10-12: part two of [the karaoke programme](karaoke-eac3.md); read from TS 103 190-1 V1.4.1 and -2 V1.3.1 and from `main`, measured with this repository's own decoder; nothing is built"
    AC-4 has **no karaoke mode**: the legacy `bsmod` syntax is not in it and its
    `content_classifier` has no karaoke value. What it has is presentations, substream groups
    with a role each, a dialogue substream with a user gain, and object gains. This page puts the
    three ways to use those for a karaoke stream side by side, what a stock decoder would do with
    each, how a person authors each from stems, how Hearth, the sinks and the Sendspin wire would
    carry the choice, and a recommendation. **It stops there**: part two is a plan until part one
    (E-AC-3) is finished and the owner says go.

## What AC-4 gives

| What | Where | Says |
|---|---|---|
| No karaoke value | TS 103 190-1 Table 91, §4.3.3.8.1 | `content_classifier` is 3 bits: complete main, music and effects, visually impaired, hearing impaired, dialogue, commentary, emergency, voice over. Table 92's special language tags refine only associated audio (audio description, spoken subtitles, emergency information) |
| A presentation is a list of substream groups | TS 103 190-2 §4.5.1, Table 53 | each group has a role (music and effects, dialogue, associated audio) or, for `presentation_config` 5, whatever its `content_classifier` says; **any number of groups** |
| Groups are shared between presentations | §4.5.1 | a group can belong to several presentations, so parts they have in common are sent once, and the gain applied to a group can differ from one presentation to the next |
| The per-presentation group gain | §6.3.3.1.22 to .24, Table 70 | `sg_gain` 0 to 62 is -0.25 dB steps, **63 is minus infinity**; present when a presentation has more than one group |
| A presentation can be named | §6.3.2.11.1, §6.3.3.1.4, §4.8.2 | a `b_alternative` presentation carries `presentation_name` (UTF-8), meant for a user interface to show |
| A listener's gain on the dialogue | TS 103 190-1 §6.2.16.1; TS 103 190-2 §4.8.3.18 | the user agent **should** apply one gain, `g_dialog`, from minus infinity up to `g_dialog_max`, to all the dialogue tracks, default 0 dB; `g_dialog_max` is `3 x (1 + dialog_max_gain)` dB from the substream's `dialog_max_gain`, **0 dB when it is absent** |
| Dialogue enhancement | TS 103 190-1 §4.3.14, §6.2.12 | a parametric tool that raises dialogue by up to `Gmax = (de_max_gain + 1) x 3` dB; **a boost only** |
| Object gains | TS 103 190-2 Tables 101, 102 | an object's `object_gain` is authored metadata; an alternative presentation can carry `alt_gain` instead (Table 97) |
| Objects can be classified as dialogue | §4.4 | the metadata lets an object be given a type, dialogue among them |
| It is not a requirement | §4.4 | the document describes the metadata and the controls available for personalisation and **specifies no requirements for them**: decoder behaviour is left to application specifications |

Two things follow from the last two rows and decide most of what follows: the listener's control
over dialogue level is **a "should" on the user agent**, not something a conforming decoder must
offer; and **a mute of a vocal needs either g_dialog or a presentation**, because dialogue
enhancement can only add.

## What the repository already has

| Cell | Has | Evidence |
|---|---|---|
| Presentation selection | by `presentation_id`, by position, or by language and associated-audio preference | `libs/ac4/include/iclforge/ac4/decoder/config.hpp` `PresentationChoice`, `select_presentation()`; `forge decode ... presentation=<n>` and `presentation-id=<id>` |
| g_dialog | `OutputConfig::dialogue_gain_db`, "below -120 dB the dialogue is silent"; up to the stream's maximum | `forge decode ... dialogue-gain=<dB>` |
| Dialogue enhancement | `OutputConfig::dialogue_enhancement_db`, 0 to 12 | `forge decode ... dialogue-enhancement=<dB>` |
| Hearth | `Ac4Settings::presentation_id`, `presentation_index`, `dialogue_db` (g_dialog), `dialogue_enhancement_db` | `apps/hearth/engine/src/decoder_settings.hpp` |
| The encoder | substreams (`substreamN=`), each with `content=`, presentations (`presentationN=`, `-config=`, `-id=`, `-name=`, `-gains=`), dialogue substreams with `max-dialogue-gain=`, object substreams (`forge atmos-encode ... codec=ac4`, `ac4_encode_objects.cpp`) | `forge help ac4-encode` |
| The wire | AC-4 travels coded (`kAc4` burst); **no presentation or dialogue key** among the sixteen decoder keys | `libs/sendspin`, `kDecoderSettingNames` |
| A per-object gain override | **none** in the decoder's controls | `OutputConfig` has the two dialogue gains and the associated gain only |

## The three shapes

### (a) Presentations: one stream, several experiences

Three substreams carry the audio once: the music and effects (the backing), V1 and V2 as dialogue
substreams. Four presentations list them with different group gains: **Vocals** (all three at 0
dB), **Backing** (V1 and V2 off), **V1** and **V2**. The player picks one.

Measured, with `forge ac4-encode` and `forge decode`, tones of 300 and 500 Hz for the backing and
900 and 1100 Hz for V1 and V2, 3 s, 160 kbps, one stream with 3 substreams and 4 presentations:

```text
forge ac4-encode bk.wav k.ac4 160 substream2=v1.wav substream3=v2.wav \
    substream1-content=music-and-effects substream2-content=dialogue substream3-content=dialogue \
    presentation1=1,2,3 presentation1-config=5 presentation1-id=0 presentation1-name=Vocals \
    presentation2=1,2,3 presentation2-config=5 presentation2-id=1 presentation2-name=Backing presentation2-gains=0,off,off \
    presentation3=1,2,3 presentation3-config=5 presentation3-id=2 presentation3-name=V1      presentation3-gains=0,0,off \
    presentation4=1,2,3 presentation4-config=5 presentation4-id=3 presentation4-name=V2      presentation4-gains=0,off,0
forge decode k.ac4 out.wav presentation-id=<n>
```

| `presentation-id` | L carries | R carries |
|---|---|---|
| 0 Vocals | L 0 dB, V1 -6.0, V2 -6.0 | R 0 dB, V1 -6.0, V2 -6.0 |
| 1 Backing | L 0 dB | R 0 dB |
| 2 V1 | L 0 dB, V1 -6.0 | R 0 dB, V1 -6.0 |
| 3 V2 | L 0 dB, V2 -6.0 | R 0 dB, V2 -6.0 |

(The dialogue is mono and centre-panned, which is -6.0 dB in each channel of a stereo output by
Table 216's 0.5.) With no choice made, the decoder's default is presentation 0.

- **A stock decoder.** The only thing every AC-4 decoder must do with a stream like this is pick a
  presentation, by its own preferences (language, associated audio, the first in the table of
  contents among equals): **it plays "Vocals"**, so the stream is an ordinary song on a device that
  offers no menu, which is a better failure than AC-3's, where a player that does not know karaoke
  plays the vocals from the surrounds. Whether a TV or an AVR *offers* the other three to a person
  is not specified (§4.4) and was not measured: no AC-4 device is here, and Dolby's AC-4 decoder
  element (`dlbac4dec`, which has a `pres-index` property and a `de-level` of -12 to 12) refused
  this repository's plain stereo stream, as it did in IM4, so it was not pursued.
- **Authoring from stems.** The command above is the whole of it: backing (stereo or 5.1), V1 and
  V2 as mono WAVs. The repository's encoder already writes it.
- **Hearth, the sinks, the wire.** Hearth carries `presentation_id` today; the choice type maps
  none, V1, V2 and both to a presentation id by a **convention** (decision A2: the ids above, or
  the names, which are text and so a weaker contract). A sink that decodes AC-4 itself
  has no key for it (the wire has no presentation key); one that does not is sent PCM that
  Hearth's own decoder made, which needs no wire work.
- **Cost.** Library: a resolver from the neutral choice to a presentation, S. Hearth: the page
  already has the choice; its notices and the id convention, S to M. The Forge GUI's flow: an AC-4
  mode that writes the substreams and the four presentations from the parts, M to L. Sinks: a new
  wire key and the firmware, M, only for boards that decode AC-4 (the P4 does 5.1 in real time, the
  S3 only 2.0 simple).
- **What it cannot do.** Per-part *levels* (W10's sliders): a presentation's gains are fixed at
  authoring, so a listener's "melody 6 dB down" has nothing to act on; levels need (c) or (b).

### (b) A vocal object with a gain

Carry the vocal as an object in an A-JOC or direct-coded object substream, with an `object_gain`
(and, in an alternative presentation, an `alt_gain`) in its OAMD.

- **A stock decoder.** Renders the object at its authored gain, vocals on. The specification gives
  a listener no object-gain control (§4.4 leaves behaviour to application specifications), and
  the repository's decoder has none either. The only user-visible gain is g_dialog, and it applies to
  a *dialogue substream*, not to an object, so an object gets a mute only by a second presentation
  with an `alt_gain` of minus infinity, which is shape (a) with more machinery.
- **Authoring.** `forge atmos-encode ... codec=ac4` with each part as an object and a scene file
  for the gains. The repository's encoder writes the object substreams itself, with no Dolby tool;
  the author has to think in objects and positions, which a karaoke author does not otherwise
  need to.
- **Hearth, the sinks, the wire.** A per-object gain control is new in the decoder's `OutputConfig`
  and new on the wire.
- **Cost.** A decoder control (M), the choice's levels mapped onto it (S), everything of (a)'s
  wire (M), and a flow that makes objects from stems (L).
- **Verdict.** It carries the levels (W10) that (a) cannot, and does nothing on any decoder that
  is not ours. It is the only shape that needs a decoder API that does not exist.

### (c) A dialogue substream and the user's gain

One music-and-effects substream and one or two dialogue substreams in a single presentation, as
`presentation_config` 0 (M&E and dialogue) or 5, with `max-dialogue-gain` set. The listener's
g_dialog moves the vocals against the backing, from silence (`-inf`) to the stream's maximum (a
boost of 3, 6, 9 or 12 dB).

Measured, the same tones, one presentation, `forge decode ... dialogue-gain=<dB>`:

| `dialogue-gain` | V1 in L and R |
|---|---|
| 0 | -6.0 dB |
| -6 | -12.0 dB |
| -120 | silent |
| +12 (stream written with `substream2-max-dialogue-gain=12`) | **+6.0** dB |

- **A stock decoder.** It "should" offer the gain (§4.8.3.18) and the specification does not say a
  device must. A decoder that offers none plays 0 dB, vocals on, as in (a). A decoder that offers
  *dialogue enhancement* offers a **boost only** (the parametric tool never cuts), which for
  karaoke raises the vocal and mutes nothing. The honest summary: a mute is reachable from a
  player we control and, on a device, only if its user interface offers the dialogue gain with its
  floor at silence, which nothing here measured.
- **Authoring.** The same stems as (a), with the vocals as `content=dialogue` and **one**
  presentation; `substreamN-pan=` places a mono vocal (or two). V1 and V2 would share the one
  g_dialog (a single gain applies to all dialogue tracks), so a separate V1 and V2 control is not
  possible here; a presentation per vocal is (a).
- **Hearth, the sinks, the wire.** `Ac4Settings::dialogue_db` exists and Hearth's decoder applies
  it; the choice's vocal level maps onto it (a single level for both vocals). Same sink story as
  (a): coded to a board that has a key, PCM otherwise.
- **Cost.** The smallest of the three: the resolver (S), a flow mode that writes one presentation
  (M), and no new decoder control.
- **What it cannot do.** The melody guide has no gain (it is in the music and effects), V1 and V2
  cannot be separated, and the upper bound is the stream's.

## Side by side

| | (a) presentations | (b) object | (c) dialogue gain |
|---|---|---|---|
| On a device with no menu | **plays with vocals** | plays with vocals | plays with vocals |
| On a device with a presentation or language menu | may offer the four | no | no |
| On a device with a dialogue-level control | the first presentation; the control acts too, if the vocals are dialogue | no | **acts**, where its floor is silence |
| V1 and V2 apart | **yes** | yes (if both are objects) | no |
| Levels of M, V1, V2 | no | **yes**, ours only | vocals together, M no |
| Authoring from stems | **one command; written by the encoder today** | objects and a scene file | one command |
| New decoder API | none | **one** | none |
| New wire keys for a sink that decodes AC-4 | one (presentation) | two (object, level) | one (dialogue level) |
| Size to build | S + M..L | M + M + L | S + M |

## Recommendation

**Build (a) with (c) inside it.** One stream with a Vocals presentation that carries the vocals as
*dialogue* substreams (so a device that offers a dialogue gain acts on them) and the Backing, V1
and V2 presentations beside it (so a player that selects can reach every state W10's choice
names). It degrades to a normal song, it needs no decoder control that does not exist, the
encoder already writes it, and it keeps the AC-3 choice type's none, V1, V2 and both meaning the
same thing. Per-part *levels* come from (c)'s g_dialog for the vocals together and are honestly not
offered per vocal; (b) is held back until someone needs a melody level on AC-4, because it is the
one shape that needs a decoder API of its own and does nothing off our decoder.

## Decisions

For the owner, when part one is finished:

| # | Decision | Recommended | Alternatives |
|---|---|---|---|
| A1 | Which shape | **(a) with (c) inside it**, as above | (c) alone (smallest); (b) later |
| A2 | How a presentation says which choice it is | **A convention of ids**: 0 vocals, 1 backing, 2 V1, 3 V2, held in the decoder's resolver and the authoring flow, with names for people | Names only (text, locale-dependent); the stream's `presentation_name` carries `karaoke:v1` (a private reading of a label) |
| A3 | What a sink that decodes AC-4 itself is sent | **A `presentation_id` key and a `dialogue_db` key** in the role's settings, advertised per sink, with PCM as the fallback as phase 2's D3 rule has it | PCM always for AC-4 karaoke (no wire work; the P4 loses the 5.1 coded path); reuse `programme` as the presentation index |
| A4 | Whether the Forge GUI's flow writes AC-4 karaoke | **A format choice in the flow**, after (a)'s library resolver, default AC-3 | Command line only (`forge ac4-encode` writes it today) |

## What can be checked

There is **no Dolby oracle** for AC-4 karaoke. The engine's AC-4 encoders write none of these
structures: they cannot write several presentations, associated audio, dialogue enhancement
parameters or a choice of bitstream version ([the oracles note](karaoke-eac3-findings.md#dolbys-encoder-cannot-write-one)),
so there is nothing to request before 2026-11-06. What can be checked is this repository's decoder
against its own reading of the specification (the measurements above), the framing and the
syntax elements by a second reader of the bitstream (`MediaInfo`'s per-frame trace, which prints
the table of contents, the presentation substream and the DE configuration), and the structure of
what the encoder wrote by `forge probe`. A listening check of a device's menu is the owner's.

## What the choice type has to carry

So that part one's type serves this without change (D5 of [the plan](karaoke-eac3.md#decisions)):
a reproduction (off, aware, capable), a vocals choice (none, V1, V2, both) and three levels in dB
over the same span. For AC-3 and E-AC-3 these select coefficients in an output stage; for AC-4
they would select a **presentation** (the reproduction and the vocals) and a **g_dialog** (the
vocal level), and "aware" means the default presentation. Nothing in the type is AC-3's but its
name, which is what E1 changes.
