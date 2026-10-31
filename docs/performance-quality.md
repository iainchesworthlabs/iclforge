# Performance & quality

CI records performance, quality, and memory measurements after merges and every
night. This page shows the latest values from `main`, explains each measure, and
links to the append-only histories. The line under the cards names the commit the
newest figures were measured on and says how old it is;
[Where each number comes from](#where-each-number-comes-from) says which job
measures each series and when.

Each card has one line per codec (AC-3, E-AC-3, and AC-4), and the speed, allocation and live-growth cards add a TrueHD line once the series holds a TrueHD workload. A figure is the worst case among that
codec's own workloads, except on the listening-quality card, which gives the range, and the
workload it comes from is named beside it. The figures of two codecs come from different workloads
and do not compare with each other. Each series is written by its own job, so a line recorded on
an older commit than the one named under the cards says which commit and day it was recorded on.
The speed and allocation series cover all three codecs and the
Atmos object layer. AC-4's decode quality is a series of its own on
[Quality trend](quality-trend.md#ac-4-decode-quality) with no per-channel floors yet, so its figure
has no colour. The listening-quality card scores the encoders of all three codecs: AC-3 and E-AC-3
from the [tool comparison](tool-comparison-trend.md), AC-4 from its own series,
[AC-4 encode quality](quality-trend.md#ac-4-encode-quality).

**TrueHD is in the speed, allocation and live-growth series, and in no quality series.** `iclforge-perf`,
`iclforge-bench`, `iclforge-membench` and `iclforge-kernelbench` measure it
([what is measured](performance-trend.md#what-is-measured-and-on-what)), and a lossless decoder has no quality
figure to trend: its check is that FFmpeg's strict decode equals the source ([Validation: TrueHD](verification.md#truehd-and-mlp)).
Its line appears on the cards once the trend history holds a TrueHD row. Instruction counts and image sizes on the small parts, and the
per-platform pages, are on the [roadmap](https://github.com/iainchesworthlabs/iclforge/blob/main/ROADMAP.md#truehd-and-e-ac-3-as-equals-proposed).

New to codec benchmarks: start with
[How to read these numbers](#how-to-read-these-numbers).

<div id="pq-status" class="pq-grid">
  <p class="pq-loading">Loading the latest measurements from <code>main</code>…</p>
</div>

<p class="pq-asof" id="pq-asof"></p>

## The three questions

The measurements answer three questions. Speed, audio quality, and memory use
are tracked separately because a change can improve one while making another worse.

### Is it fast enough?

Encoding and decoding are measured in **milliseconds per frame**, then expressed
as a multiple of **real time**. One AC-3 frame carries 32 ms of audio, so a
frame that takes 0.32 ms to encode runs at 100x real time: a minute of audio in
0.6 seconds. An AC-4 frame carries 2,048 samples, 42.67 ms at 48 kHz, and its
multiple is taken against that.

The Speed card shows the slowest workload of each codec, encode or decode, and
says which it is. Encoding is the slower direction by a wide margin, so it is the
one shown in practice.

Each line also names the CPU model it was measured on. The `linux-gcc` leg runs on
GitHub's hosted runner, which is one of several models from run to run, and the
card shows the newest run, whichever machine it had. Over the fourteen runs to
2026-10-12 AC-3 read 7x, 9x and 10x and E-AC-3 4x, 5x and 6x, while the eight of
those runs that had the same machine agree to within 0.4%. When the x86 machine is
a fast one the arm64 leg is the slower of the two, and its figure is the one shown.
The README's speed badge carries the same figures and has no room for the name.

Anything above 1x is fast enough to keep up with playback. The margin above that
is what buys you batch transcoding, live capture with headroom, and running on a
Raspberry Pi rather than a workstation.

→ [Performance trend](performance-trend.md) tracks this per workload, per
kernel, and alongside memory use, and compares each CPU model with itself.

### Does it sound right?

Two measures are used.

**SNR** (signal-to-noise ratio, in dB) compares the decoded waveform against the
original sample by sample. Higher is better. It is objective and unforgiving,
and it is the right tool for catching a decoder that has broken —
but it punishes techniques that are *designed* to discard inaudible detail, so a
low SNR is not automatically a quality problem.

!!! note "What the SNR number on this page is actually measuring"
    Most checks behind the **Decode accuracy** card compare this decoder with
    FFmpeg's decoder for the same bitstream. Where FFmpeg cannot read a stream,
    and for every AC-4 stream, the check scores this decoder against the source
    the stream was encoded from. Neither measures perceived audio quality. The
    inputs include bitstreams produced by Dolby's encoder.

    That distinction matters for reading the number. Perfect agreement would be
    limited only by floating-point noise, and on the channels that carry most
    of the audio it very nearly is: 57–88 dB, run after run, on every platform.
    Where the two decoders diverge, it is usually not because either is wrong.
    A/52 §7.3.4 says a decoder may substitute *"any reasonably random
    sequence"* for the bins the encoder spent no bits on, so two
    spec-correct decoders are *required* to differ there — and those bins
    cluster in the surround channels, which is why a 5.1 fixture's Ls/Rs land
    around 22 dB while its centre channel sits near 58 dB.

    So: a low number on one channel of one fixture is a statement about how
    much freedom the standard leaves, not about audio quality. Read
    [Quality trend](quality-trend.md) for what the number does over time, which
    is the part that carries information, and see
    [Validation](verification.md#what-would-make-these-numbers-excellent) for
    what it would take to remove that spec-permitted divergence from the
    measurement entirely.

**MOS-LQO** (1 to 5) is a prediction of what a listening panel would say,
produced by [ViSQOL](https://github.com/google/visqol). It models hearing rather
than arithmetic, so it credits a stream that sounds right even where the
waveform has moved.

→ [Quality trend](quality-trend.md) tracks the gold-reference SNR gate,
[AC-4 decode quality](quality-trend.md#ac-4-decode-quality) and
[AC-4 encode quality](quality-trend.md#ac-4-encode-quality).
→ [Tool comparison trend](tool-comparison-trend.md) tracks per-tool quality.
→ [Object quality trend](object-quality-trend.md) covers Atmos objects.
→ [Landscape](landscape.md) puts this encoder beside FFmpeg's and Dolby's.

### Does it stay within its budget?

Two cards come from the allocator. **Allocation per frame** counts allocator
traffic: the bytes and calls the heap served over a frame, whatever became of
them. It is not the memory a codec occupies, because a buffer allocated and freed
every frame counts every time and is held by none. No gate sits on it, so its
card has no colour. **Live growth** is what is still held after the steady-state
frames (199 of a 200-frame run for most workloads). A couple of KiB of retained
working state is expected; CI warns at 4 KiB and fails the build at 1 MiB, and
the card turns amber at the same 4 KiB line, so its colour says which workload
retains memory. Peak resident memory is recorded as well.

A codec that leaks, or whose working set grows with the length of the file, fails
on long content and on small devices even when it is fast.

→ [Performance trend](performance-trend.md#memory-trend) carries the memory tables.

## Where each number comes from

Nothing on these pages is typed in. Each page fetches a history file from the
`quality-history` branch when it is opened and draws its figures from the records
in it, and a record carries the commit it was measured on and that commit's date.
The files are written by jobs of `ci.yml`, the run that verifies `main`:

| Series (file on `quality-history`) | Measured by | Runs | Runner |
| --- | --- | --- | --- |
| Speed, per workload (`performance-main.jsonl`) | `Publish performance trend`: builds `iclforge-bench` with GCC 16, runs it three times and records the fastest; the arm64 figures come from `Publish performance trend (arm64 measurements)` | After a merge, nightly, on request | GitHub-hosted `ubuntu-latest` and `ubuntu-24.04-arm`, in an `ubuntu:26.04` container |
| Speed, per kernel (`kernels-main.jsonl`) | The same job, `iclforge-kernelbench` | The same | `ubuntu-latest` |
| Allocation and live growth (`memory-main.jsonl`) | The same job, `iclforge-membench` | The same | `ubuntu-latest` |
| Decode accuracy (`main.jsonl`) | The gold-reference gate of each build leg; `Publish quality trend` appends the legs whose gates passed | After a merge (the legs the merges touched), nightly (every leg), on request | The Linux and Windows legs normally on the self-hosted fleet, macOS and arm64 on GitHub-hosted runners |
| Tool comparison and listening quality (`external-comparison-main.jsonl`) | `FFmpeg Validate` scores this build's encodes (SNR, LSD and ViSQOL's MOS-LQO, `tools/ci/quality_race.py`); `Publish external-encoder landscape trend` appends them beside the committed FFmpeg and Dolby figures | Nightly, on request | Normally self-hosted Linux |
| AC-4 decode quality (`ac4-quality-main.jsonl`) and AC-4 encode quality (`ac4-encode-quality-main.jsonl`) | Two scoring steps of `FFmpeg Validate`, appended by the same publisher | Nightly, on request | Normally self-hosted Linux |
| Object reconstruction (`object-quality-main.jsonl`) | The last step of `FFmpeg Validate`; `Publish object-reconstruction quality trend` appends it | Nightly, on request, and only after an `FFmpeg Validate` that passed | Normally self-hosted Linux |
| The README's badges (`badges/*.json`) | `Publish performance trend`, from the series above | With the speed series | `ubuntu-latest` |

"After a merge" is the run of `ci.yml` on a push to `main`. One runs at a time, so
when merges come faster than a run takes (about an hour), one run stands for all
the merges since the run before it, and its records carry the newest of those
commits. "Nightly" is the scheduled run of the whole matrix. "On request" is
`gh workflow run ci.yml --ref main`, which runs the whole matrix on the tip of
`main` and records every series: it is how a full set of records is taken without
waiting for the night. A series holds a commit once, so a second run of a commit
adds only what the first did not record.

Each publisher depends on the job that measured its figures and on nothing else.
A leg that failed leaves a gap in its own series for that commit, and every other
series is still written. When no run has recorded anything for a while, the pages
go on showing the last figures: the line above each series gives their commit and
age, and turns amber after two days for the series written after merges and three
for the nightly ones.

The figures on the ESP32 pages are not part of this. They are measured on real
boards with `tools/hearth/board_suite.py`, by hand, and kept as baselines under
`firmware/hearth-sink/baselines/`; the emulated ESP32 legs of CI hold a build to
its size, its levels and its heap floor and publish nothing.

## What happens when a number moves

Thresholds separate expected run-to-run variation from regressions:

| Tier | What it means | What happens |
| --- | --- | --- |
| Noise | Under ~3%, or inside the run-to-run spread | Reported as unchanged |
| Soft | 20% slower, or 0.5 dB of quality lost | Warning on the run; merge proceeds |
| Hard | **Twice** as slow, quality 10 dB below its trailing average (3 dB for AC-4), or a gate floor breached | **Fails the build** |

Quality is gated **per channel**, not once per file. Each channel of each
fixture has its own floor, derived from that channel's own lowest measurement
across every platform and every commit on record
(`tools/checks/derive_channel_floors.py` is the derivation, and it is a script
rather than a judgement call so a floor move is reviewable as a diff against
evidence). The trend check works the same way: a channel is compared against
its own trailing average, so a front channel slipping is caught even while the
surrounds — which sit far lower by design — have not moved at all.

Per-channel floors closed a hole. One floor per fixture had
to be low enough for the lowest channel to pass, which on the 5.1 fixtures
meant a floor set by the dither-dominated surrounds: 22 dB, against a centre
channel measuring 58 dB. The centre could have collapsed by 36 dB and the gate
would still have gone green. Each floor is now its channel's lowest measurement
minus 1 dB, rounded down, so the same fixture fails on a drop of about 1 dB in
**any** channel.

Timings come from shared CI runners, so a single slow run is not evidence of
anything. Every published number is the fastest of several repetitions, and the
trend pages show the run-to-run spread beside it so you can tell a real move
from a noisy one.

## How to read these numbers

A short glossary, in the order you are likely to meet them.

**Frame** — the unit a codec codes audio in. An AC-3 or E-AC-3 frame is 1536
samples, 32 ms at 48 kHz; an AC-4 frame at the default frame rate is 2048 samples,
42.67 ms. Nearly every measurement here is "per frame" so that it does not depend
on how long the test file happens to be.

**Workload** — one benchmarked configuration: a codec, a channel layout, and a
direction (encode or decode), such as AC-4 5.1 encode. Every figure on this page
names its workload, because two workloads of one codec can differ by an order of
magnitude in time and in allocation.

**x real time** — how much faster than playback. 100x means one minute of audio
is processed in 0.6 seconds. Below 1x means it cannot keep up live.

**ms/frame** — the same number before the division. Lower is better.

**SNR (dB)** — how closely two pieces of audio match, sample by sample. Higher
is better, and every 6 dB is roughly one more bit of accuracy. What the two
pieces *are* decides how to read it. Against the original recording it measures
coding loss (where below ~20 dB is usually audible and above ~40 dB usually is
not). Between two independent decoders given the same bitstream, as in most
checks behind this page's Decode accuracy card, it measures agreement: a
different question, with a different scale, explained in the note above.

**Floor** — the SNR a given channel of a given fixture must stay above. One per
channel, not one per file, and each derived from that channel's own measured
history rather than chosen. The surrounds of a 5.1 fixture have much lower
floors than its front channels because the standard permits decoders to differ
more there, not because less is expected of them.

**Headroom** — how far a channel currently sits above its own floor. This is
the number to watch: the raw SNR says how two decoders compare, headroom says
how close the gate is to firing. A channel with a low SNR and healthy headroom
is behaving exactly as designed.

**MOS-LQO** — predicted listening quality from 1 to 5, where 5 is
indistinguishable from the original. Roughly: **4-5** excellent, **3-4** good,
**2-3** fair, **under 2** poor. A low bitrate legitimately scores lower — the
point is whether it *changes*, not whether it is 5.

**LSD (dB)** — log-spectral distance: how far the decoded *spectrum* has moved,
rather than the waveform. Lower is better. It is the fairer measure for coding
tools that deliberately resynthesise a band instead of reproducing it.

**Allocation per frame** — the bytes requested from the heap over one frame in
steady state. Allocator traffic, not memory in use.

**Live growth** — the bytes a workload still holds after its steady-state
frames. CI warns at 4 KiB and fails the build at 1 MiB.

**Leg** — one platform-and-compiler combination (for example `linux-gcc`,
`windows-msvc`). The same code is measured on several, because a change can help
one and hurt another.

**Gate** — a threshold CI enforces. A number below its floor (or above its
ceiling) fails the build rather than merely being recorded.

<style>
.pq-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 0.9rem; margin: 1.4rem 0 0.4rem; }
.pq-card { border: 1px solid var(--md-default-fg-color--lightest); border-radius: 6px; padding: 0.85rem 1rem; }
.pq-card h3 { margin: 0 0 0.15rem; font-size: 0.78rem; font-weight: 600; text-transform: uppercase; letter-spacing: 0.04em; color: var(--md-default-fg-color--light); }
.pq-sub { font-size: 0.78rem; color: var(--md-default-fg-color--light); margin: 0 0 0.35rem; }
.pq-line { display: grid; grid-template-columns: 4rem 1fr; column-gap: 0.6rem; align-items: baseline; padding: 0.4rem 0; border-top: 1px solid var(--md-default-fg-color--lightest); }
.pq-codec { grid-column: 1; grid-row: 1; font-family: var(--md-code-font-family, monospace); font-size: 0.72rem; font-weight: 600; color: var(--md-default-fg-color--light); }
.pq-figure { grid-column: 2; grid-row: 1; font-size: 1.5rem; font-weight: 700; line-height: 1.15; }
.pq-figure-watch { color: #ef6c00; }
.pq-unit { font-size: 0.95rem; font-weight: 500; color: var(--md-default-fg-color--light); }
.pq-what { grid-column: 2; grid-row: 2; font-size: 0.76rem; color: var(--md-default-fg-color--light); }
.pq-note { font-size: 0.78rem; color: var(--md-default-fg-color--light); margin-top: 0.3rem; }
.pq-badge { display: inline-block; font-size: 0.7rem; font-weight: 700; padding: 0.05rem 0.4rem; border-radius: 3px; vertical-align: middle; margin-left: 0.35rem; }
.pq-ok { background: rgba(67, 160, 71, 0.16); color: #2e7d32; }
.pq-watch { background: rgba(251, 140, 0, 0.18); color: #ef6c00; }
.pq-unknown { background: var(--md-default-fg-color--lightest); color: var(--md-default-fg-color--light); }
.pq-loading, .pq-asof { color: var(--md-default-fg-color--light); font-size: 0.82rem; }
.pq-asof-stale { color: #ef6c00; font-weight: 600; }
[data-md-color-scheme="slate"] .pq-ok { color: #81c784; }
[data-md-color-scheme="slate"] .pq-watch, [data-md-color-scheme="slate"] .pq-figure-watch,
[data-md-color-scheme="slate"] .pq-asof-stale { color: #ffb74d; }
</style>

<script>
(function () {
  // Self-contained, like every other trend page here - see the note at the top
  // of performance-trend.md's script on why these pages deliberately do not
  // share a docs/javascripts asset.
  // The site is served under the repository's name, so the repository is read off the address:
  // it is right before and after the repository is renamed. A local preview, and the stub
  // tools/ci/render_measurement_tiles.js runs the scripts in, have no such address.
  const REPO = typeof location !== "undefined" && location.hostname.endsWith(".github.io")
    ? location.hostname.split(".")[0] + "/" + location.pathname.split("/")[1]
    : "iainchesworthlabs/iclforge";
  const HISTORY_BRANCH = "quality-history";

  // Every badge on this page is decided by a threshold the project ALREADY
  // enforces somewhere else, never by a number invented for a status card.
  // That rule exists because the first draft of this page invented three of
  // them and lit up "watch" on a completely healthy build - a card that cries
  // wolf teaches people to ignore it, and there is no shortage of real gates.
  //
  // So: real time is 1x by definition, the SNR gate is whatever
  // thresholds_db each record carries, and the memory line is the 4 KiB
  // steady-state retention that append_memory_history.py warns at (see
  // performance-trend.md - it warns at 4 KiB and hard-fails at 1 MiB).
  // Allocation per frame, MOS and AC-4's decode quality get no badge at all,
  // because no absolute gate exists to borrow for them.

  // >>> measurement summary
  // The text between these markers is the same in docs/javascripts/hero-stats.js and in the
  // script of docs/performance-quality.md, and tools/ci/test_write_measurement_badges.py fails
  // when the two differ or when the tables differ from tools/ci/write_measurement_badges.py,
  // which writes the README's badges by the same rules. Change all three together.
  //
  // Every line is ONE codec's and names the workload it comes from: the worst case among that
  // codec's workloads, except for listening quality, which is a range. The cards used to show
  // one figure each - "2.0 dB SNR", "2009 KB/frame" - and a reader took them for the project's
  // headline numbers: they were the tightest E-AC-3 check and the AC-4 5.1 encoder's allocator
  // traffic, which say nothing about each other.
  const CODECS = ["AC-3", "E-AC-3", "AC-4"];

  // The codecs a speed or allocation line is drawn for: the three above, and TrueHD once the series
  // holds a TrueHD workload. TrueHD is lossless, so it has no decode-accuracy or listening-quality
  // line; the cards for those two keep CODECS.
  const codecsOf = (picked) => (has(picked, "TrueHD") ? [...CODECS, "TrueHD"] : CODECS);

  // The `codec` field of a gold-reference record (main.jsonl) and of a tool-comparison one.
  const QUALITY_CODECS = { "ac3": "AC-3", "eac3": "E-AC-3" };

  // The `codec` field of a listening-quality row: a tool-comparison one (external-comparison-main.jsonl)
  // or one of the AC-4 encoder's own series (ac4-encode-quality-main.jsonl). Not QUALITY_CODECS: that
  // is the gold-reference gate's, which has no AC-4 check and must refuse a record that says it has.
  const LISTENING_CODECS = { "ac3": "AC-3", "eac3": "E-AC-3", "ac4": "AC-4" };

  // config -> [codec, what the workload is]. The names in performance-main.jsonl and
  // memory-main.jsonl, which record no codec of their own. A config missing here fails
  // loudly (UnknownWorkload) rather than being filed under a guessed codec.
  const WORKLOADS = {
    "plain_51": ["AC-3", "5.1 encode"],
    "plain_51_fast_mdct": ["AC-3", "5.1 encode, fast MDCT"],
    "ac3_51_encode": ["AC-3", "5.1 encode"],
    "ac3_51_decode": ["AC-3", "5.1 decode"],
    "eac3_51_auto": ["E-AC-3", "5.1 encode, automatic tools"],
    "eac3_stereo_auto": ["E-AC-3", "stereo encode, automatic tools"],
    "eac3_51_encode": ["E-AC-3", "5.1 encode"],
    "eac3_51_decode": ["E-AC-3", "5.1 decode"],
    "ecpl_51_encode": ["E-AC-3", "5.1 enhanced-coupling encode"],
    "atmos_4obj": ["E-AC-3", "Atmos 4-object encode"],
    "atmos_4obj_fast_mdct": ["E-AC-3", "Atmos 4-object encode, fast MDCT"],
    "atmos_4obj_qmf_fast_mdct": ["E-AC-3", "Atmos 4-object encode, QMF, fast MDCT"],
    "atmos_4obj_encode": ["E-AC-3", "Atmos 4-object encode"],
    "atmos_4obj_decode": ["E-AC-3", "Atmos 4-object decode"],
    "ac4_stereo_encode": ["AC-4", "stereo encode"],
    "ac4_stereo_decode": ["AC-4", "stereo decode"],
    "ac4_51_encode": ["AC-4", "5.1 encode"],
    "ac4_51_decode": ["AC-4", "5.1 decode"],
    "truehd_stereo_encode": ["TrueHD", "stereo encode"],
    "truehd_stereo_decode": ["TrueHD", "stereo decode"],
    "truehd_51_encode": ["TrueHD", "5.1 encode"],
    "truehd_51_decode": ["TrueHD", "5.1 decode"],
    "truehd_71_encode": ["TrueHD", "7.1 encode"],
    "truehd_71_decode": ["TrueHD", "7.1 decode"],
    "truehd_51_96k_encode": ["TrueHD", "5.1 96 kHz encode"],
    "truehd_51_96k_decode": ["TrueHD", "5.1 96 kHz decode"],
    "truehd_stereo_192k_encode": ["TrueHD", "stereo 192 kHz encode"],
    "truehd_stereo_192k_decode": ["TrueHD", "stereo 192 kHz decode"],
  };

  const LAYOUTS = { "1": "mono", "2": "stereo", "6": "5.1", "8": "7.1" };
  const CHANNEL_NAMES = { L: "left", R: "right", C: "centre", LFE: "LFE", Ls: "left surround", Rs: "right surround" };

  // append_memory_history.py's LIVE_GROWTH_WARN_BYTES, which warns at this many bytes or more
  // (and fails the build at 1 MiB). Borrowed, not chosen for a card.
  const MEMORY_RETENTION_WARN_BYTES = 4 * 1024;

  class UnknownWorkload extends Error {}

  const has = (table, key) => typeof key === "string" && Object.prototype.hasOwnProperty.call(table, key);
  const isNumber = (v) => typeof v === "number";

  // Rounded half up, as Math.round does, and the same in tools/ci/write_measurement_badges.py,
  // so a figure on a card and on the badge beside it cannot differ in its last digit.
  const round0 = (x) => Math.floor(x + 0.5);
  const round1 = (x) => Math.floor(x * 10 + 0.5) / 10;
  const round2 = (x) => Math.floor(x * 100 + 0.5) / 100;
  const oneDecimal = (x) => round1(x).toFixed(1);
  const twoDecimals = (x) => round2(x).toFixed(2);
  // A figure below 1x must not round up to it: 0.96 reads "0.96", not "1".
  const formatMultiple = (times) => (times >= 1 ? String(round0(times)) : twoDecimals(times));
  const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

  function classify(config) {
    if (has(WORKLOADS, config)) return WORKLOADS[config];
    throw new UnknownWorkload(`The workload "${config}" is not classified. Add it to WORKLOADS in ` +
      "tools/ci/write_measurement_badges.py, docs/javascripts/hero-stats.js and docs/performance-quality.md.");
  }

  function qualityCodec(rec) {
    if (has(QUALITY_CODECS, rec.codec)) return QUALITY_CODECS[rec.codec];
    throw new UnknownWorkload(`The codec "${rec.codec}" is not classified. Add it to QUALITY_CODECS in ` +
      "tools/ci/write_measurement_badges.py, docs/javascripts/hero-stats.js and docs/performance-quality.md.");
  }

  function listeningCodec(rec) {
    if (has(LISTENING_CODECS, rec.codec)) return LISTENING_CODECS[rec.codec];
    throw new UnknownWorkload(`The codec "${rec.codec}" is not classified. Add it to LISTENING_CODECS in ` +
      "docs/javascripts/hero-stats.js and docs/performance-quality.md.");
  }

  // The slowest timed workload of each codec, by how many times real time it runs: the
  // frame's own budget over its time. The slowest, not the mean: "is it fast enough" is a
  // worst-case question, and an average over ten workloads would hide one of them dropping
  // below real time behind nine that did not. Against its own budget, not by raw ms/frame:
  // an AC-4 frame is 2 048 samples (42.67 ms) against A/52's 1 536 (32 ms). Encode and
  // decode workloads compete, and the description says which one won.
  function slowestPerCodec(rows) {
    const picked = {};
    for (const row of rows) {
      if (!isNumber(row.ms_per_frame) || !(row.ms_per_frame > 0)) continue;
      const [codec, words] = classify(row.config);
      const budget = row.real_time_budget_ms_per_frame || 32;
      const times = budget / row.ms_per_frame;
      if (!picked[codec] || times < picked[codec].times) {
        picked[codec] = { config: row.config, words, times, ms: row.ms_per_frame, budget, leg: row.leg };
      }
    }
    return picked;
  }

  // tightest_headroom_db arrived with per-channel floors; records written before that carry
  // only worst_db and a scalar threshold_db, and fall back to exactly the old computation
  // rather than being dropped from the comparison.
  const headroomOf = (r) => (isNumber(r.tightest_headroom_db) ? r.tightest_headroom_db : r.worst_db - r.threshold_db);

  const layoutOf = (rec) => {
    const labels = Array.isArray(rec.channel_labels) ? rec.channel_labels : rec.channels_db;
    if (!Array.isArray(labels) || labels.length === 0) return null;
    return LAYOUTS[labels.length] || `${labels.length} channels`;
  };

  // The workload of a gold-reference check: its layout, rate, and the signal when it is named.
  const checkWords = (rec) => [
    layoutOf(rec),
    isNumber(rec.bitrate_kbps) ? `${round0(rec.bitrate_kbps)} kbps` : null,
    String(rec.check).includes("transient") ? "transient" : null,
  ].filter(Boolean).join(" ");

  // What the decode is scored against. verify_gold_reference.sh's check_against_source names
  // its checks *_source: FFmpeg cannot read those streams, so the reference is the source WAV
  // the third-party encoder was given. Every other check compares this decoder with FFmpeg's.
  const checkAgainst = (rec) => (/_source(_reference)?$/.test(String(rec.check)) ? "the source WAV" : "FFmpeg's decode");

  // The tightest channel of a record, by its own SNR. The index is bounds-checked: an
  // out-of-range one reads as missing here, as it must in tools/ci/write_measurement_badges.py.
  function describeCheck(rec, headroom) {
    let value = rec.worst_db;
    let floor = rec.threshold_db;
    let channel = null;
    if (Array.isArray(rec.thresholds_db) && Array.isArray(rec.channels_db) && rec.channels_db.length > 0) {
      const i = Number.isInteger(rec.tightest_channel) && rec.tightest_channel >= 0 &&
        rec.tightest_channel < rec.channels_db.length ? rec.tightest_channel : 0;
      value = rec.channels_db[i];
      if (i < rec.thresholds_db.length) floor = rec.thresholds_db[i];
      channel = Array.isArray(rec.channel_labels) && i < rec.channel_labels.length ? rec.channel_labels[i] : `ch${i}`;
    }
    return { check: rec.check, words: checkWords(rec), against: checkAgainst(rec), value, floor, channel, headroom, leg: rec.leg };
  }

  // The tightest gold-reference check of each codec, by margin over its own floor. Each check
  // is gated per CHANNEL, so the number that matters is the smallest per-channel margin, which
  // is NOT the lowest SNR: a 58 dB centre channel 6 dB above a 52 dB floor is closer to failing
  // than a 22 dB surround 6 dB above a 16 dB floor.
  function tightestPerCodec(rows) {
    const picked = {};
    for (const rec of rows) {
      if (!isNumber(rec.worst_db) || !isNumber(rec.threshold_db)) continue;
      const codec = qualityCodec(rec);
      const headroom = headroomOf(rec);
      if (!picked[codec] || headroom < picked[codec].headroom) picked[codec] = describeCheck(rec, headroom);
    }
    return picked;
  }

  // AC-4's decode quality: its lowest SNR and lowest MOS-LQO, one row per Dolby stream scored
  // against the source it was encoded from. Only CI's pinned floors exist, none recorded per
  // row, so nothing here can be coloured, and the figures are shown without a verdict.
  function ac4Quality(copies) {
    // One row per stream: a run repeated on one commit recorded each stream again.
    const once = new Set();
    const rows = copies.filter((r) => !once.has(r.leg) && once.add(r.leg));
    const snr = rows.filter((r) => isNumber(r.min_snr_db));
    const mos = rows.filter((r) => isNumber(r.mos_lqo));
    if (snr.length === 0 && mos.length === 0) return null;
    const lowest = (list, key) => list.reduce((a, b) => (b[key] < a[key] ? b : a));
    const highest = (list, key) => list.reduce((a, b) => (b[key] > a[key] ? b : a));
    const lowSnr = snr.length ? lowest(snr, "min_snr_db") : null;
    const lowMos = mos.length ? lowest(mos, "mos_lqo") : null;
    return {
      streams: rows.length,
      snr: lowSnr ? lowSnr.min_snr_db : null, snrLeg: lowSnr ? lowSnr.leg : null,
      mos: lowMos ? lowMos.mos_lqo : null, mosLeg: lowMos ? lowMos.leg : null,
      mosBest: mos.length ? highest(mos, "mos_lqo").mos_lqo : null,
    };
  }

  // The workload with the largest `field` for each codec (the first one on a tie).
  function extremePerCodec(rows, field) {
    const picked = {};
    for (const row of rows) {
      if (!isNumber(row[field])) continue;
      const [codec, words] = classify(row.config);
      if (!picked[codec] || row[field] > picked[codec].value) {
        picked[codec] = { config: row.config, words, value: row[field], frames: row.frames, leg: row.leg, allocs: row.allocs_per_frame };
      }
    }
    return picked;
  }

  // Allocator traffic: bytes requested from the heap over a frame, whatever became of them.
  // A workload that allocates and frees a 100 KB scratch buffer every frame scores 100 KB and
  // holds none of it, so this is not the memory a codec occupies. That is live growth.
  const heaviestPerCodec = (rows) => extremePerCodec(rows, "bytes_per_frame");
  const mostRetainingPerCodec = (rows) => extremePerCodec(rows, "steady_live_growth");

  // The tool comparison's encoder output, scored by ViSQOL against its source: the range of
  // MOS-LQO for each codec and the rows at both ends of it.
  function listeningPerCodec(rows) {
    const picked = {};
    const seen = new Set();
    for (const row of rows) {
      if (!isNumber(row.mos_lqo)) continue;
      const codec = listeningCodec(row);
      // A (leg, variant) is one measurement. A run repeated on one commit (the nightly, on a tip
      // that has not moved) recorded it again, to the last digits of the MOS, and the history on
      // the branch still holds those copies.
      const identity = [codec, row.leg, row.variant].join("\u0000");
      if (seen.has(identity)) continue;
      seen.add(identity);
      const p = picked[codec] || (picked[codec] = { count: 0, low: null, high: null, legs: new Set() });
      p.count += 1;
      p.legs.add(row.leg);
      if (!p.low || row.mos_lqo < p.low.mos_lqo) p.low = row;
      if (!p.high || row.mos_lqo > p.high.mos_lqo) p.high = row;
    }
    return picked;
  }

  // The layouts a leg name spells in digits: A/52's "51", and the AC-4 encoder's, whose first
  // digits count the front and surround channels and whose last the LFE ("20" stereo, "514" 5.1.4).
  const LEG_LAYOUTS = { "10": "mono", "20": "stereo", "50": "5.0", "51": "5.1", "514": "5.1.4" };

  // "5.1, 256 kbps" from a leg named "eac3-51-256"; "music stereo, 96 kbps" from
  // "eac3-music-stereo-96"; "stereo music, 48 kbps" from "ac4-20-music-48". A name in another
  // shape comes back as it is.
  function legWords(leg) {
    const parts = String(leg).split("-");
    if (parts[0] === "ac3" || parts[0] === "eac3" || parts[0] === "ac4") parts.shift();
    const rate = parts.length > 1 && /^\d+$/.test(parts[parts.length - 1]) ? `${parts.pop()} kbps` : null;
    return [parts.map((p) => (has(LEG_LAYOUTS, p) ? LEG_LAYOUTS[p] : p)).join(" "), rate].filter(Boolean).join(", ");
  }

  // Each series is a file of its own, written by its own job, so two cards, or two lines of one,
  // can be from different commits: speed, memory and the gold-reference gate follow each merge,
  // while the AC-4 decode scores, the encoders' listening scores and the tool comparison follow
  // the nightly scoring run. `series` is newestCommitRows() of one history file and `latest` is
  // the newest of all of them. A line from an older commit than that says which commit and day
  // it was recorded on, so an old figure does not read as the latest one. Nothing for a series
  // that is the newest, or one that has no commit to name.
  function recordedNote(series, latest) {
    if (!series || !latest || !series.commit || series.commit === latest.commit) return "";
    const day = Number.isNaN(Date.parse(series.date)) ? "" : new Date(series.date).toISOString().slice(0, 10);
    return ` <em>Last recorded on <code>${esc(String(series.commit).slice(0, 8))}</code>${day ? ", " + day : ""}.</em>`;
  }

  // The series whose newest commit is the most recent, by the date that commit was made.
  function newestSeries(all) {
    let best = null;
    for (const s of all) {
      if (!s || !s.commit || Number.isNaN(Date.parse(s.date))) continue;
      if (!best || Date.parse(s.date) > Date.parse(best.date)) best = s;
    }
    return best;
  }

  // How old the newest record is, in whole hours, or null with no date to count from. `latest`
  // is newestSeries()'s pick and `now` the reader's clock in milliseconds.
  function recordAgeHours(latest, now) {
    const at = latest ? Date.parse(latest.date) : NaN;
    return Number.isNaN(at) ? null : Math.max(0, Math.floor((now - at) / 3600000));
  }

  // An age in words: hours up to two days, days after.
  function agoWords(hours) {
    if (hours < 1) return "within the last hour";
    if (hours < 48) return hours === 1 ? "1 hour ago" : `${hours} hours ago`;
    return `${Math.floor(hours / 24)} days ago`;
  }

  // The line under the cards: the commit the newest figures are from, the day that commit was
  // made and how long ago that is. CI writes the series after merges and nightly, and when it
  // has written nothing for a while (main red, a failed nightly) the cards go on showing the
  // last figures recorded: this line is what says how old they are. Past STALE_AFTER_DAYS it
  // says that nothing newer has been recorded, and `stale` tells the caller to colour it. Speed,
  // memory and the gold-reference gate are recorded after merges and nightly, so the newest
  // record is normally a day old at the most. The date is the commit's own, which is all a
  // record carries; its figures were measured within hours of it.
  const STALE_AFTER_DAYS = 2;
  function measuredOn(latest, now) {
    const hours = recordAgeHours(latest, now);
    if (hours === null || !latest.commit) return { html: "", stale: false };
    const stale = hours > STALE_AFTER_DAYS * 24;
    return {
      stale,
      html: `Measured on <a href="https://github.com/${REPO}/commit/${esc(latest.commit)}">` +
        `<code>main@${esc(String(latest.commit).slice(0, 8))}</code></a>, committed ` +
        `${new Date(latest.date).toISOString().slice(0, 10)} (${agoWords(hours)}).` +
        (stale ? " Nothing newer has been recorded since." : ""),
    };
  }
  // <<< measurement summary

  function rawUrl(file) {
    return `https://raw.githubusercontent.com/${REPO}/${HISTORY_BRANCH}/${file}`;
  }

  function parseJsonl(text) {
    return text.split("\n").filter((l) => l.trim().length > 0).map((l) => JSON.parse(l));
  }

  // Every append-history producer writes a *.recent.jsonl sidecar once its
  // history outgrows the shared window. Before that, absence is normal and the
  // authoritative full file remains the fallback.
  async function fetchHistory(stem) {
    for (const file of [`${stem}.recent.jsonl`, `${stem}.jsonl`]) {
      try {
        const resp = await fetch(rawUrl(file));
        if (!resp.ok) continue;
        return parseJsonl(await resp.text());
      } catch (e) {
        // fall through to the next candidate
      }
    }
    return [];
  }

  // Only the newest commit's rows. These files are appended to in merge order,
  // so the last commit SHA to appear is the newest - taken from the end rather
  // than by sorting on commit_date, which would tie whenever two records share
  // a second.
  function newestCommitRows(records) {
    if (records.length === 0) return { rows: [], commit: null, date: null };
    const commit = records[records.length - 1].commit;
    const rows = records.filter((r) => r.commit === commit);
    return { rows, commit, date: rows[0] ? rows[0].commit_date : null };
  }

  // One card per measure, one line per codec: the codec, its figure, and the
  // workload the figure comes from. A codec with no rows says so on its own line.
  function line(codec, figure, unit, what, watch) {
    return `<div class="pq-line"><span class="pq-codec">${codec}</span>` +
      `<span class="pq-figure${watch ? " pq-figure-watch" : ""}">${figure}` +
      `<span class="pq-unit">${unit ? " " + unit : ""}</span></span>` +
      `<span class="pq-what">${what}</span></div>`;
  }

  // badge may be null, meaning NO badge - not an unknown one. A card with a
  // real number on it must never be labelled "no data" just because nothing
  // gates that number.
  function card(title, sub, lines, note, badge) {
    const chip = badge ? `<span class="pq-badge ${badge.cls}">${badge.label}</span>` : "";
    return `<div class="pq-card">
      <h3 class="pq-head">${title}${chip}</h3>
      ${sub ? `<div class="pq-sub">${sub}</div>` : ""}
      ${lines.join("")}
      <div class="pq-note">${note}</div>
    </div>`;
  }

  const OK = { cls: "pq-ok", label: "ok" };
  const WATCH = { cls: "pq-watch", label: "watch" };
  const NONE = { cls: "pq-unknown", label: "no data" };

  const NO_MEASUREMENT = "No measurement recorded.";
  const emptyCard = (title) => card(title, "", [], NO_MEASUREMENT, NONE);
  const noLine = (codec, what) => line(codec, "&mdash;", "", what || NO_MEASUREMENT);
  const code = (s) => `<code>${esc(s)}</code>`;

  // The CPU model a picked workload was measured on, for the line that shows it. The x86
  // leg's hosted runner is one of several models from run to run and twice as fast on one as
  // on another, so a multiple on this card moves with the machine as well as with the code.
  // Nothing for a record that names no model: one from before the model was recorded, or one
  // whose runner gave the capture script no name ("unknown").
  function machineOf(rows, e) {
    const row = rows.find((r) => r.config === e.config && r.leg === e.leg && r.ms_per_frame === e.ms);
    return row && row.cpu_model && row.cpu_model !== "unknown" ? `, ${esc(row.cpu_model)}` : "";
  }

  function speedCard(rows, noteFor = () => "") {
    const picked = slowestPerCodec(rows);
    if (Object.keys(picked).length === 0) return emptyCard("Speed");
    const timed = rows.filter((r) => isNumber(r.ms_per_frame) && r.ms_per_frame > 0);
    const lines = codecsOf(picked).map((codec) => {
      const e = picked[codec];
      return e ? line(codec, formatMultiple(e.times) + "&times;", "",
        `${esc(e.words)} (${code(e.config)}) on ${code(e.leg)}${machineOf(rows, e)}: ${e.ms.toFixed(3)} ms/frame ` +
        `against a ${round2(e.budget)} ms frame.${noteFor(codec)}`) : noLine(codec);
    });
    // 1x is the line that means something - below it the codec cannot keep up
    // with playback. Anything above is headroom, and how much headroom is
    // "enough" is a judgement no threshold here should be making.
    const ok = Object.values(picked).every((e) => e.times >= 1);
    return card("Speed", "Times real time: the slowest workload of each codec.", lines,
      `Slowest of ${new Set(timed.map((r) => r.config)).size} workloads on ` +
      `${new Set(timed.map((r) => r.leg)).size} CI legs, encodes and decodes together. ` +
      "Real time is 1&times;. An AC-4 frame is 2&thinsp;048 samples, 42.67 ms, against A/52's 1&thinsp;536 samples, 32 ms. " +
      "The x86 leg runs on a different CPU model from run to run, and its multiples move with the machine.",
      ok ? OK : WATCH);
  }

  function qualityCard(rows, ac4Rows, noteFor = () => "") {
    // Least headroom over the gate, again worst-case: one leg sitting on its
    // floor matters more than four sitting well above it.
    const gated = tightestPerCodec(rows);
    const ac4 = ac4Quality(ac4Rows);
    if (Object.keys(gated).length === 0 && !ac4) return emptyCard("Decode accuracy");
    const lines = CODECS.map((codec) => {
      const g = gated[codec];
      if (g) {
        // Name the channel and the workload. "2.0 dB SNR" on its own invites the reading
        // that the codec is 2 dB accurate; it is one channel of one check, and the check's
        // own floor is 1 dB - see "What the SNR number is measuring" on this page.
        const channel = g.channel ? `, ${esc(has(CHANNEL_NAMES, g.channel) ? CHANNEL_NAMES[g.channel] : g.channel)} channel` : "";
        return line(codec, oneDecimal(g.value), "dB",
          `${esc(g.words)}${channel}, on ${code(g.leg)} (check ${code(g.check)}): ` +
          `${g.headroom.toFixed(1)} dB above its own ${oneDecimal(g.floor)} dB floor, scored against ${g.against}. ` +
          "Every channel is gated separately." + noteFor(codec), g.headroom < 0);
      }
      if (codec === "AC-4" && ac4 && ac4.snr !== null) {
        const mos = ac4.mos !== null
          ? ` MOS-LQO ${twoDecimals(ac4.mos)}&ndash;${twoDecimals(ac4.mosBest)}, lowest ${code(ac4.mosLeg)}.` : "";
        return line(codec, oneDecimal(ac4.snr), "dB",
          `Lowest minimum SNR of ${ac4.streams} Dolby streams, ${code(ac4.snrLeg)}, scored against the ` +
          `sources they were encoded from. No per-channel floor is recorded yet.${mos}${noteFor(codec)}`);
      }
      return noLine(codec);
    });
    // The colour is the margin over each channel's own floor. AC-4 has none and takes no part.
    const badge = Object.keys(gated).length === 0 ? null
      : Object.values(gated).every((g) => g.headroom >= 0) ? OK : WATCH;
    return card("Decode accuracy", "Tightest check of each codec: decoded audio against a reference, channel by channel.",
      lines, "The colour is the smallest margin over a channel's own floor. Nothing here measures perceived audio quality.", badge);
  }

  function listeningCard(rows, noteFor = () => "") {
    const picked = listeningPerCodec(rows);
    if (Object.keys(picked).length === 0) return emptyCard("Predicted listening quality");
    const lines = CODECS.map((codec) => {
      const p = picked[codec];
      if (!p) return noLine(codec);
      const range = p.low.mos_lqo === p.high.mos_lqo ? twoDecimals(p.low.mos_lqo)
        : `${twoDecimals(p.low.mos_lqo)}&ndash;${twoDecimals(p.high.mos_lqo)}`;
      return line(codec, range, "MOS-LQO",
        `${p.count} encoder measurements on ${p.legs.size} legs. Lowest is ${code(p.low.leg)} ` +
        `(${esc(legWords(p.low.leg))}${p.low.variant ? `, tool set ${code(p.low.variant)}` : ""}).${noteFor(codec)}`);
    });
    // NO badge, deliberately - not an unknown one. A low-bitrate leg is
    // SUPPOSED to score lower, so any absolute floor here would flag the
    // encoder for doing its job. A MOS change is judged on the trend pages,
    // against that leg's own past, which is the only comparison that means
    // anything.
    return card("Predicted listening quality", "This build's encoder output, scored by ViSQOL against its source.", lines,
      "5 is indistinguishable from the original; a lower bitrate scores lower by design. AC-4's line is " +
      "its programme legs at default options: the synthetic test signals are gated in CI and not counted here.", null);
  }

  function allocationCard(rows, noteFor = () => "") {
    const picked = heaviestPerCodec(rows);
    if (Object.keys(picked).length === 0) return emptyCard("Allocation per frame");
    const lines = codecsOf(picked).map((codec) => {
      const e = picked[codec];
      return e ? line(codec, round0(e.value / 1024), "KB",
        `${esc(e.words)} (${code(e.config)})${isNumber(e.allocs) ? `: ${round0(e.allocs)} allocations a frame` : ""}.${noteFor(codec)}`) : noLine(codec);
    });
    // NO badge: nothing gates allocator traffic, so there is no line to borrow.
    return card("Allocation per frame", "Bytes requested from the heap per frame: the heaviest workload of each codec.", lines,
      "This counts allocator traffic, not memory in use: a buffer allocated and freed every frame counts every time " +
      "and is held by none. What stays in memory is live growth.", null);
  }

  function growthCard(rows, noteFor = () => "") {
    const picked = mostRetainingPerCodec(rows);
    if (Object.keys(picked).length === 0) return emptyCard("Live growth");
    const windows = [...new Set(Object.values(picked).filter((e) => isNumber(e.frames)).map((e) => e.frames - 1))];
    const frames = windows.length === 1 ? String(windows[0]) : "about 200";
    const lines = codecsOf(picked).map((codec) => {
      const e = picked[codec];
      const over = e && e.value >= MEMORY_RETENTION_WARN_BYTES;
      return e ? line(codec, oneDecimal(e.value / 1024), "KiB",
        `${esc(e.words)} (${code(e.config)})${over ? ", over the 4 KiB line" : ""}.${noteFor(codec)}`, over) : noLine(codec);
    });
    // steady_live_growth is the bytes still held live after the steady-state frames. NOT
    // "> 0 means a leak": append_memory_history.py warns at 4 KiB and hard-fails at 1 MiB, so
    // a couple of KiB of retained working set is expected and this card says so rather than
    // calling it growth. The colour is the same line, so it says which workload retains memory.
    const held = Object.values(picked).every((e) => e.value < MEMORY_RETENTION_WARN_BYTES);
    return card("Live growth", `Bytes still held after ${frames} steady-state frames: the worst workload of each codec.`, lines,
      "The warning line is 4 KiB, the one append_memory_history.py warns at; it fails the build at 1 MiB.",
      held ? OK : WATCH);
  }

  // A workload no table lists is a maintainer's problem, not a reader's: say so in the card
  // it would have gone in rather than leaving the page on "Loading".
  function guarded(title, build) {
    try {
      return build();
    } catch (e) {
      if (!(e instanceof UnknownWorkload)) throw e;
      return card(title, "", [], esc(e.message), NONE);
    }
  }

  async function render() {
    const target = document.getElementById("pq-status");
    const [perf, quality, ac4, external, ac4Encode, memory] = await Promise.all([
      fetchHistory("performance-main"),
      fetchHistory("main"),
      fetchHistory("ac4-quality-main"),
      fetchHistory("external-comparison-main"),
      fetchHistory("ac4-encode-quality-main"),
      fetchHistory("memory-main"),
    ]);

    if (perf.length === 0 && quality.length === 0 && ac4.length === 0 && external.length === 0 &&
        ac4Encode.length === 0 && memory.length === 0) {
      target.innerHTML = `<p class="pq-loading">Could not reach the measurement history on
        <code>${HISTORY_BRANCH}</code>. The detail pages linked below fetch it the same way,
        so they will be empty too &mdash; this is a network or availability problem, not a
        missing measurement.</p>`;
      return;
    }

    const p = newestCommitRows(perf);
    const q = newestCommitRows(quality);
    const a = newestCommitRows(ac4);
    const e = newestCommitRows(external);
    const n = newestCommitRows(ac4Encode);
    const m = newestCommitRows(memory);

    // The newest commit of any series. A line from an older one says so (recordedNote): the
    // series are written by different jobs on different cadences, and any one of them can be
    // absent or behind on a given commit.
    const latest = newestSeries([p, q, a, e, n, m]);
    const since = (series) => () => recordedNote(series, latest);

    target.innerHTML = [
      guarded("Speed", () => speedCard(p.rows, since(p))),
      // Two series: the gold-reference gate (AC-3, E-AC-3) and AC-4's decode scores, each its
      // own newest commit.
      guarded("Decode accuracy", () => qualityCard(q.rows, a.rows,
        (codec) => recordedNote(codec === "AC-4" ? a : q, latest))),
      // Two series, each its own newest commit: the AC-3 and E-AC-3 tool comparison, and the
      // AC-4 encoder's own. A row says its codec, so the card files each under its own line.
      guarded("Predicted listening quality", () => listeningCard(e.rows.concat(n.rows),
        (codec) => recordedNote(codec === "AC-4" ? n : e, latest))),
      guarded("Allocation per frame", () => allocationCard(m.rows, since(m))),
      guarded("Live growth", () => growthCard(m.rows, since(m))),
    ].join("");

    if (latest) {
      // The commit the newest figures are from and how old it is (measuredOn). It used to give
      // the commit and its date in the reader's locale, with nothing to say the date was a week
      // ago when it was.
      const measured = measuredOn(latest, Date.now());
      const asof = document.getElementById("pq-asof");
      asof.innerHTML = `${measured.html} A line recorded on an older commit says which.`;
      if (measured.stale) asof.className = "pq-asof pq-asof-stale";
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", render);
  } else {
    render();
  }
})();
</script>
