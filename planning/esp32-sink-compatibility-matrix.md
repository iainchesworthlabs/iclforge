# ESP32 sinks: what each part accepts, what it refuses, and where PCM comes in

**Status, 2026-10-10:** a reading of `main` at `8bcd33be9` and of the measured tables on the three
platform pages. Nothing was run for this page: no board, no engine. Every "refused" and "nothing is
sent" below is read from the code at the file and line given, and the timings are the platform
pages' own. It is the input to phase I6 of [`ac4.md`](ac4.md#i6-the-esp32-sinks), which is the phase
that settles most of what is open here.

**x real time** is the decoder's time for a frame over the audio the frame carries (42.7 ms for AC-4
at 48 kHz, 32 ms for AC-3 and E-AC-3). Below 1 keeps up; above 1 falls behind for as long as the
stream plays.

**Update, 2026-10-12: the AC-4 path in the burst player (I6) is built** (`ac4_stream.{hpp,cpp}`,
`burst_player.cpp`, `CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4`). A P4 whose image has the AC-4 decoder
lists `ac4`, states 6 channels and no objects, and plays AC-4 from a group: 2.0 at 0.18 to 0.33 and
5.1 at 0.51 to 0.73 of real time, every output's level the host decoder's to 0.01 dB
([`esp32-p4.md`](../docs/platforms/bare-metal/esp32-p4.md#ac-4-from-a-server)). An S3 does not list
it by default (its 2.0 A-SPX and the converter fall behind, and the support object cannot list a
class), and the C6 cannot hold the decoder. Facts 1 and 3 below describe `main` on 2026-10-10.

## The finding that changes the question

Decision 32 says "where it misses, Hearth sends the C6 PCM, which it plays today". **The code does
not do that for a sink Hearth has paired.** Decision 39 and the I2 note in `ac4.md` say what it does
instead, and they are right:

> A sink on the extension role that does not list `"ac4"` ... is sent nothing for an AC-4 item.

Three facts make that so, and none of them depends on the chip:

1. **Every ESP32 build lists `ac3` and `eac3` and never `ac4`.** It is a literal:
   `s.data_types = {ac::DataType::kAc3, ac::DataType::kEac3};`
   ([`sendspin.cpp:159`](../firmware/hearth-sink/main/sendspin/player/sendspin.cpp)). It does not
   follow `CONFIG_ICLFORGE_AC4`, so the P4, which decodes AC-4 well, is listed the same as the C6.
2. **A paired Hearth sink is on the extension role and never on `player@v1`.** The server activates
   `_iclforge_player@v1` for a long-term-PSK connection that sent `iclforge_support`, and the
   `player@v1` PCM role only otherwise
   ([`server_host.cpp:316`](../libs/sendspin/src/server_host.cpp)). The sink's PCM role is for a
   server that is not Hearth (Music Assistant), and it offers stereo only
   (`channels = 2`, [`sendspin.cpp:212`](../firmware/hearth-sink/main/sendspin/player/sendspin.cpp)).
3. **An extension-role member is only ever sent bursts.** `try_start` returns without starting a
   member whose burst stream is refused (`start_burst_stream` gives `kFormatNotListed` for an
   unlisted data type, [`server_session.cpp:869`](../libs/sendspin/src/server_session.cpp)), and
   `push()` skips every member that `bursts` ([`server_host.cpp:1682`](../libs/sendspin/src/server_host.cpp)).
   There is no PCM leg for such a member, for any codec.

So today, for a paired board in a Hearth group, **the answer to "when does it go to PCM" is never.**
It goes to nothing. The same is true when a wider AC-3 or E-AC-3 stream reaches the C6 (below): the
board refuses each syncframe and stays quiet.

The test that exists covers the two cases that do work: a `player@v1` sink gets the decode as PCM,
and an extension-role sink that lists AC-4 gets the bursts
([`test_engine_network_group.cpp:399`](../apps/hearth/engine/tests/test_engine_network_group.cpp)).
Nothing tests an extension-role sink that does not list the stream.

## The three doors

| Door | Who uses it | Role | What it carries | What decides |
|---|---|---|---|---|
| A Hearth group | The product | `_iclforge_player@v1`, bursts | AC-3, E-AC-3 (AC-4 only if the sink lists it) | The server: data type and rate listed. The board: channel cap, memory |
| Another Sendspin server | Music Assistant | `player@v1`, PCM | 2 channels, 48 kHz, 16 or 24 bit | Nothing: the board plays PCM |
| `POST /play <url>` | Development and every AC-4 figure on the platform pages | None, the board's own HTTP source | AC-3, E-AC-3, AC-4 | The board: rate, layout, memory floor |

The on-board AC-4 decoder is reachable by the third door only.

## What each part advertises and caps

| | ESP32-S3 | ESP32-C6 | ESP32-P4 |
|---|---|---|---|
| `data_types` in a group | `ac3`, `eac3` | `ac3`, `eac3` | `ac3`, `eac3` |
| Sample rates | 48,000 | 48,000 | 48,000 |
| Widest syncframe the burst player decodes | no cap | **2 coded channels** (`sdkconfig.sendspin-c6:35`) | no cap |
| AC-4 decoder in the image | `sdkconfig.ac4`, off by default | `sdkconfig.ac4`; never fits (below) | `sdkconfig.ac4`, off by default |
| AC-4 heap floor before a play (`player.cpp:819`) | 286,365 B, always met with PSRAM | **286,365 B, never met** (117 KB free beside the network) | always met with PSRAM |
| Output | I2S TDM, 128 bits a frame | I2S TDM, 128 bits a frame | TDM unreachable above 2 channels on revision v1.3 silicon |

## Matrix 1: AC-3 and E-AC-3 in a Hearth group (the product today)

| Stream | S3 | C6 | P4 |
|---|---|---|---|
| AC-3 or E-AC-3, 2.0 | **Plays**, 0.11 to 0.17 | **Plays** | **Plays**, 0.05 to 0.07 |
| AC-3 5.1 | **Plays**, 0.47 | **Refused**, 5.1 > 2 channels; quiet | **Plays**, 0.18 |
| E-AC-3 5.1 (AHT, ecpl) | **Plays**, 0.48 | **Refused**; quiet | **Plays**, 0.18 |
| E-AC-3 7.1.4, JOC, Atmos | **Plays at the line**, 1.08 with Wi-Fi up, not gated | **Refused**; quiet | **Plays**, 0.37 |

Notes:

- The S3's 5.1 and 7.1.4 figures are the AC-4 image's with Wi-Fi up (`esp32-s3.md`, AC-4 "On the
  board"). The probe with no network has 0.31 (AC-3) and 0.34 (E-AC-3) for 5.1 and 0.90 for 7.1.4;
  its 2.0 figures, 0.11 and 0.17, are the only ones the 2.0 row has. The P4's 2.0 figures are the
  probe's too, and its 5.1 and 7.1.4 are with the network up.
- **The C6's cap of 2 is a 2026-09-22 measurement.** E-AC-3's fixed-tier 5.1 peak has fallen from
  164,066 to 109,806 B since, and the sink has not been run on a board again
  ([`sink-esp32-c6.md`](../docs/hearth/sink-esp32-c6.md)). The cap may now be tighter than the part
  needs. It is one board run to find out.
- The P4 decodes 7.1.4 but plays two channels on this silicon; the sink folds.

## Matrix 2: AC-4

The first column is the same for every chip: nothing is listed, so nothing is sent. The others are
the board's own decode, by `POST /play`, with Wi-Fi up and the Sendspin player idle. Streams are
DEE's gold set; "to" is the layout the play was placed on.

| Stream | A Hearth group (any chip) | S3, x real time | C6 | P4, x real time |
|---|---|---|---|---|
| 2.0 SIMPLE | **Nothing sent** | **0.87, keeps up** | **Refused**, `memory` | 0.28 |
| 2.0 A-SPX | Nothing sent | 1.03 to 1.09, **falls behind** | Refused | 0.37 |
| 2.0 through the frame-rate converter (23.976, 24, 25, 29.97 fps) | Nothing sent | 1.32 to 1.88, falls behind | Refused | 0.51 to 0.69 |
| 5.1 SIMPLE, to 5.1 | Nothing sent | 2.14 | Refused | 0.64 |
| 5.1 A-SPX, to 5.1 | Nothing sent | 2.60 | Refused | 0.83 |
| 5.1 A-SPX + A-CPL 2, to 5.1 | Nothing sent | 2.81 | Refused | 0.91 |
| 5.1 A-SPX + A-CPL 3, to 5.1 | Nothing sent | 3.21 | Refused | **1.14, falls behind** (1.06 folded to 2.0) |
| 5.1.4, full decoding | Nothing sent | 4.90 to 5.61 | Refused | **1.57 to 1.90** (1.34 to 1.66 folded) |
| 5.1.4, core decoding | Nothing sent | not measured | Refused | 1.24 to 1.58 |

What the cells mean:

- **Every S3 and P4 PCM hash equals the host's** for all twenty plays. "Falls behind" is speed
  only: the S3 is correct on every stream, and 2 to 5 times too slow on 5.1 and wider.
- **C6 "Refused" is one test**, the heap floor, taken at the start of a play: 117,328 B free at
  the start and 26,420 when the decoder is made, against 286,365 B for the smallest stream. The
  floor is the smallest stream's peak, so a wider one asks for more still (2.0 A-CPL 426,918 B,
  5.1 704,311 B, 5.1.4 1,502,903 B). No decode time was measured on the C6.
- **The S3 starts a play it cannot keep up with.** Nothing in the player compares decode time with
  the frame period. A 5.1 AC-4 play on an S3 over HTTP begins, underruns, and goes on.

## The tests that exist, in the order a stream meets them

| # | Test | Where | On what | A failure is |
|---|---|---|---|---|
| 1 | Data type listed, and rate listed | Server, `server_session.cpp:869` | Every group stream | The member does not start; the item is not sent to it |
| 2 | Coded channels <= `max_coded_channels` | Board, `burst_player.cpp:780` | Every AC-3 and E-AC-3 syncframe, C6 only (cap 2) | Each syncframe dropped, one console line a stream, `invalid_chunks` counts |
| 3 | Rate equals the sink's | Board, `player.cpp:721` | HTTP AC-4 | Play fails, `why: "sample rate"` |
| 4 | Layout placeable, <= `kMaxCoded` | Board, `player.cpp:725` | HTTP AC-4 | Play fails, `why: "channel layout"` |
| 5 | Heap >= 286,365 B | Board, `player.cpp:819` | HTTP AC-4 | Play fails, `why: "memory"`, board runs on |
| 6 | The decoder's own refusal | Board, `player.cpp:928` | HTTP AC-4 | Play fails, `why: "decode"` |

AC-4 **objects** are a class of their own, and one the boards cannot take whole. The decoder
puts a presentation's objects out beside its channels (`DecodedFrame::objects`) for the
application to place; `decode_by_block()`, which the board's player calls, hands over channels
alone. By the code (none of this has been run on a board), a presentation that has channels (a
bed) therefore plays as that bed, after the board has paid to reconstruct objects it throws away;
a presentation of objects alone (A-JOC or direct-coded, which is what this project's encoder
writes) decodes to no channels, and the board plays nothing. So the AC-4 path of a board that
lists `ac4` has to state `max_objects.ac4` of 0
until it places objects, and Hearth, which now reads an AC-4 presentation's object count and
whether it has channels (`ItemFacts::objects`, `objects_alone`), sends such a board PCM decoded
here for a presentation past it. What this does not yet do is the board's half: skipping the
object reconstruction when the player places none (a `DecoderConfig` option the library does not
have), which is where a bed-and-objects stream's time goes on a P4 or an S3; no AC-4 stream with
both has been measured on a board.

What is **not** there:

- **No test on time.** Nothing refuses, or falls back, because a stream would not keep up.
- **No PCM leg for an extension-role member**, so no test could send one.
- **No per-class listing.** `data_types` is a flat set of three words. A sink cannot say "AC-4, but
  only 2.0 SIMPLE".
- **No memory test per stream class.** One floor, for the cheapest stream.

## Proposed criteria

One rule, so that no one has to remember a table:

> **A sink decodes a stream itself only if the stream's class is one its firmware lists as
> measured-good. Every other stream, Hearth decodes and sends that sink as PCM.**

A *class* is what Hearth can know about a stream before it sends a byte: codec, channel mode of the
presentation the listener chose, the codec tools in use (SIMPLE, A-SPX, A-CPL level, S-CPL), whether
the frame rate needs the converter, and the sample rate. **Not all of that is readable today.** The
table of contents gives channel mode (`Toc::channel_mode`), frame rate and sample rate. The codec
tools are in the substream data, and no public decoder or inspector header reports them (the only
`aspx` and `acpl` in `libs/ac4/include` is the encoder's config). The tools are what separate the
P4's 5.1 A-CPL 2 (0.91, keeps up) from A-CPL 3 (1.14, does not), so a per-class listing needs the
inspector to report them first. Without that, a class is channel mode and frame rate only, and the
P4 would have to list 5.1 whole or not at all.

A class is **measured-good on a part** when, on that part's board with Wi-Fi up and the player idle:

1. **It is correct**: the PCM hash equals the host's.
2. **It keeps up with a margin**: average x real time <= 0.95 (the project calls 0.91 real time;
   ten boots of one image moved a frame by 5%), and the worst frame inside `min_buffer_ms` (300 ms).
3. **It fits**: the free heap at the start of a play is at least the class's measured peak, never
   the one floor for all classes.
4. **It holds through a group**: the same stream played from Hearth to the board for ten minutes
   with no underrun, because every AC-4 figure on the platform pages is the board's HTTP source
   with the Sendspin player idle and a null sink. No AC-4 stream has yet reached a board through a
   group, since none can be sent. The nearest evidence is two S3s playing E-AC-3 JOC as a group
   for ten minutes with no underrun.

Applied to the tables above:

| Part | Decodes itself (listed) | Hearth decodes, sends PCM |
|---|---|---|
| **P4** | 2.0 SIMPLE, 2.0 A-SPX, 2.0 through the converter, 5.1 SIMPLE, 5.1 A-SPX, 5.1 A-CPL 2 | 5.1 A-CPL 3, all 5.1.4 (core decoding to 2.0 needs one more run: 1.05 to 1.39 is not enough) |
| **S3** | 2.0 SIMPLE | Everything else: 2.0 A-SPX at 1.03 to 1.09 is the closest and fails the line |
| **C6** | Nothing | All AC-4 |

For AC-3 and E-AC-3 the same rule gives the C6's cap a **measured** basis instead of the 2026-09-22
one, and, once there is a PCM leg, sends a 5.1 stream to the C6 as stereo PCM rather than as
silence.

### Two things the rule needs that are not built

These are the open choices for the owner. Neither changes the rule.

**1. How a class is listed.** Either:

- (a) **Per class**, an additive key on the role's support object. The spec's Versions section
  covers a new `data_types` value and a new `decoder` key, on the ground that a server only sends
  what a sink listed; a new support key needs the same ruling. The sink says, for `ac4`, the
  channel modes and tool sets it takes. Hearth reads the stream's class and sends bursts only to a
  member whose set holds it. **Recommended**: it is the only form that keeps the S3's 2.0 SIMPLE
  and the P4's 5.1 on the board. It needs the inspector to report the codec tools (above) for the
  P4's A-CPL 2 and 3 to be told apart.
- (b) **Per data type**, as now: list `ac4` only on a part that decodes every class Hearth can
  send. None of the three does (the P4 fails A-CPL 3 and 5.1.4), so no part would list it and
  nothing would decode on a board.

**2. How PCM reaches an extension-role sink.** Either:

- (a) **Add a `pcm` data type to the extension role.** A new `data_types` value is what the
  Versions section already allows. One role stays active, and the PCM can be as wide as the sink's
  slots. The board's burst player needs a PCM path of its own on this role (`start_pcm` is the
  `player@v1` role's today, `sendspin.cpp:294`), and the server has to send PCM to a member that
  `bursts`, which `push()` now skips. **Recommended.**
- (b) **Move the board to `player@v1` for the items it cannot decode**, and back to the extension
  role for the next. The sink already offers `player@v1` with PCM (Music Assistant uses it), and
  both ends' session code already handles the active roles changing on a live connection
  (`server_session.cpp:700-725`, `player_session.cpp:556`). It is a server-side change only. **The
  session layer was checked on 2026-10-10**: `test_sessions.cpp`, "a player moves from bursts to
  PCM and back on one connection per item", carries E-AC-3 bursts, then PCM after the move to
  `player@v1`, then bursts again, with the right refusals while the other role is active and the
  chunk times unchanged by the moves, at two clock offsets. It passes. What it does not cover is
  `server_host.cpp`'s group (which picks the role today from the hello alone) and the board's own
  host reacting to a move. `producible()` (`server_host.cpp:74`) takes a member's format
  only when its channel count **equals** the group's, and the sink advertises 2 channels. A 5.1
  group would therefore send a stereo sink nothing, not a fold, until the group folds per member.

A third item is a number and not a design: the bar in criterion 2. 0.95 is a proposal, not a
measurement, and it is what puts the S3's 2.0 A-SPX (1.03 to 1.09) on the PCM side and the P4's
5.1 A-CPL 2 (0.91) on the board.

## Build order: everything plays first, offload second

The rule above is two jobs, and only the first is needed for "play everything everywhere":

| Step | What it gives | What it needs |
|---|---|---|
| **1. A PCM leg** | Every stream plays on every board. Hearth decodes what the board cannot take, and the board plays samples | The server sends PCM to an extension-role member (2(a) or 2(b) above); a per-member fold so a stereo board takes a 5.1 group; the sink saying what it takes (a per-type channel limit, which the C6 already has inside the firmware and never tells Hearth). **No board change for AC-4**: the board's Sendspin burst player has no AC-4 path at all (`burst_player.cpp` mentions neither AC-4 nor `ac4`), so no board lists `ac4` and every AC-4 item goes as PCM |
| **2. Offload** | A board decodes AC-4 itself where it keeps up (the P4 first, at 2.0, which every measured 2.0 stream keeps up with: 0.28 to 0.69) | I6 proper: an AC-4 path in the burst player, `ac4` listed with a limit, the 16 to 43 ms frame sizes in the queue, the latency trim. Then the codec-tool level in the inspector, to move the P4 to 5.1 and the S3 to 2.0 SIMPLE |

Step 1 makes the speed problem stop being a playback problem: a part that cannot keep up simply
does not list the stream, and a speed-up moves one row from "Hearth decodes" to "the board
decodes" with no protocol change.

Checked, 2026-10-10: the session layer takes the move between roles and back (above).

**The C6 PCM run, 2026-10-10** (aiosendspin 9.1.1 as a scratch server, a paired `player@v1`
connection, stereo 24-bit 48 kHz, no flash; the C6 `hearth-db4c40` and, as a control, S3
`hearth-47b39c`). Three findings and one thing not settled:

1. **An unpaired server never gets playback.** Music Assistant is attached to both boards and
   "activated for nothing": with unpaired access off, a playback activation on the Sentinel PSK is
   not admissible (`player_session.cpp:51-61`), and the first holder keeps the board. A server must
   pair first, and must declare the pairing in the connect itself (`pairing_attempt=`), since a
   pairing started after a first activation of "nothing" is refused with the rest.
2. **A stock server's PCM chunk is over the C6's message cap and kills the connection.**
   `sdkconfig.sendspin-c6` sets `MAX_CHUNK_BYTES=4096`, so the host takes 4,224 bytes at most
   (`sendspin_host.cpp:729`). The SDK's 25 ms chunk of 24-bit stereo is 7,225 bytes: the board
   logs "a 7225-byte message is more than this board takes" and closes, on every attempt. 10 ms
   chunks (2,905 bytes) were taken. **So the fallback has to chunk to the sink's own limit**, and
   the sink should advertise that limit; Hearth controls its chunking, a stock server does not.
3. **The ring was never the constraint.** At 10 ms chunks the C6's ring high-water mark was 8,796
   of 49,152 bytes, `late=0 dropped=0 invalid=0`, the decode task took 2.1 ms a 10 ms chunk (21%
   of the one core, worst 5.2 ms) and the heap's least was 64.7 KB.
4. **The rig could not answer whether a C6 sustains the rate.** That run also had one underrun,
   one resync, 843,264 silence frames and 1,582,304 frames skipped as late. The same rig against
   S3 .45 showed the same server-side stalls (`send_bytes` taking 0.6 to 1.0 s, 479 chunks
   skipped as late) while the S3's own counters stayed at zero underruns, late and dropped, and
   the path is clean (10 GbE wired, 40 of 40 pings, at most 45 ms). A Python asyncio sender on
   Windows cannot hold a steady 100 messages a second, so Hearth's own C++ server was used
   instead: [the first slice](#the-first-slice-built-and-measured) answers it.

Both boards were restarted afterwards so Music Assistant re-attached; each pairing record the run
made was forgotten, and `GET /pairing` is back to what it was.

## The first slice, built and measured

Built on 2026-10-10 with Hearth's own C++ server as the sender; the library and firmware parts
below have since merged (#1248, #1251, #1253, #1257), and the C6 account under it was **corrected on
2026-10-11** (see [the real limit](#the-real-limit-on-the-c6-2026-10-11)).

| Part | What | Where |
|---|---|---|
| Library | `ServerHost::use_pcm(client, use, max_chunk_bytes)`: a paired board that offers both roles is moved to `player@v1` on the live connection, and its PCM units are sized to its message limit | `libs/sendspin/src/server_host.cpp` and `.hpp` |
| Library | **A bug fix**: the group counted every unit it had sent as held 150 ms (`kLongestUnit`), so a small sink's modelled buffer filled with units that had already played and each chunk was sent after its play time. A PCM unit now counts as held for what its bytes last | `Group::push` |
| Test sink | `SinkOptions::buffer_capacity`, so a test can play to a board-sized buffer | `apps/hearth/testsink` |
| Test server | `--pcm`, `--max-chunk-bytes`, `--tone`, `--tone-bits`; pairs by code with `dial_to_pair()`; reports the board's PCM counters and its `sendspin.stream=` totals | `apps/hearth/testserver` |
| Firmware | A sink lists its own slot depth first (16-bit on the C6), since a server sends the first format it can | `firmware/hearth-sink/.../sendspin.cpp` |

Tests added, all passing: the session moves bursts to PCM and back (`test_sessions.cpp`); a paired
sink is moved to `player@v1`, played PCM in chunks inside its limit, and moved back
(`test_group.cpp`); **a small-buffer sink is fed before play and not after**, which measured -66 ms
before the fix and passes after it (`test_group.cpp`); and the playout on a model of the I2S ring
recovers a PCM stream from a clock step, a late chunk and late interrupts, and skips whole a stream
whose chunks stay late (`test_playout.cpp`). The suites: sendspin 224 cases, device 151, group 14.

### What the C6 did

Same server, same 48 kHz stereo tone, the C6 paired by its code, PCM over `player@v1`:

| Run | Chunk | Of the chunks sent, received | Frames played | Underruns |
|---|---|---|---|---|
| Python SDK, 10 ms chunks, 24-bit | 480 frames | most | about 0% | 1, then skipped to the end |
| C++ server, 24-bit, 666 frames, before the fix | 666 | 80 to 88% | about 1% | 1 to 2 |
| C++ server, 24-bit, after the fix | 666 | 88 to 96% | 1 to 2% | 2 to 5 |
| C++ server, 16-bit, after the fix, no polling | 1,000 | 100% | **54%** | 29 |
| The S3 on the same server, 24-bit | 666 | 100% | settled, error 4 us | 7 early, then none |

The board's own counters (`silence_frames`, `skipped_frames`) say the same: of a 20 s stream at
24-bit the C6 skipped 18.8 s as late.

### Why

1. **The fix above is real and not enough.** Before it, chunks reached a small sink after their
   play time. After it, the group sends them 70 to 110 ms ahead.
2. **A playout that finds its chunks later than they are due skips them, and a short stall starts
   it.** The board's trace (a temporary build, since removed) shows the first chunk reaching the
   playout only 56 ms before it plays, the sink running dry about 0.27 s in (the writer 20 ms late),
   and then every re-align finding its chunk already late by 14 ms, then 20, 30, ... and 200 ms and
   growing, so every frame is skipped. The host model reproduces exactly that signature
   (one underrun, one resync, nearly all frames skipped) for a stream that stays late, and recovers
   from a single late chunk or step, so the playout's logic is sound: the stream really is late.
3. **The C6 cannot sustain the PCM path as it is.** At 16-bit the player spends about 5.3 ms
   rendering and 5.6 ms in the sink for each 20.8 ms chunk, about half of a single 160 MHz core with
   no FPU, beside Wi-Fi and the Noise decryption. A 64 ms DMA queue runs dry about every 0.7 s.
   At 24-bit the board does not even receive every chunk. The S3, with a PSRAM ring and a second
   core, plays the same stream. *(The conclusion stands; the cause given here, the CPU, does not.
   The player's time is 1.9 ms a chunk once the unpack is integer, and the board still runs dry:
   [the real limit](#the-real-limit-on-the-c6-2026-10-11).)*
4. **The lead is thin by construction.** The sink says it needs 250 ms between a chunk's arrival
   and its first sample (`required_lead_time_ms`) and its 48 KB ring holds 167 ms of 24-bit stereo,
   so the group can send at most about 110 ms ahead. Opening the sink on the first chunk eats most
   of the 400 ms the timeline starts with, and what is left (56 ms) is less than a re-prime.

### What it means

- **A C6 should not be offered the PCM fallback yet.** It stays coded-only, and a stream it cannot
  decode is refused with its reason rather than played badly. The S3 and the P4 can take it.
- **What would change that** was thought to be a lean PCM path on the C6. One was built (the unpack
  and the meter in integers) and it took the player's time from 5.4 to 1.9 ms a chunk, and the
  board still ran dry: the limit is how much time the ring holds against the gaps in the WiFi link,
  not the CPU. [The real limit](#the-real-limit-on-the-c6-2026-10-11) has the numbers and what
  would change it.
- **The sink must advertise its message limit**, and the group must chunk to it. Today the limit
  is told to the host out of band (`use_pcm(..., max_chunk_bytes)`); a field in the support object
  is the real form.
- **The playout could be kinder to a late stream**: a re-align that costs more writer time than the
  lead leaves cannot recover, so a stream that stays late is better ended with a reason than skipped
  frame by frame for the rest of the play.

### The real limit on the C6 (2026-10-11)

Measured on the C6 `hearth-db4c40` (RSSI -60 dBm, 2.4 GHz, `Stresemannstrasse-IoT`), Hearth's test
server playing a 15 s 48 kHz stereo tone, with temporary traces in the firmware (since removed).
The link's quality moved from minute to minute, so each row below is a count of runs.

| What | Measured |
|---|---|
| Player time per 1,000-frame chunk, before the integer unpack / after | 5.4 ms (27% of the core) / 1.8 to 2.0 ms (9%) |
| Underruns in 15 s, 16-bit, 4,000-byte (1,000-frame) chunks, 15 runs on six images | 10 to 65, median 22 |
| The same with 2,000-byte (500-frame) chunks, 6 runs | 1, 1, 2, 2, 9, 35 |
| The same with the 24-bit format listed first (666-frame chunks in 4,000 bytes), 5 runs | 1, 1, 1, 4, 4 |
| 4,000-byte chunks, TCP window 5,760 -> 11,520 bytes and receive mailbox 6 -> 16, 3 runs | 5, 7, 7; the heap's least 96 KB -> 75 KB |
| The CPU during a 320 ms stall | idle for 70 to 90 ms of every 100 |
| A message's payload after its header | 30 to 60 ms in a bad minute, 200 to 320 ms about twice a second |
| What the ring holds | 48 KB; a server fills three quarters: 36,864 bytes = **192 ms** of 16-bit stereo (128 ms of 24-bit), against 1.65 s of 192 kbit/s AC-3 |
| Idle ping from the wired PC to each ESP32 board on that SSID | 2 ms least, 8 to 18 ms mean, 35 to 90 ms most; the wired router 0 to 1 ms |

What follows from it:

1. **The player was never the limit.** The server's task on the board was blocked in the read of
   a message's payload while the CPU idled: the bytes had not arrived, in the middle of a message,
   which is a segment held up or lost and retransmitted. Every board on that SSID shows the link's
   jitter, the S3 and P4 too. They play the same stream because their rings, in PSRAM, are 256 KB
   (about a second of PCM at the three quarters a server fills), where the C6's holds 192 ms.
2. **A gap longer than the ring is a drop-out, and the playout does the right thing about it**: the
   chunks behind the gap are late by then, so it skips them rather than playing the stream late,
   about 0.2 s of audio a gap. The 15 s runs skipped 6 to 12 s of it in a bad minute.
3. **A bigger TCP window and receive mailbox take about two thirds of the stalls, at a price the C6
   cannot pay.** Raising them (`CONFIG_LWIP_TCP_WND_DEFAULT` 5,760 -> 11,520 and
   `CONFIG_LWIP_TCP_RECVMBOX_SIZE` 6 -> 16) took 17 to 23 underruns to 5 to 7 on the same stream (on
   a base a few commits later) and the heap's least from 96 KB to 75 KB, on a part where the coded decoders abort for want of
   heap. Not raised.
4. **The ring cannot be made deeper.** `docs/platforms/bare-metal/esp32-c6.md` has the 64 KB ring
   failing on the heap (two WiFi receive allocations refused, `heap_least` 4,792 bytes). The 1.65 s
   AC-3 gets from the ring would take 317 KB of PCM, 8.6 times it. Lossless packing (FLAC) would
   give about 1.7 times the depth for music, not 8.
5. **A shorter chunk costs nothing and helps.** The underruns fall with the chunk's duration:
   500-frame chunks gave a median of 2 against 22 for 1,000-frame ones across the afternoon, and
   1, 9 and 35 against 17, 18 and 23 alternated on one image. That is also why the 24-bit-first listing did better in the earlier runs: at the same 4,000
   bytes it sends 666 frames a chunk, not 1,000. **The listing is not the cause and the chunk
   duration is**, so #1251's 16-bit-first order is right (it sends fewer bytes) and the fix is to
   size a PCM unit by duration: the library now sends 10 ms units to a sink whose buffer holds
   under a second of the stream (#1275), where `use_pcm(client, true, 0, width)` took the board's
   whole 4,096-byte limit. No firmware change.

So the C6 is a board that plays PCM on a clean link and drops out on a busy one, and a 10 ms unit
moves the line without a byte of firmware.

**The owner's decision (2026-10-11): the C6 is offered the PCM fallback, with a caveat in its
reason (#1275), and a lossy Opus leg is added beside it.** The caveat is shown in the group
editor and the log: "This board holds under a fifth of a second of PCM, so on a busy WiFi the
stream can drop out."

### Opus on the C6, measured (2026-10-11)

libopus 1.6.1, fixed point (`FIXED_POINT`, no float API), `-O2`, in the Hearth sink image with
WiFi up and nothing playing, 48 kHz stereo, 20 ms packets of a music clip at CBR, 200 packets a
set, three sets (the range across them is given):

| What | 128 kbit/s (CELT fullband) | 64 kbit/s |
|---|---|---|
| Decode time per 20 ms packet, mean | 7.5 to 7.8 ms (**37 to 38% of the core**) | 6.4 to 7.2 ms (32 to 36%) |
| Worst packet | 11 ms (one of 600 at 51 ms, under WiFi) | 9.4 to 11.5 ms |
| Decoder state | 27,060 bytes of internal heap | the same |
| Stack | about 16.4 KB (a 16 KB task overflowed) | the same |
| Bytes a second | 16,000, a twelfth of 16-bit PCM | 8,000 |
| What the 48 KB ring holds | about 2.3 s (1.7 s at the three quarters a server fills) | twice that |

So the part decodes it with room, and the ring holds as much of it as AC-3, which the link's
gaps do not reach. What it costs is 27 KB of heap for the decoder while an Opus stream plays, and
a stack of 17 KB, which the 24 KB decode task already has (a PCM stream does not use it), against
about 120 KB free in the idle image (79 KB with the benchmark's own 40 KB stack, which is what the
figures above were taken under); and the quality: Opus is lossy, so a stream decoded here and sent
as Opus is decoded, encoded and decoded again. It is a choice a person makes for a board on a busy
link, not what a board is sent by default.

**Built and played, 2026-10-11**: the player decodes Opus (`CONFIG_ICLFORGE_SENDSPIN_OPUS`,
`firmware/esp-idf/iclforge/src/opus_stream.*`, `opus.cmake`), the board lists it after its PCM
formats, and `ServerHost::use_pcm` takes the codec to play a sink in. Hearth's test server played
30 s of tone as Opus to the C6, three runs: 0 underruns, late, dropped or invalid; the player's
time 11.8 ms a packet (59% of the core), the heap's least 51 KB, the decode stack 7.9 KB to
spare. Alternated with PCM streams in the same minutes: Opus 0 and 0 underruns; PCM 14 and 17.
What is left is the choice: the engine offering it per sink, and the group editor's control.

## What this means for the status rows

The question asked was whether the S3's limits and the C6 should be 🟡🔵 (a known, accepted gap).
The two halves are different things and the row should say so:

| Half | What it is | Mark |
|---|---|---|
| **The on-board decode limit**: S3 not real time wider than 2.0 SIMPLE, C6 does not fit | Physical, measured on the boards, and decisions 28 and 32 already took it | **🟡🔵**: accepted |
| **What the sink does with the rest** | Today: nothing is sent. Decision 32 says PCM, decision 39 says nothing, and the code says nothing. It is I6's | **🟡**, not blue: the owner has accepted PCM, which does not exist, and not silence, which does |

Blue on the second half would record a gap as accepted that is in fact the opposite of what was
accepted.

## Wording that disagrees with the code, to fix once the criteria are chosen

Each says a paired board is sent PCM for AC-4, or leaves that to decision 32:

- `planning/ac4.md`, decision 32 (a): "where it misses, Hearth sends the C6 PCM, which it plays
  today". Decision 39 and the I2 note in the same file say the opposite.
- `docs/hearth/sink-esp32-c6.md`: "A C6 sink takes AC-4 programmes from Hearth as PCM."
- `docs/platforms/bare-metal/esp32-c6.md`: "so a C6 sink takes AC-4 programmes from Hearth as PCM",
  and "AC-4 reaches a C6 sink as PCM from Hearth, which is decision 32's plan".
- `docs/platforms/bare-metal/esp32-s3.md`, "What the board's timers leave": "Decision 32 made the
  call for the C6: Hearth sends PCM."
- The Cross-cutting status row for the minimum-footprint decoder, from the same session.

Decision 32's own premise holds: the C6 should not decode AC-4. What does not hold is the second
half of the sentence.

## Where this was read

| Claim | Source |
|---|---|
| Sinks list `ac3`, `eac3` | `firmware/hearth-sink/main/sendspin/player/sendspin.cpp:159` |
| PCM role stereo only; offered by default | `sendspin.cpp:204-215`, `Kconfig.projbuild:665` (`ICLFORGE_EXAMPLE_SENDSPIN_PCM`, default 1) |
| Unpaired access off by default | `Kconfig.projbuild:656` (`ICLFORGE_EXAMPLE_SENDSPIN_UNPAIRED_ACCESS`, default 0) |
| Codec tools not in the public API | `libs/ac4/include/iclforge/ac4/core/toc.hpp:99-108` (channel mode only); `aspx`/`acpl` only in `encoder/config.hpp` |
| Role activation | `libs/sendspin/src/server_host.cpp:312-346` |
| Member start, bursts only for the extension role | `server_host.cpp:1301-1353` |
| PCM never pushed to a bursts member | `server_host.cpp:1682` |
| Format not listed | `libs/sendspin/src/server_session.cpp:869-873` |
| Channel cap | `firmware/esp-idf/iclforge/src/burst_player.cpp:780`, `sdkconfig.sendspin-c6:35` |
| AC-4 refusals on the HTTP path | `firmware/esp-idf/iclforge/src/player.cpp:721-727, 819-828, 920-934` |
| S3, C6 and P4 figures | `docs/platforms/bare-metal/esp32-s3.md#on-the-board`, `esp32-c6.md#on-the-board`, `esp32-p4.md#what-it-decodes-in-real-time` |
| Decisions | `planning/ac4.md`: 28, 29, 30, 32, 39; I2 and I6 |
