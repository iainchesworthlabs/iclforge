# AC-4 (ETSI TS 103 190): `iclforge::ac4`

`iclforge/ac4/decoder/decoder.hpp` and `iclforge/ac4/encoder/encoder.hpp`, with the table of contents
both work through, `iclforge/ac4/core/toc.hpp`, all in the library `iclforge::ac4`. An AC-4 decoder and
encoder written from ETSI TS 103 190-1 V1.4.1 (channel-based coding) and TS 103 190-2 V1.3.1
(immersive and personalized audio). The library is in namespace `iclforge::ac4` and links nothing from
`iclforge::ac3`: AC-4 shares no bitstream syntax with AC-3 or E-AC-3. The encoder is described under
[Encoding a stream](#encoding-a-stream), and the [AC-4 concepts page](../concepts/ac4.md)
explains the format.

It decodes the mono, stereo, 3.0, 5.X and 7.X channel elements in each of Part 1's codec modes
(SIMPLE, ASPX and the three A-CPL modes) at every frame rate; the immersive element of 7.0.4,
7.1.4, 9.0.4 and 9.1.4 in full and core decoding, rendered by Part 2's channel renderer
([9.X.4](#9x4-channel-elements)); the 22.2 element in full
decoding, as coded, to 24 channels ([22.2](#222-channel-element)); object audio, A-JOC in
full and core decoding and direct-coded objects, with each object's metadata ([Objects](#objects));
streams of several presentations, the one a system chooses decoded with all of its substreams
mixed; the output level and dynamic range control, dialogue enhancement and the downmix; and it
conceals a frame that does not decode when asked to. It refuses, per substream and per frame, with
`DecodeError::kUnsupported` and a reason: core decoding of the 22.2
element and its rendering to any layout but as coded, and, in a stream at 96 or 192 kHz, what the HSF
text does not give it ([96 and 192 kHz](#96-and-192-khz)). It decodes the
speech spectral frontend (Part 1 clause 5.2) for the tracks that select it, from the text alone: no
stream here uses it. A presentation in the efficient high frame rate
mode (`frame_rate_fraction` 2 or 4, Part 2 clause 5.1.3) spreads one codec frame over that many
`raw_ac4_frame()`s: the decoder holds the fragments, `decode()` returns no frame until the unit's
last transmission frame arrives, and the frame it then returns is at the audio frame rate of Part 2
Table 18, with the unit's first `sequence_counter` divided by the fraction as its own. A unit that
does not arrive whole is concealed as any frame that does not decode is. [Development status](development-status.md)
has the detail, feature by feature, and [Validation](../verification.md#ac-4) says how each part
is checked.

## Decoding a stream

A player's stream arrives in pieces. `iclforge::ac4::SyncFrameSplitter` hands over each sync frame once all
of it has arrived, and `iclforge::ac4::Decoder::decode_by_block` turns it into PCM for one presentation, in
blocks of 256 samples. The settings a television offers go in the configuration:

```cpp
iclforge::ac4::DecoderConfig config;
config.output.output_level_dbfs = -24.0;     // Lout: the dialogue level the output is taken to
config.output.drc = iclforge::ac4::DrcMode::kDefault;  // the mode that output level selects
config.output.downmix = iclforge::ac4::DownmixTarget::kStereo;
config.output.dialogue_enhancement_db = 6.0;  // up to the stream's cap
config.presentation.language = "en";          // where the stream offers a choice
iclforge::ac4::Decoder decoder(config);

// The splitter owns no memory: this holds the frame being assembled.
std::vector<std::byte> storage(iclforge::ac4::kSplitterRecommendedBuffer);
iclforge::ac4::SyncFrameSplitter splitter{storage};
std::size_t frames = 0;
for (;;) {
    const auto next = splitter.next();
    if (next.status == iclforge::ac4::SyncFrameSplitter::Status::kNeedMoreInput) {
        const std::span<std::byte> space = splitter.writable();
        const std::size_t want = std::min<std::size_t>(space.size(), 4096);
        in.read(reinterpret_cast<char*>(space.data()), static_cast<std::streamsize>(want));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) {
            splitter.finish();
        } else {
            splitter.commit(got);
        }
        continue;
    }
    if (next.status != iclforge::ac4::SyncFrameSplitter::Status::kFrame) {
        break;  // kEndOfStream, or kTruncated at a cut-off last frame
    }
    ++frames;
    const auto info = decoder.decode_by_block(next.frame.raw_ac4_frame, sink);
    if (!info) {
        const std::string_view reason = decoder.refusal_reason();
        fmt::printf("frame %zu: %.*s\n", frames, static_cast<int>(reason.size()),
                    reason.data());
        return 1;
    }
}
decoder.flush(sink);  // the samples held back short of a block, as one shorter block
```

Full program: [`examples/decode_ac4.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/examples/decode_ac4.cpp) —
decodes the stream it is given to stereo, then prints the presentations it found and the metadata
of the one it decoded. `ctest` runs it on a committed DEE stream.

`sink` is any callable taking a `const iclforge::ac4::PcmBlock&`: one span per channel, the speakers they
are for, the sample rate and the block's position in the output. `iclforge::ac4::BlockSink` refers to it
without copying it, so it has to outlive the call, which a lambda named before the loop does. A
frame's samples rarely divide into 256 (2 002 at 23.976 fps), so each call hands over the whole
blocks it has and holds the rest for the next frame, and `flush()` hands over what is left at the
end. Once the layout is set, decoding this way allocates nothing per frame. `decode()` returns the
same output a frame at a time instead, as an `iclforge::ac4::DecodedFrame` of planar channels.

A frame that produces no output, such as the frames of a stream joined before its first I-frame,
returns nothing rather than an error. A frame that does not decode returns its error, and
`refusal_reason()` says why; with `DecoderConfig::concealment` set to `kRepeatFade` or `kMute`, a
concealed frame comes back in its place, marked as one.

## The controls

`OutputConfig` holds what the listener sets, and `Decoder::set_output()` changes it from the next
frame while a stream plays. A stage takes a new value as it takes the stream's own from one frame
to the next, and a new layout starts its channels' synthesis from silence.

| `OutputConfig` field | What it sets | Default |
|---|---|---|
| `output_level_dbfs` | Lout (Part 1 clause 5.7.9.3.3), the level the stream's dialnorm is taken to, cutting or boosting by 2^((Lout − dialnorm) / 6) | unset: the coded level, nothing compressed |
| `drc` | Which of Table 161's DRC decoder modes compresses: `kDefault` takes the one clause 5.7.9.2 gives the output level; `kHomeTheatre`, `kFlatPanelTv`, `kPortableSpeakers` and `kPortableHeadphones` name one; `kOff` applies the level alone | `kDefault` |
| `headphones` | Whether `kDefault` takes portable headphones rather than portable speakers where the level falls in their range, −16 to 0 dBFS | `false` |
| `dialogue_enhancement_db` | G_DE (clause 5.7.8): how far the dialogue is raised, up to the cap the stream sets, 3, 6, 9 or 12 dB | 0, the tool bypassed |
| `downmix` | The layout (clause 6.2.17): `kAsCoded`, `k5X` (a 7.X element folded to 5.X), `kStereo` (the method the stream prefers), `kLoRo`, `kLtRt`, `kMono`; and for the immersive element and an intermediate spatial format, `k7X4`, `k7X2`, `k7X0`, `k5X4` and `k5X2` (Part 2 clauses 5.10.2 and 5.10.3), from a 9.X.4 source too. A 22.2 source is delivered `kAsCoded` only; no 9.X layout is a target | `kAsCoded` |
| `mix_lfe` | Whether a two-channel or mono downmix takes the LFE at the stream's `lfe_mixgain`, as Part 1 does | `true` |
| `dialogue_gain_db` | g_dialog (clause 6.2.16.1): a presentation's dialogue against its music and effects, up to the stream's `g_dialog_max` | 0 |
| `associated_gain_db` | g_assoc (clause 6.2.16.2), 0 or less: a presentation's associated audio | 0 |

### 22.2 channel element

The 22.2 element (Part 2 clause 6.2.4.3) is two LFE tracks and eleven channel pairs, in the SIMPLE
and ASPX codec modes, and decodes to 24 channels in the order of Part 2 Table A.27's speaker
indices: L, R, C, Ls, Rs, Lb, Rb, Tfl, Tfr, Tbl, Tbr, LFE, Tsl, Tsr, Tfc, Tbc, Tc, LFE2, Bfl, Bfr, Bfc,
Cb, Lw, Rw (`DecodedFrame::speakers` names them). The LFEs are therefore not the fourth channel, as
they are in the other layouts. `iclforge::ac4::Speaker` has an enumerator for each speaker of the
table, the 9.X.4 screen pair included.

Part 2 gives no renderer or downmix for a 22.2 input (Tables 35 to 43 have no row for one) and lists
the element as full decoding only (Table 8). So the decoder delivers 22.2 as coded, refuses every
`downmix` but `kAsCoded` and `decoding = kCore` with `kUnsupported` and a reason that names 22.2, and
dialogue enhancement acts on L, R and C. DRC groups the channels by Part 2 Table 69. No stream of this
element and no other decoder is available to check it against; the readings are in
`libs/ac4/ERRATA.md` under "The 22.2 element", and the streams its tests decode are built from the
standard's tables ([Validation](../verification.md#the-decoders-222-element)).

### 9.X.4 channel elements

The 9.0.4 and 9.1.4 channel modes (`ch_mode` 13 and 14) are the immersive element with `b_5fronts`
(Part 2 clause 6.2.4.1): the 7.X.4 channels and the screen pair, Lscr and Rscr, thirteen tracks in
all, in SCPL, ASPX_SCPL, ASPX_ACPL_1, ASPX_ACPL_2 and ASPX_AJCC. They decode in full decoding to 13 or
14 channels in the order of Part 2 Table A.27, the LFE after the tops and the screen pair last: L, R, C,
Ls, Rs, Lb, Rb, Tfl, Tfr, Tbl, Tbr, LFE, Lscr, Rscr. Core decoding gives the same 5.X.2 as the
7.X.4 modes' (L, R, C, LFE, Ls, Rs, Tsl, Tsr), without the screen pair.

The renderer rows of Part 2 Tables 38 to 43 for a 9.X input fold the screen pair into L and R, or into
C, at the custom downmix gains `gain_f1` and `gain_f2` (6.2.9.4), and the `downmix` targets `k7X4` to
`k5X2` take them. Tables 35 to 37 render to a 9.X layout, which no `downmix` names, so a 9.X layout is
never a target. Dialogue enhancement acts on Lscr, Rscr and C in full decoding (Table 15) and, for
the A-JCC and A-CPL modes in core decoding, by the extension tools of clauses 5.8.2.1 and 5.8.2.2;
`b_de_simulcast` selects the second `de_data()` for core decoding. DRC groups Lscr and Rscr with L and
R (Table 69). No stream of these modes and no other decoder is available to check them against; the
readings are in `libs/ac4/ERRATA.md` under "The 9.X.4 element", and the streams their tests decode
are built from the standard's tables.

`DecoderConfig` holds the rest: `output`, `presentation` (below), `concealment`, `level` (the
`md_compat` level the decoder claims, 3 by default; presentations above it are not chosen),
`decoding` (full or core decoding, Part 2 clause 4.7, full by default), and `syntax`, a trace of
every syntax element read. `syntax` is an `iclforge::ac4::SyntaxTrace`, a
`std::function` the configuration owns, and the decoder keeps a copy of its own, so a lambda
written in place, in a class's constructor for instance, stays valid; what it captures by
reference has to outlive the decoder. The records are described under
[Validation](../verification.md#the-decoders-syntax).

`forge decode` spells each control as an option (`output-level=`, `drcmode=`, `headphones`,
`dialogue-enhancement=`, `channels=`, `downmix=`, `speakers=`, `mix-lfe=`, `dialogue-gain=`,
`associated-gain=`, `md-compat=`, `decoding=`, `conceal=`); see
[Commands](../forge/cli/commands.md#the-output-stage-channels-downmix-drcmode).

### 96 and 192 kHz

A substream whose `ac4_substream_info()` carries `sf_multiplier` (Part 1 Table 89) is at 96 or 192 kHz, and
its group's `ac4_hsf_ext_substream()` holds the lines beyond 24 kHz (clause 4.2.4.3). The decoder reads the
core's lines and the extension's into transforms two or four times as long (Tables 99 to 105, with Annex B's
Tables B.2 to B.7 for the bands), inverse transforms them with Table 186's windows, delays them by clause
5.6, and passes them through the sample rate converter of clause 6.2.15 at the same ratio as at 48 kHz.
`DecodedFrame::sample_rate_hz` is then 96000 or 192000, each frame holds twice or four times the samples
Table 83 gives at 48 kHz, and `decode_by_block()` hands them over in the same blocks of 256 samples
at that rate. `PresentationInfo::sample_rate_hz` gives the rate before a frame is decoded, so that a player
can open its output at it.

Clause 5.4 says a stream with high sampling frequency data uses none of the QMF domain tools, and 6.2.5.2
that decoding it needs the SAP tool and the inverse transform alone. So only a SIMPLE codec mode of the
mono, stereo, 3.0, 5.X and 7.X elements decodes at these rates, and of the output stages only the output
level gain (a scalar on the samples) and the downmix (a matrix on them) apply. A stream at 96 or 192 kHz
that needs more is refused per frame with `DecodeError::kUnsupported` and a reason that names it: the A-SPX
and A-CPL codec modes, the speech spectral frontend, the immersive and 22.2 elements, object audio, the
mixing of a presentation's substreams, dialogue enhancement where the stream sends it and `OutputConfig` asks
for a gain, and the compression of DRC (`DrcMode::kOff` keeps the output level). A 96 or 192 kHz substream
with no extension substream linked is refused too: the text does not say what rate it is at.

No stream at these rates was available, and no other decoder: the tests decode streams built from the
text (`testdata/ac4-hsf/` and `libs/ac4/tests/decoder/hsf.hpp`), each channel a tone above 24 kHz where the
base rate has none, and hold the output to its frequency, level and waveform. The readings the text left open
are in `libs/ac4/ERRATA.md` under "96 and 192 kHz".

## Choosing a presentation

A stream can carry several presentations of its substreams: music and effects with dialogue in
one language or another, the main audio with audio description, and so on (Part 2 clause 4.8).
`PresentationChoice` says which one to decode, and `Decoder::set_presentation()` changes it from
the next frame. Every presentation's substreams are read in every frame, so a newly chosen one
needs no I-frame; its signal starts from silence.

- `presentation_id`: the presentation carrying it, which stays with the presentation as the table
  of contents changes.
- `index`: its position in the table of contents.
- Otherwise the preferences, in the order clause 4.8.2 lists them: `language`, a BCP 47 tag, a
  presentation whose tag matches it whole before one whose primary subtag does; `associated`, the
  content classifier of the associated audio wanted (Part 1 Table 91), with `associated_type`
  refining it by Table 92; and `headphones`, whether a presentation rendered for headphones before
  it was encoded (`b_pre_virtualized`) comes before one that was not, or after it.

Of the presentations this decoder can decode, the stream has not disabled and whose `md_compat` is
within the decoder's level, the one that meets the choice is decoded, the first in the table of
contents among equals. `iclforge::ac4::select_presentation(toc, choice, level)` makes the same choice from a
table of contents alone. Where the text leaves the choice open, `libs/ac4/ERRATA.md` records the
reading taken. `forge decode` takes the choice as `presentation=` (the position),
`presentation-id=`, `language=`, `associated=` and `headphones`.

## What the decoder reports

- `presentations()`: each presentation of the last frame's table of contents, as
  `iclforge::ac4::PresentationInfo`: its `presentation_id`, version, configuration and `md_compat`, whether
  it is enabled, an alternative or pre-virtualized, its name (Part 2 clause 6.3.3.1.4; a name sent
  in chunks over several frames once the decoder has all of it), its language, the channels it
  decodes to, the sampling frequency it decodes at (`sample_rate_hz`, also on each substream), its
  substreams with the role each plays, and whether this decoder decodes it and may choose it.
- `metadata()`: the metadata of the presentation decoded, as the frames read so far have sent it:
  the loudness values (dialnorm and Part 1 clause 4.3.12.3's further values), the DRC
  configuration with each decoder mode and the one applied, dialogue enhancement's method, channels
  and cap, and the downmix gains and preferred method. Values a stream sends only in I-frames are
  kept until a change of source.
- `parse()`: a frame read without decoding, as an `iclforge::ac4::FrameReport`: every substream of its
  `substream_index_table()` in index order, what it turned out to be, how many bits the syntax
  took of it, and, for one not read to its end, the error and the reason. A substream that no
  element of the table of contents this decoder reads names, an HSF extension substream that
  nothing claims among them, is reported as refused and unread. An OAMD substream that sends an
  `oamd_common_data()` (Part 2 clause 6.2.8.1) reports it as `oamd_common_data`. An EMDF payloads
  substream, and an audio substream whose `metadata()` carries an `emdf_payloads_substream()`,
  report the payloads as `emdf_payloads`: each `emdf_payload_id` (Part 1 Table 174) and its
  bytes, in the order read. The decoder does not interpret them.
- `latency_samples()`: the decoder's delay at the output rate, 1 313 samples at
  `frame_rate_index` 13 and at the other indices the same at the internal rate plus the sample rate
  converter's delay. `decode_by_block()` holds back up to 255 samples more.

`forge probe json=1` writes the same reports for a stream: see
[Commands](../forge/cli/commands.md#ac-4).

## Objects

A presentation with object audio (Part 2 clause 4.8.3) decodes to objects, which
`DecodedFrame::objects` hands over for the application to render: each object's PCM, as long as the
frame, and the properties its object audio metadata sets, which Part 2 Annex F lists as what a
decoder gives an object renderer.

- `DecodedObject`: a dynamic object or a bed object (`kind`), the LFE (`lfe`), a bed object's
  loudspeaker (`speaker`), the samples, the properties in force at the frame's first sample, and
  the updates within the frame, in order.
- `ObjectProperties`: whether the object is active, its gain in dB (−infinity for silence) and
  priority, its position (X from the left wall to the right, 0 to 1; Y from the front wall to the
  back, 0 to 1; Z from the floor through the screen's height to the ceiling, −1 to 1), its zone
  constraint, elevation and snap, its width, screen factor and depth exponent, distance and
  divergence, and the trim and headphone data of `add_per_object_md()`.
- `ObjectUpdate`: the sample of the frame at which an update takes effect, counted with the
  decoder's delay as the samples are (clause 5.9.2), and its ramp, in samples, for the renderer to
  move over.
- `DecodedFrame::object_common`: the group's common data (clause 6.3.9.2), trim among it, as the
  stream codes it.

An A-JOC substream decodes in full decoding to its upmix's objects, reconstructed from the downmix
(clause 5.7), and in core decoding to the downmix's own signals, or a static 5.X downmix's bed, with
the downmix's metadata. Dialogue enhancement raises the dialogue objects in both, and a direct-coded
dialogue substream's objects, up to the stream's cap. The decoder renders only an intermediate
spatial format, into `channels`: 7.X.4 as coded and the `downmix` layout otherwise. A presentation
of objects alone has no channels besides; `decode_by_block()` hands over channels only, so a player
of objects takes them from `decode()`.

`forge decode` renders a presentation with objects to speakers through the layout renderer Hearth
plays E-AC-3's objects with (`iclforge::render::LayoutRenderer`, by way of
`apps/shared/media/src/ac4_object_render.hpp`): each object panned from its position at its gain, moving to
each update over its ramp, to the layout `speakers=`, `channels=` or `downmix=` names, 7.1.4
without them. Width, divergence, zones and the screen factor are not rendered.

## Encoding a stream

`iclforge::ac4::Encoder` writes mono, stereo, 5.0 and 5.1, and 5.0.4 and 5.1.4 in Part 2's immersive element
(and, as experimental options, 7.0, 7.1, 7.0.4, 7.1.4 and a 3.0 dialogue substream), at 48 kHz at
every frame rate of Part 1 Table 83, or at 44.1 kHz in frames of 2 048 samples, in the SIMPLE, ASPX
and A-CPL codec modes and, in the immersive layouts, S-CPL and A-SPX with S-CPL, at a constant,
average or variable rate. It takes planar samples at full scale 1.0, in the order the decoder
writes them, and returns raw AC-4 frames:

```cpp
iclforge::ac4::EncoderConfig config{
    .channels = 6,          // 5.1: L R C LFE Ls Rs
    .frame_rate_index = 2,  // 25 fps; 13, the default, is the 2 048-sample frame
    .bitrate_kbps = 384,
    .dialnorm_db = -24.0,
    .drc = iclforge::ac4::DrcConfig{.profile = iclforge::ac4::DrcProfile::kFilmStandard},
};
auto encoder = iclforge::ac4::Encoder::create(config);
if (!encoder) {
    const std::string_view why = iclforge::ac4::Encoder::refusal_reason(config);
    fmt::println("refused: {}", why);  // e.g. "a rate outside 8 to 3 000 kbps"
    return 1;
}
for (const auto& block : input) {  // any number of samples at a time
    auto frames = encoder->encode(block.channels);
    for (const iclforge::ac4::EncodedFrame& frame : *frames) {
        write(iclforge::ac4::sync_frame(frame.raw_ac4_frame, true));  // a raw .ac4 file's sync frame
    }
}
for (const iclforge::ac4::EncodedFrame& frame : *encoder->flush()) {
    write(iclforge::ac4::sync_frame(frame.raw_ac4_frame, true));
}
```

Every field of `EncoderConfig` and the structures in it has a default, so a designated initializer
names only the fields it sets, in the order the header declares them. `create()` refuses a
configuration outside what the encoder writes with `EncodeError::kInvalidConfig`, and
`refusal_reason()` names the rule it breaks, as a string literal: a layout it does not write, a
rate its frames cannot hold, a presentation of the wrong number of substreams, and the rest.

| `EncoderConfig` field | What it sets | Default |
|---|---|---|
| `channels`, `sample_rate_hz` | 1, 2, 5 or 6 channels; 9 or 10 (5.0.4 and 5.1.4); 7 or 8 with `experimental.seven_x`, and 11 or 12 with `experimental.back_pair`; 48 000 or 44 100 Hz | 2, 48 000 |
| `frame_rate_index`, `bitrate_kbps`, `rate_mode` | Part 1 Table 83's frame rate; the rate over whole frames, 8 to 3 000 kbps; `kConstant`, `kAverage` (within the decoder's buffer, Part 1 clause 6.2.4) or `kVariable` | 13, 192, `kConstant` |
| `codec_mode` | `kAuto` (the rate's choice, as DEE's streams make it), `kSimple`, `kAspx`, an A-CPL mode, and in the immersive layouts `kScpl`, `kAspxScpl` and, behind `experimental.ajcc`, `kAspxAjcc` | `kAuto` |
| `iframe_interval`, `iframes`, `fragment_starts` | An I-frame every so many frames, at named frames, and where a container's fragments start, which an MP4 lists as its sync samples | 24 |
| `dialnorm_db`, `loudness` | The dialogue level, 0 to -31.75 dBFS, and Part 1's further loudness values | -31, none |
| `drc`, `downmix`, `dialogue` | The DRC decoder modes on their profiles, the stereo downmix's values, and dialogue enhancement from marked channels or a stem | none |
| `substreams`, `presentations` | Several substreams and the presentations of Part 2 Table 53 made of them (below) | one of each |
| `trace`, `experimental` | A record of every syntax element written; the tools and layouts that no reader outside this project has been checked against | none |

A frame comes out when the input it needs has arrived: `encode()` returns the frames each call
completes, and `flush()` pads the input with silence to the end of its last frame and returns the
rest. Each `EncodedFrame` holds the raw frame, the samples it decodes to and whether it is an
I-frame. `delay_samples()` and `decoder_delay_samples()` give where an input sample lands in the
decoded output, which an MP4's edit list can skip; at `frame_rate_index` 13 the two are 3 072 and
1 313 samples.

### Substreams and presentations

A stream can carry several substreams, each coding inputs of its own (or, for a hybrid dialogue
enhancement, the dialogue beside another substream), and presentations that play them together in
Part 2 Table 53's roles:

```cpp
iclforge::ac4::EncoderConfig config{.bitrate_kbps = 448};
config.substreams = {
    {.channels = 6, .content = iclforge::ac4::ContentClassifier::kMusicAndEffects},
    {.channels = 1, .content = iclforge::ac4::ContentClassifier::kDialogue, .language = "en"},
    {.channels = 1, .content = iclforge::ac4::ContentClassifier::kDialogue, .language = "de"},
};
config.presentations = {
    {.config = 0, .substreams = {0, 1}},  // music and effects with English dialogue
    {.config = 0, .substreams = {0, 2}},  // and with German
};
```

`encode()` then takes the substreams' channels one substream after the other: eight here. Each
presentation gets the least `md_compat` its tracks need and a `presentation_id` of its own unless it
sets them, and its own dialnorm, loudness, DRC and downmix where it sets them. The substreams take
shares of the rate in proportion to their full-band channels unless they set their own. What the
encoder refuses there, and why, is in the header and `libs/ac4/ERRATA.md`.

### Encoding objects

With `experimental.objects`, a substream codes objects in place of channels: each object's PCM, one
input channel each, and its metadata over time in the `ObjectProperties` the decoder reports
(`iclforge/ac4/core/toc.hpp`). The applications convert object scenes, ADM BWF and IAB masters into these
(`forge atmos-encode`, `atmos-adm` and `atmos-iab` with `codec=ac4`, and the Forge GUI's encoder
page, through `apps/shared/media/src/ac4_objects_core.hpp`); the library reads no scene format.

```cpp
iclforge::ac4::EncoderConfig config{.bitrate_kbps = 256};
config.experimental.objects = true;
iclforge::ac4::ObjectsConfig objects;
objects.objects = {
    {.properties = {.position = {0.0, 0.0, 0.0}}},  // a dynamic object at the front left
    {.bed = iclforge::ac4::BedChannel::kCentre},              // a bed object on C
    {.lfe = true},                                  // the LFE
};
config.substreams = {{.objects = objects}};
auto encoder = iclforge::ac4::Encoder::create(config);
// Object 0 moves to the front right over 2 048 samples from input sample 48 000 on.
const iclforge::ac4::ObjectMetadataUpdate move{
    .object = 0, .sample = 48000, .ramp_samples = 2048,
    .properties = {.position = {1.0, 0.0, 0.0}}};
auto frames = encoder->encode(channels, std::span(&move, 1));
```

An object's screen factor and depth exponent are sent as one group of fields, whose factor has no
code for 0, so an exponent other than 1 needs a screen factor of 1/8 or more: `Encoder::create()`
refuses an object without one, and `encode()` an update with such properties
(`EncodeError::kInvalidInput`).

`ObjectsConfig::coding` chooses between an A-JOC substream (Part 2 clause 5.7), the default, and
direct-coded object substreams. A-JOC codes a downmix and the matrices that rebuild the objects from
it: `downmix` is a computed downmix of `downmix_signals` signals (by default one a 32 kbps, up to
ten), each the sum of a run of the objects in the order of their azimuth and carrying its group's
position for core decoding, or a static 5.0 or 5.1 bed the objects are panned onto. The matrices are
chosen frame by frame by running the decoder's own reconstruction on candidate fits and keeping the
one that comes closest to the objects; `decorrelation`, `parameter_bands` and `coarse` set A-JOC's
decorrelators, bands and quantisation. Direct-coded objects go five, three, two or one a substream in
Part 1's elements, with the LFE in the first and an OAMD substream for the group.

An update at input sample n comes out of the decoder at n + `delay_samples()` +
`decoder_delay_samples()`, to within 32 samples. The object substream is the stream's one, at
`frame_rate_index` 13, played by presentations of it alone; an A-JOC presentation takes md_compat 3,
and 7 above 17 objects. `forge ac4-encode objects=` takes a scene file of these terms: see
[Commands](../forge/cli/commands.md#ac4-encode).

### Containers

`iclforge::ac4::sync_frame()` wraps a frame for a raw `.ac4` file or an MPEG-2 transport stream, with Part 2
Annex G's CRC or without it. An MP4 sample holds the raw frame as it is, and the sample entry's
`dac4` box comes from the table of contents the encoder reports: `iclforge::ac4::build_dac4(encoder->toc())`
describes every presentation, and `iclforge::ac4::media_timing()` gives the track's time scale. A CMAF track
keeps TS 103 190-2 Annex H.1.2's rules, which `iclforge::ac4::cmaf_refusal()` checks: a presentation of
configuration 6, EMDF payloads alone, has no field for the `presentation_id` each presentation of
a CMAF track carries.

MPEG-TS carries AC-4 under the DVB profile only, and Matroska registers no codec ID for it, so
`forge mkv` refuses an AC-4 stream. `forge ac4-encode` spells each setting as an option, raw or
MP4 by the output's name: see [Commands](../forge/cli/commands.md#ac4-encode); `forge mp4`, `ts`
and `fmp4` package a stream that already exists, and [Muxing & sinks](muxing-and-sinks.md#muxing-iclforgecontainersmp4mux)
has the library's side.

## The inspector

`iclforge::ac4` reads the framing the decoder starts from, and works on its own for a muxer or a
probe:

- `iclforge::ac4::scan(data)` walks the sync frames of a whole buffer, checking each frame's CRC (Annex G).
  The frames it returns view `data`, so the buffer has to outlive them.
- `iclforge::ac4::SyncFrameSplitter` does the same for a stream that arrives in pieces, in storage the caller
  owns (`kSplitterRecommendedBuffer` holds every frame shorter than 64 KiB). Where the stream does
  not start on a sync word, or something between frames is not a frame, it skips to the next sync
  word, counts the bytes it skipped (`resynchronised_bytes()`) and hands over the frame it found
  only once another sync word follows it.
- `iclforge::ac4::parse_raw_frame()` reads a frame's table of contents: the presentations, the substream
  groups and the substream index table.
- `iclforge::ac4::frame_rate(toc)` gives Part 1 Tables 83 and 84's frame rate, frame length and internal
  sample rate; `iclforge::ac4::build_dac4(toc)` and `iclforge::ac4::rfc6381_codec_string(toc)` give an MP4 sample
  entry's `dac4` box and the codec string HLS and DASH signal. The box describes every
  presentation (Part 2 Annex E.10), or is empty where the table of contents holds something it
  cannot describe whole, and `iclforge::ac4::dac4_refusal(toc)` says what; `iclforge::ac4::cmaf_refusal(toc)` names
  the rule of Annex H.1.2.1 a stream breaks for a CMAF track.
- What a manifest says of a track (Part 2 Annex G): `iclforge::ac4::signalled_presentation(toc)` is the
  presentation it describes, the one with the widest compatibility, the lowest `md_compat` (G.2.3;
  the codec string names it too); `iclforge::ac4::presentation_channel_count(toc)` its channels, for HLS's
  `CHANNELS`; `iclforge::ac4::dash_channel_configuration(toc)` its DASH AudioChannelConfiguration (Table
  G.1, or the Dolby 2015 scheme's word); and `iclforge::ac4::dash_supplemental_properties(toc)` the frame
  rate and a pre-virtualized presentation's descriptors (G.3). `iclforge::ac4::configuration_difference(a,
  b)` names the Annex H.1.2.4 parameter in which two tables of contents differ, empty where every
  sample of a CMAF track may carry both. `libs/ac4/ERRATA.md` ("Manifests and CMAF tracks") has
  the readings these take.

## Errors

Nothing throws for a stream or a configuration. `iclforge::ac4::Error` (`kTruncated`, `kLostSync`,
`kUnsupportedBitstreamVersion`) is the inspector's, `iclforge::ac4::DecodeError` (`kTruncated`, `kInvalidToc`,
`kInvalidStream`, `kUnsupported`, `kMissingIFrame`) the decoder's and `iclforge::ac4::EncodeError`
(`kInvalidConfig`, `kInvalidInput`) the encoder's, each an `std::expected` error with a
`describe()` overload. `iclforge::ac4::DecodeError` is a different type from `iclforge::ac3::DecodeError`, and the
namespace tells them apart. A decoder or an encoder that refuses says why in words through
`Decoder::refusal_reason()` and `Encoder::refusal_reason()`.

## Linking

**In-tree:**

```cmake
target_link_libraries(your_target PRIVATE iclforge::ac4)
```

**Installed package** (`find_package(iclforge)`, see [Using the libraries](index.md)):

```cmake
find_package(iclforge REQUIRED)
target_link_libraries(your_target PRIVATE iclforge::ac4_static)   # or iclforge::ac4_shared
```

The inspector, the decoder and the encoder are one library, and the tables and transforms the
decoder and the encoder share are inside it, with private headers. A package installed with one
linkage, as a vcpkg or Conan one is, also defines the bare `iclforge::ac4`. Through pkg-config it is
`iclforge-ac4`:

```bash
c++ -std=c++23 player.cpp $(pkg-config --cflags --libs iclforge-ac4)
```

`ICLFORGE_BUILD_AC4`, on by default, builds the AC-4 library. The vcpkg port and the Conan
recipe install it where asked for, off by default: `vcpkg install iclforge[ac4]`, or
`-o "iclforge/*:ac4=True"` (see [Using the libraries](index.md)). The C API, Python, Rust and
WebAssembly bindings wrap the decoder and the encoder, the object encoder included ([C
API](c-api.md#ac-4), [Python API](python-api.md#ac-4), [Rust API](rust-api.md#ac-4) and
[WebAssembly](../platforms/wasm.md#ac-4-module)). Android's CMake build compiles the library and
links it into nothing in the app. The ESP-IDF component builds the inspector, core and decoder
behind `CONFIG_ICLFORGE_AC4`, off by default, in single precision on parts with a floating-point
unit and in the fixed-point tier on the ESP32-C3 and ESP32-C6, and never the encoder
([ESP32-P4](../platforms/bare-metal/esp32-p4.md#ac-4), [ESP32-S3](../platforms/bare-metal/esp32-s3.md#ac-4),
[ESP32-C6](../platforms/bare-metal/esp32-c6.md#ac-4)). A decode peaks at 0.29 MB at 2.0 and 1.50 MB
at 5.1.4 on the footprint probe's streams, so the S3 and P4 keep its state in PSRAM; on the C6 it
does not fit beside WiFi.
