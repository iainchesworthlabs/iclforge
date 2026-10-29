# Choosing the speaker layout: what a board can carry, and what a change does

**Status, 2026-10-25: built and host-tested; no board was used.** The verdict, `GET /layout-options`, the
device page, the Speaker layout preference (a list on the screen then, [a sheet since](p4x-layout-builder.md)), and the root-cause fix for a chip
that cannot open TDM are in one pull request; the board suite's capability-aware rows are in the next
(`board_suite.py`). The kit and board check (a legal `PUT /layout`, an illegal one, the listing, a layout
change on a playing stream) is booked later by the coordinator.

The owner, 2026-10-25: "The speaker layout: I can pick 2.0 to 9.1.6 and apply it through Preferences,
rather than going to the status screen. There are considerations: slot widths, device types, what the
device can support. It should show 'this device is configured in such a way that it can only support this,
this, this', and the others are not available." This is a generic product base, so what a board can do is
data the hardware and the build state, not "the P4X". And later the same day: "platform capabilities are a
core piece here", so a route that accepts what the list refuses is a bug and not a note, and "we can
increase the device web page size if required".

## What decides what a board can carry

A layout is a name (`5.1.4`) or a speaker list; it needs one output slot per speaker. What the sink
carries depends on four things, each stated by the board and none by the layout. They are fields of
`BoardCaps` (`libs/device/include/iclforge/board_caps.hpp`), filled once at boot from the build and the
hardware (`firmware/hearth-sink/main/board_caps.cpp`):

| Fact | Field | Where it comes from | What it does |
|---|---|---|---|
| Slot width, 16 or 32 | `sink.slot_bits` | `Settings::slot_bits` (`PUT /slot-width`), Kconfig `ICLFORGE_EXAMPLE_I2S_SLOT_BITS` | One TDM line is one frame of at most `sink.frame_bit_max` bits: 128 on the S3 and C6 (four 32-bit slots, or eight 16-bit), 512 on the P4 (sixteen or thirty-two) |
| Second I2S line | `sink.second_line`, `sink.second_line_possible` | `Settings::second_line` (`PUT /wiring`), only where the part has a second controller (the S3) | Doubles the ceiling. Not on the C6, and the P4's one wide line is deliberately never doubled |
| The chip | `sink.tdm_usable` | `sink_tdm_usable()`, from the chip revision | An ESP32-P4 below v3.0 has no clock for TDM: nothing above stereo opens |
| The build | `coded_channels_max` | `CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS` | The widest stream the Sendspin player decodes: 2 on the C6 |

`SinkCaps::slots()` is what the sink carries now, and it is what `sink_slots()` reports: they are one
number.

## One verdict

`libs/device/include/iclforge/layout_options.hpp`, free of ESP-IDF. `evaluate_layout(caps, name, slots)`
gives a layout a verdict (supported, with limits, unavailable), a sentence a person can act on, a `fix`
word saying which setting would help, and whether a measured play stands behind it. Four callers ask it
and nothing else:

| Caller | What it does with the verdict |
|---|---|
| `PUT /layout` (`accept_layout`, `hearth_sink.cpp`) | Refuses an unavailable layout with 409 and the sentence as the body; takes the rest, with a limit as a note after the `ok` |
| `GET /layout-options` (`control.cpp`) | The whole catalog, one option at a time, in the JSON the page reads |
| The Preferences registry (`preferences.hpp`, entry `speaker-layout`) | One choice for each layout, available when the verdict is not unavailable, the sentence as the note; `set_value()` asks it before it asks the board |
| The device page (`iclforge_ui.js`) | Greys an unavailable preset with the sentence, lists every sentence, and shows the board's own words when a `PUT` is refused |

In the order the rules are tried:

1. **No audio output**: unavailable, "This board has no audio output to set a layout for."
2. **The chip cannot open TDM** and the layout is wider than stereo: unavailable, `hardware`: "This chip
   (v1.3) has no clock for multi-channel output, so only 1.0 and 2.0 can play."
3. **More slots than the sink carries now**: unavailable. The sentence names the need, the supply and the way
   out: "7.1.4 needs 12 slots; this board's output carries 4 (32-bit slots, one line). 16-bit slots and a
   second line would carry 16." `fix` is `slot-width`, `second-line`, `slot-width-and-second-line`, or
   `hardware` when no setting reaches it ("The most this board can carry is 8.").
4. **The build decodes fewer channels than the layout has**: with limits (the C6).
5. **The layout needs the second I2S line**: with limits. Two lines have been brought up on a development S3
   and the Kconfig help says no DAC was ever on both.
6. **A stream this wide was measured falling behind** on this kind of board: with limits.
7. **Wider than anything measured** on this kind of board: with limits, "Not verified".
8. Otherwise supported.

**A layout is accepted if and only if the sink carries its slots.** The test
(`libs/device/tests/test_layout_options.cpp`) runs every one of the 83 names the grammar reads against
`OutputLayout::parse` and `SinkCaps::slots()`, for every board in `board_variants.hpp`, each S3 at every
slot width and wiring, and a board with no sink, and holds `PUT /layout`'s decision, the listing and the
registry's choices to one verdict, sentence for sentence.

### The root cause on an ESP32-P4 below v3.0

Until 2026-10-25 the wide sink reported what its frame holds, 32 slots at 16-bit, and `PUT /layout` took `5.1`
on a chip that could not open it (the play then failed, or the decoder folded the stream to Lo/Ro). A
listing that said "unavailable" while the route said "ok" was the symptom; the sink's number was the bug
([decision 31](esp32-device-ui.md#decisions), revising decision 21). The wide sink now reads the chip
revision (`kEsp32p4TdmClockRevision`, the one constant `GET /hardware`'s `revision_hard_limit_below` uses too)
and reports two slots from `sink_slots()` and `sink_max_slots()`. So on such a board `/status` says
`sink_slots: 2`, `/hardware` says "up to 2 slots at 16-bit" (and its notice still says why), the Sendspin
hello's `outputs.count` is 2, a 5.1 stream plays folded to the 2.0 layout, and `PUT /layout 5.1` is refused
with the chip as the reason. `PUT /layout` has no special case: the verdict is the board's capabilities.

### The evidence

A table keyed by the build's profile id (`kLayoutEvidence`), each row from a record in the tree. A profile with
no row, or a scratch build, has measured nothing beyond stereo, which every board plays; the answer is "Not
verified", not an invented limit. `test_sink_profile.py` fails when a profile in `profiles.json` has no row, so
a new hardware variant is a profile and a row.

| Profile | Widest play measured keeping up | Source |
|---|---|---|
| `p4-rev3` (the P4X kit) | 16 slots: AC-3 5.1 onto `9.1.6`, sixteen 32-bit TDM slots, 142 permille of a frame, no underruns | `baselines/p4-rev3.json`, row `ac3-5.1-tdm16`, recorded 2026-10-20 |
| `s3-n16r8` | 2 (the suite plays 2.0 only). A 7.1.4 Atmos stream took 1.08 of real time with Wi-Fi up, so 12 slots and wider say so | `baselines/`, [`esp32-sink-compatibility-matrix.md`](esp32-sink-compatibility-matrix.md) Matrix 1 |
| `c6-16mb`, `c6-4mb` | 2; the build decodes two coded channels and a server decodes anything wider and sends it narrower (a 5.1 stream aborted the board in 10 to 12 s on 2026-09-22) | `sdkconfig.sendspin-c6`, the Kconfig help of `SENDSPIN_MAX_CODED_CHANNELS` |
| `p4-rev1`, `p4-rev1-ac4` | 2; TDM does not open above stereo on v1.x silicon | `esp32-platform-profiles.md` (PR 11), `hardware_info.hpp` |

What it offers at each board's shipped setting (all of it is in the test):

| Board | Shipped setting | Carries | Listing |
|---|---|---|---|
| ESP32-S3 N16R8 | 32-bit, one line | 4 | `1.0`, `2.0` supported; `2.1` with limits (not verified); `5.0` to `7.1` need 16-bit slots; `5.1.4` and wider need 16-bit and a second line |
| ESP32-S3, 16-bit, two lines | | 16 | all offered, `2.0` supported, the rest not verified, `5.1.4` and wider with the second line's caveat, `7.1.4` and wider with the 1.08 figure |
| ESP32-C6 | 16-bit, one line | 8 | `1.0`, `2.0` supported; up to `7.1` with limits (the build decodes two channels); wider unavailable, nothing to change |
| ESP32-P4 below v3.0 | 32-bit | 2 | `1.0`, `2.0` supported; every wider layout unavailable (the chip), and refused by `PUT /layout` with the same sentence |
| ESP32-P4 v3.x, the P4X kit | 32-bit | 16 | all thirteen supported |
| A board with no sink | | 0 | all unavailable |

## The route and the page

`GET /layout-options`, a route of its own and not a change to `GET /layout`: that one answers a bare layout name
that `tools/hearth` and the board suite read. Read-only, 404 on a build with no preferences backend to ask, and sent
one option at a time (`httpd_resp_send_chunk`), because the whole list is about 3 KB and the server task has 6 KB of
stack. The bytes are `layout_options_json()`'s, tested on the host; `control.cpp` sends them in pieces. **Not a
`GET /capabilities` serving `BoardCaps` whole**: the suite and the page need the verdict, and a dump of the
capabilities would have each of them re-derive it in its own language, which is the duplication this change removes.

```json
{"layout":"2.0","slots":4,"slot_bits":32,"second_line":false,"profile":"s3-n16r8","options":[
 {"layout":"2.0","slots":2,"verdict":"supported","fix":"none","verified":true},
 {"layout":"5.1","slots":6,"verdict":"unavailable","fix":"slot-width","verified":false,
  "reason":"5.1 needs 6 slots; this board's output carries 4 (32-bit slots, one line). 16-bit slots would carry 8."}]}
```

The device page reads it when the sink's slots, slot width or wiring change. A preset the board calls unavailable is
greyed with the reason as its tooltip; one with a limit keeps its place and its reason; a closed **Why some layouts
are unavailable or limited** under the field lists every reason in words (a tooltip does not reach a touch screen).
A board without the route leaves the presets to the page's own slot count, as before. A refused `PUT` shows the
firmware's sentence. The page is 63,776 bytes; its budget is raised to 81,920 ([decision 22](esp32-device-ui.md#decisions)).

The four copies of the route list were kept in step: `GET /api` in `control.cpp`, and
`apps/demos/wasm/tests/device-ui/stub.js` (its `/api` text, its route table, its model of the route and of `PUT
/layout`'s answers). The contract test holds the stub's reply texts and key order to `control.cpp`'s and
`layout_options.hpp`'s.

## On the screen

Preferences, group Sound, **Speaker layout** (`iclforge/preferences.hpp`, entry `speaker-layout`, route `/layout`).
**2026-10-25** it was drawn as a list (`Editor::kList`): one line a layout, a button with the layout and its sentence
beside it. **2026-10-26** the owner, on the kit: "it's a big long vertical list. I'm surprised it isn't a horizontal
list, or some better grouping", and asked for a way to say what the room has instead. The row is now the layout in
force and a Change button (`Editor::kBuilder`), and Change opens a sheet with quick picks in two groups and a
Build yours (around you, subwoofers, heights, how the heights are mounted), over the same verdict:
[p4x-layout-builder.md](p4x-layout-builder.md). What stayed: a layout the board cannot carry is greyed with the
reason; one it carries with a limit is available and says so; while a stream plays Apply asks first
(`Pref::confirm`): "Changing the speakers makes the music drop out for a moment. Change to 5.1?" with Change and
Cancel; with nothing playing the change is made at Apply.

## What a layout change does to a playing stream

Read from the code on 2026-10-25, not measured on a board. There are two paths, and the page's sentence "takes effect
at the next play" is true of only one.

| | A server plays to the board over Sendspin (the product) | `POST /play`, the board's own HTTP source (development) |
|---|---|---|
| When it applies | At the next **burst boundary**, or at once if nothing plays (`BurstPlayer::set_layout`, `apply_pending`). The stream is not stopped | At the next play: a running play keeps its layout, `begin_play` reads the new one |
| What is rebuilt | The renderer; routing back to identity; **per-output trims and delays zeroed** (a server's settings); the decoders given back (an AC-4 decoder is 286 KB and is made again at the next chunk); and if the output count changed, the playout is flushed and the sink reopened (the I2S lines torn down and set up again) | The sink is reopened between plays if the slot count differs |
| What the person hears | A gap while the sink reopens and the decoder restarts. **Its length is not measured** | Nothing until the next play |
| The server | **No new hello**: `outputs.count` in the hello is the sink's ceiling, which a layout does not move (a slot-width or wiring change does, and re-announces). The new layout reaches the server in the role's `state` report (`layout`), sent when the report differs | n/a |
| A server that sets its own layout | Replaces the board's at its next `settings` message (a server on the extension role, Hearth's, can; Music Assistant sends none) | n/a |
| The room view on the screen | The panel re-parses the layout text when it changes (five times a second) and redraws the room from it, while a stream is active; it follows at the burst the layout takes effect | n/a |
| Persistence | `settings_set_layout` stores it in NVS (since #1633), so it survives a restart; a speaker list too long to store applies until the restart | Same |

### The question for the owner, and what is built meanwhile

**A layout change while a stream plays: apply at once, with a confirm that the music will break up for a moment,
or apply on the next play?** Recommendation, and what is built until the owner answers (the coordinator, 2026-10-25):
**at once, with a confirm only when something is playing**. It is what the firmware already does on the product path.
"Next play" has no clean meaning for a Sendspin board (a server's stream carries on and nothing marks it), and would be
new code and a pending state on the screen; the board cannot stop a server's stream (only the server stops one); a
person setting speakers wants to hear it now. Flipping it is one line (`confirm` and the apply words in the registry
entry) plus a hold in `BurstPlayer::set_layout`.

Two things that follow: the break is **not measured** (the kit run will time it), and a layout change **zeroes the
trims, delays and routing** a server set per output (`configure_defaults` passes empty spans); the per-speaker trim
session has been told exactly where, because its table has to survive a layout change.

The text `PUT /layout` answers on success ("ok; takes effect at the next play") and the page's confirmation are left
as they are until the question is answered.

## Not built, and open

- **The kit and board check**, booked by the coordinator: `PUT` a legal layout and an illegal one, read the listing,
  watch a playing stream take a change and time the gap; a P4 below v3.0: `PUT /layout 5.1` is refused with the chip
  as the reason, a 5.1 stream plays folded.
- **Rows for the board suite on other profiles**: a row is a baseline and a baseline is recorded on a board. The suite
  now plays or refuses a layout row by what the board says; the rows come with the next recording.
- **`OutputLayout::parse("9.2.6")`** silently dropped `LFE2`; PR #1638 rejects it. The test counts the names that parse
  and does not depend on which side of that change it is.
- **App budgets in `profiles.json`** for `p4-rev3` and `s3-n16r8` were already exceeded on `main` before this change;
  raised by their own change (the owner approved).
