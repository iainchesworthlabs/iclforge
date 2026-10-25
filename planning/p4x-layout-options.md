# Choosing the speaker layout: what a board can carry, and what a change does

**Status, 2026-10-25: logic, route and web page built and host-tested; the on-screen chooser is not
built yet** (it waits for the Preferences registry that the Preferences foundation work defines); no
board was used for any of this. The kit and board check (a legal `PUT /layout`, an illegal one, the
listing, a layout change on a playing stream) is booked later by the coordinator.

The owner, on 2026-10-25: "The speaker layout: I can pick 2.0 to 9.1.6 and apply it through
Preferences, rather than going to the status screen. There are considerations: slot widths, device
types, what the device can support. It should show 'this device is configured in such a way that it
can only support this, this, this', and the others are not available." And: this is a generic product
base, so what a board can do is data the hardware and the build state, not "the P4X".

## What decides what a board can carry

A layout is a name (`5.1.4`) or a speaker list; it needs one output slot per speaker. The board
refuses one with more slots than its sink has now (`accept_layout()`, `hearth_sink.cpp`). What the
sink has depends on four things, each stated by the board and none by the layout:

| Fact | Where it comes from | What it does |
|---|---|---|
| Slot width, 16 or 32 | `Settings::slot_bits` (`PUT /slot-width`), Kconfig `ICLFORGE_EXAMPLE_I2S_SLOT_BITS` | One TDM line is one frame of at most `I2S_LL_SLOT_FRAME_BIT_MAX` bits: 128 on the S3 and C6 (four 32-bit slots, or eight 16-bit), 512 on the P4 (sixteen or thirty-two) |
| Second I2S line | `Settings::second_line` (`PUT /wiring`), only where the part has a second controller (the S3) | Doubles the ceiling. Not on the C6, and the P4's one wide line is deliberately never doubled |
| The chip | `HardwareFacts::revision_hard_limit_below` | An ESP32-P4 below v3.0 has no clock for TDM: nothing above stereo opens, whatever the ceiling says |
| The build | `CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS` | The widest stream the Sendspin player decodes: 2 on the C6 |

`sink_slots()` is the ceiling for the settings in force and the only number `accept_layout()` reads.
`sink_max_slots()` and its width are the most the sink carries at any setting, which is what tells
the listing what a setting change would buy.

## The verdicts

`libs/device/include/iclforge/layout_options.hpp`, free of ESP-IDF, with
`libs/device/tests/test_layout_options.cpp` running it for sixteen kinds of board. For each layout in
a catalog of thirteen (`1.0 2.0 2.1 5.0 5.1 5.1.2 7.1 5.1.4 7.1.2 7.1.4 9.1.2 9.1.4 9.1.6`) it gives a
verdict, a sentence a person can act on, a `fix` word saying which setting would help, and whether
the verdict rests on a measured play. In the order the rules are tried:

1. **No audio output** on the board: unavailable, "This board has no audio output to set a layout for."
2. **More slots than the sink has now**: unavailable. The sentence names the need, the supply and the
   way out: "7.1.4 needs 12 slots; this board's output carries 4 (32-bit slots, one line). 16-bit slots
   and a second line would carry 16." `fix` is `slot-width`, `second-line`, `slot-width-and-second-line`,
   or `hardware` when no setting reaches it ("The most this board can carry is 8.").
3. **The chip cannot open TDM** and the layout is wider than stereo: unavailable, `hardware`.
4. **The build decodes fewer channels than the layout has**: with limits (the C6).
5. **The layout needs the second I2S line**: with limits. Two lines have been brought up on a
   development S3, and the Kconfig help says no DAC was ever on both.
6. **A stream this wide was measured falling behind** on this kind of board: with limits.
7. **Wider than anything measured** on this kind of board: with limits, "Not verified".
8. Otherwise supported, and `verified`.

**The listing is never looser than the route, and is stricter in one place.** A layout the listing
calls available or limited is one `accept_layout()` takes (the test runs every one of the 84 names
the grammar reads against `OutputLayout::parse` and the same rule, on every board). The route stays
permissive for a chip that cannot open TDM: `PUT /layout` still takes `5.1` on a P4 below v3.0,
because the board suite sets exactly that layout on such a board to test the fold. The listing is
where a person is told it cannot work.

### The evidence

A table keyed by the build's profile id (`kLayoutEvidence`), each row from a record in the tree. A
profile with no row, or a scratch build, has measured nothing beyond stereo, which every board
plays; the answer is "Not verified", not an invented limit. `test_sink_profile.py` fails when a
profile in `profiles.json` has no row, so a new hardware variant is a profile and a row.

| Profile | Widest play measured keeping up | Source |
|---|---|---|
| `p4-rev3` (the P4X kit) | 16 slots: AC-3 5.1 onto `9.1.6`, sixteen 32-bit TDM slots, 142 permille of a frame, no underruns | `baselines/p4-rev3.json`, row `ac3-5.1-tdm16`, recorded 2026-10-20 |
| `s3-n16r8`, `s3-n16r8-ac4` | 2 (the suite plays 2.0 only). A 7.1.4 Atmos stream took 1.08 of real time with Wi-Fi up, so 12 slots and wider say so | `baselines/`, [`esp32-sink-compatibility-matrix.md`](esp32-sink-compatibility-matrix.md) Matrix 1 |
| `c6-16mb`, `c6-4mb` | 2; the build decodes two coded channels and a server decodes anything wider and sends it narrower (a 5.1 stream aborted the board in 10 to 12 s on 2026-09-22) | `sdkconfig.sendspin-c6`, the Kconfig help of `SENDSPIN_MAX_CODED_CHANNELS` |
| `p4-rev1`, `p4-rev1-ac4` | 2; TDM does not open above stereo on v1.x silicon | `esp32-platform-profiles.md` (PR 11), `hardware_info.hpp` |

What it offers at each board's shipped setting (all of it is in the test):

| Board | Shipped setting | Carries | Listing |
|---|---|---|---|
| ESP32-S3 N16R8 | 32-bit, one line | 4 | `1.0`, `2.0` supported; `2.1` with limits (not verified); `5.0` to `7.1` need 16-bit slots; `5.1.4` and wider need 16-bit and a second line |
| ESP32-S3, 16-bit, two lines | | 16 | all offered, `2.0` supported, the rest not verified, `5.1.4` and wider with the second line's caveat, `7.1.4` and wider with the 1.08 figure |
| ESP32-C6 | 16-bit, one line | 8 | `1.0`, `2.0` supported; up to `7.1` with limits (the build decodes two channels); wider unavailable, nothing to change |
| ESP32-P4 below v3.0 | 32-bit | 16 | `1.0`, `2.0` supported; every wider layout unavailable (the chip) |
| ESP32-P4 v3.x, the P4X kit | 32-bit | 16 | all thirteen supported |
| A board with no sink | | 0 | all unavailable |

## The route and the page

`GET /layout-options`, a route of its own and not a change to `GET /layout`: that one answers a
bare layout name that `tools/hearth` and the board suite read. It is read-only, 404 on a build with
no sink to choose for, and sent one option at a time (`httpd_resp_send_chunk`), because the whole
list is about 3 KB and the server task has 6 KB of stack. `control.cpp` builds the facts from the
handlers `/status` already reads, plus `max_coded_channels` (new) and the chip's clause.

```json
{"layout":"2.0","slots":4,"slot_bits":32,"second_line":false,"profile":"s3-n16r8","options":[
 {"layout":"2.0","slots":2,"verdict":"supported","fix":"none","verified":true},
 {"layout":"5.1","slots":6,"verdict":"unavailable","fix":"slot-width","verified":false,
  "reason":"5.1 needs 6 slots; this board's output carries 4 (32-bit slots, one line). 16-bit slots would carry 8."}]}
```

The device page reads it when the sink's slots, slot width or wiring change. A preset the board
calls unavailable is greyed with the reason as its tooltip; one with a limit keeps its place and its
reason; a closed **Why some layouts are unavailable or limited** under the field lists every reason
in words (a tooltip does not reach a touch screen). A board without the route (an older firmware)
leaves the presets to the page's own slot count, as before. The page is 63,843 of its 65,536 bytes.

The four copies of the route list were kept in step: `GET /api` in `control.cpp`, and
`apps/demos/wasm/tests/device-ui/stub.js` (its `/api` text, its route table, its model of the
route). The contract test holds the stub's reply texts and key order to `control.cpp`'s.

## What a layout change does to a playing stream

Read from the code on 2026-10-25, not measured on a board. There are two paths, and the page's
sentence "takes effect at the next play" is true of only one.

| | A server plays to the board over Sendspin (the product) | `POST /play`, the board's own HTTP source (development) |
|---|---|---|
| When it applies | At the next **burst boundary**, or at once if nothing plays (`BurstPlayer::set_layout`, `apply_pending`). The stream is not stopped | At the next play: a running play keeps its layout, `begin_play` reads the new one |
| What is rebuilt | The renderer; routing back to identity; **per-output trims and delays zeroed** (a server's settings); the decoders given back (an AC-4 decoder is 286 KB and is made again at the next chunk); and if the output count changed, the playout is flushed and the sink reopened (the I2S lines torn down and set up again) | The sink is reopened between plays if the slot count differs |
| What the person hears | A gap while the sink reopens and the decoder restarts. **Its length is not measured** | Nothing until the next play |
| The server | **No new hello**: `outputs.count` in the hello is the sink's ceiling, which a layout does not move (a slot-width or wiring change does, and re-announces). The new layout reaches the server in the role's `state` report (`layout`), sent when the report differs | n/a |
| A server that sets its own layout | Replaces the board's at its next `settings` message (a server on the extension role, Hearth's, can; Music Assistant sends none) | n/a |
| The room view on the screen | The panel re-parses the layout text when it changes (five times a second) and redraws the room from it, while a stream is active; it follows at the burst the layout takes effect | n/a |
| Persistence | `settings_set_layout` stores it in NVS (since #1633), so it survives a restart; a speaker list too long to store applies until the restart | Same |

### The question for the owner

**A layout change while a stream plays: apply at once, with a confirm that the music will break up
for a moment, or apply on the next play?**

Recommendation: **at once, with a confirm only when something is playing** ("Changing the speakers
makes the music drop out for a moment. Change them?" with the new layout named; no question when
nothing plays).

- It is what the firmware already does on the product path. "Next play" has no clean meaning for a
  Sendspin board: a server's stream carries on, and nothing marks "the next play" to defer to. It would be new code
  (hold the layout until the stream ends, and show a pending state), and a person setting up
  speakers wants to hear the change now.
- Choosing a layout is a rare, deliberate act, and a break of the stream for it is cheap; a
  confirm costs one tap and keeps a stray touch from breaking the music.
- Stopping the stream is not an option the board has: only the server stops a stream; the board can
  hold one (`hold`), and the server's session carries on.

Two things the owner should know: the break is **not measured** (the kit run will), and a layout
change **zeroes the trims and delays** a server set per output; the per-speaker level and distance
trim preference should not be set before the layout and then lost to a later change, or the layout
change should keep them where the outputs still exist.

The text `PUT /layout` answers ("ok; takes effect at the next play") and the page's confirmation
should change with the answer; they are left as they are until it is given.

## Not built, and open

- **The on-screen chooser**: an entry "Speaker layout" in Preferences with a list editor (the
  unavailable rows greyed, each with its reason). It needs the Preferences registry and the caps
  struct from the Preferences foundation work; `layout_options.hpp` takes `LayoutFacts`, which that
  struct fills the same way `control.cpp` does.
- **The kit and board check**, booked by the coordinator: `PUT` a legal layout and an illegal one,
  read the listing, watch a playing stream take a change and time the gap.
- **`OutputLayout::parse("9.2.6")`** accepts and keeps sixteen of seventeen speakers, silently
  dropping `LFE2` (`named()`'s `add` stops at `kMaxSlots`). Not touched here; the catalog does not
  contain it.
- **The device test `every zone the page offers is one the firmware takes`** fails on `main`
  (`validTimeZone` rejects the `none` zone the page offers); not touched here.
