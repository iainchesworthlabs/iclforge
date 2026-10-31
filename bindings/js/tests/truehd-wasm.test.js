// The compiled TrueHD module (apps/demos/wasm's `iclforge_wasm_truehd`), held to the committed
// fixtures of testdata/truehd: every decoded sample to the SHA-256 of what FFmpeg's strict decode
// gives, and the scan and the seek to what the manifest states. It is the one test where
// truehd.ts, truehd_bindings.cpp and the library meet; truehd.test.js checks truehd.ts alone
// over a stand-in.
//
// It needs the module built, which a plain `npm test` has not done, so it runs when
// ICLFORGE_TRUEHD_WASM names the glue (`iclforge_truehd.js`, written by the wasm-emscripten preset
// to bin/wasm_truehd_demo/) and is skipped, with that said, when it does not. The module is built
// for Node as well as the web (apps/demos/wasm/CMakeLists.txt), which is what lets it run here.

import { test } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import {
  truehdProbe,
  truehdScanUnits,
  TruehdScannedStream,
  TruehdDecoder,
  TruehdError,
  TruehdErrorKind,
  TruehdPresentation,
} from "../dist/truehd.js";

const glue = process.env.ICLFORGE_TRUEHD_WASM;
const skip = glue ? false : "ICLFORGE_TRUEHD_WASM does not name the built iclforge_truehd.js";

const fixtureDir = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "..", "testdata", "truehd");
const manifest = JSON.parse(readFileSync(join(fixtureDir, "manifest.json"), "utf8"));

let module = null;

async function load() {
  if (module === null) {
    const factory = createRequire(import.meta.url)(resolve(glue));
    module = await factory();
  }
  return module;
}

const streamOf = (fixture) => new Uint8Array(readFileSync(join(fixtureDir, fixture.file)));

/** What FFmpeg's strict decode writes: 32-bit little-endian samples, the 24-bit value shifted left by 8. */
function ffmpegSha256(pcm) {
  const bytes = Buffer.alloc(pcm.length * 4);
  pcm.forEach((sample, i) => bytes.writeInt32LE(sample << 8, i * 4));
  return createHash("sha256").update(bytes).digest("hex");
}

/** Every access unit of `stream` through a decoder: the samples joined, and the channel count. */
function decodeAll(m, stream, options) {
  const decoder = new TruehdDecoder(m, options);
  const pcm = [];
  let channels = 0;
  for (const unit of truehdScanUnits(m, stream).units) {
    const decoded = decoder.decode(unit);
    if (decoded) {
      channels = decoded.channels;
      pcm.push(...decoded.pcm);
    }
  }
  decoder.close();
  return { pcm, channels };
}

test("there are fixtures to hold the binding to", { skip }, () => {
  assert.ok(manifest.fixtures.length >= 20);
  assert.ok(manifest.fixtures.some((fixture) => fixture.codec === "mlp"));
});

test("decode gives FFmpeg's samples for every fixture", { skip }, async () => {
  const m = await load();
  for (const fixture of manifest.fixtures) {
    const { pcm, channels } = decodeAll(m, streamOf(fixture));
    assert.equal(channels, fixture.channels, fixture.name);
    assert.equal(pcm.length, fixture.pcm_samples * fixture.channels, fixture.name);
    assert.equal(ffmpegSha256(pcm), fixture.pcm_sha256, fixture.name);
    assert.ok(
      pcm.every((sample) => sample >= -(1 << 23) && sample < 1 << 23),
      `${fixture.name}: 24 bits, sign-extended`,
    );
  }
});

test("probe and scan state what the manifest states", { skip }, async () => {
  const m = await load();
  for (const fixture of manifest.fixtures) {
    const stream = streamOf(fixture);
    const info = truehdProbe(m, stream);
    assert.equal(info.isMlp, fixture.codec === "mlp", fixture.name);
    assert.equal(info.sampleRate, fixture.sample_rate, fixture.name);
    assert.equal(info.accessUnits, fixture.access_units, fixture.name);
    assert.equal(info.channels[info.substreams - 1], fixture.channels, fixture.name);
    assert.equal(info.firstUnitOffset, 0, fixture.name);

    const { units, skippedBytes } = truehdScanUnits(m, stream);
    assert.equal(units.length, fixture.access_units, fixture.name);
    assert.equal(skippedBytes, 0, fixture.name);
    assert.equal(
      units.reduce((total, unit) => total + unit.length, 0),
      fixture.stream_bytes,
      fixture.name,
    );
    assert.deepEqual(Buffer.concat(units), Buffer.from(stream), fixture.name);

    const scanned = new TruehdScannedStream(m, stream, 0);
    assert.equal(scanned.info.sampleRate, fixture.sample_rate, fixture.name);
    assert.equal(scanned.info.units, fixture.access_units, fixture.name);
    assert.equal(scanned.info.samples, fixture.pcm_samples, fixture.name);
    assert.equal(scanned.info.skippedBytes, 0, fixture.name);
    assert.equal(scanned.info.leadingBytes, 0, fixture.name);
    assert.equal(
      scanned.info.samples + scanned.info.paddingSamples,
      scanned.info.units * scanned.info.samplesPerUnit,
      fixture.name,
    );
    assert.ok(Math.abs(scanned.info.durationSeconds - fixture.pcm_samples / fixture.sample_rate) < 1e-9, fixture.name);
    assert.deepEqual(scanned.restarts()[0], { unit: 0, offset: 0 }, fixture.name);
    scanned.close();
  }
});

test("a decode from the restart for a sample gives that sample", { skip }, async () => {
  const m = await load();
  for (const fixture of manifest.fixtures) {
    const stream = streamOf(fixture);
    const scanned = new TruehdScannedStream(m, stream, 1);
    const whole = decodeAll(m, stream);
    const last = scanned.info.samples;
    for (const sample of [0, 1, Math.floor(last / 3), Math.floor(last / 2) + 7, last - 1]) {
      const restart = scanned.restartForSample(sample);
      assert.ok(restart, fixture.name);
      const first = restart.unit * scanned.info.samplesPerUnit;
      assert.ok(first <= sample, fixture.name);
      const joined = decodeAll(m, scanned.streamFrom(restart));
      const at = (sample - first) * whole.channels;
      const want = sample * whole.channels;
      assert.deepEqual(
        joined.pcm.slice(at, at + whole.channels),
        whole.pcm.slice(want, want + whole.channels),
        `${fixture.name} sample ${sample}`,
      );
    }
    scanned.close();
  }
});

test("the scan places time and samples", { skip }, async () => {
  const m = await load();
  const fixture = manifest.fixtures.find((candidate) => candidate.name === "truehd_st48_s24");
  const scanned = new TruehdScannedStream(m, streamOf(fixture), 64);
  assert.deepEqual(
    scanned.restarts().map((restart) => restart.unit),
    [0, 64, 128, 192, 256],
  );
  assert.equal(scanned.info.samplesPerUnit, 40);
  assert.equal(scanned.restartForSample(scanned.info.samples), null);
  assert.equal(scanned.restartForSample(64 * 40).unit, 64);
  assert.equal(scanned.sampleAtSeconds(0.125), 6000);
  assert.equal(scanned.sampleAtSeconds(-2), 0);
  scanned.close();
  // A spacing of 0 keeps every unit with a major sync, as 1 does.
  const every = new TruehdScannedStream(m, streamOf(fixture), 0);
  assert.equal(every.restarts().length, 19);
  every.close();
});

test("a decoded unit owns its samples and names its channels", { skip }, async () => {
  const m = await load();
  const fixture = manifest.fixtures.find((candidate) => candidate.name === "truehd_m51_s24");
  const decoder = new TruehdDecoder(m);
  const [first, second] = truehdScanUnits(m, streamOf(fixture)).units;
  const one = decoder.decode(first);
  const copy = Int32Array.from(one.pcm);
  decoder.decode(second);
  assert.deepEqual(one.pcm, copy, "the next decode leaves an earlier unit alone");
  assert.equal(one.channels, 6);
  assert.equal(one.samples, 40);
  assert.equal(one.sampleRate, 48000);
  assert.equal(one.restarted, true);
  assert.equal(one.pcm.length, 6 * 40);
  assert.equal(one.layout.length, 6);
  assert.ok(one.layout.includes("LFE"), one.layout.join(","));
  decoder.close();
});

test("the presentation chooses the substreams decoded", { skip }, async () => {
  const m = await load();
  const fixture = manifest.fixtures.find((candidate) => candidate.name === "truehd_m51_s24");
  const stream = streamOf(fixture);
  assert.equal(decodeAll(m, stream, { presentation: TruehdPresentation.TwoChannel }).channels, 2);
  assert.equal(decodeAll(m, stream, { presentation: TruehdPresentation.Best }).channels, 6);
});

test("a decoder given a unit before a restart header gives nothing, and says so in its counters", { skip }, async () => {
  const m = await load();
  const fixture = manifest.fixtures.find((candidate) => candidate.name === "truehd_st48_s24");
  const units = truehdScanUnits(m, streamOf(fixture)).units;
  const decoder = new TruehdDecoder(m);
  // A unit with no major sync, with nothing decoded before it.
  const midStream = units.find((unit) => !(unit[4] === 0xf8 && unit[5] === 0x72 && unit[6] === 0x6f));
  assert.equal(decoder.decode(midStream), null);
  assert.equal(decoder.counters().unitsWaiting, 1);
  assert.equal(decoder.counters().unitsDecoded, 0);
  decoder.close();
});

test("data that is not a stream is a TruehdError of kind BadSync, in all three entry points", { skip }, async () => {
  const m = await load();
  const junk = new Uint8Array(512).fill(0x5a);
  for (const call of [() => truehdProbe(m, junk), () => truehdScanUnits(m, junk), () => new TruehdScannedStream(m, junk)]) {
    assert.throws(call, (error) => error instanceof TruehdError && error.kind === TruehdErrorKind.BadSync);
  }
});

test("a damaged unit fails a strict decode and is counted by a lenient one", { skip }, async () => {
  const m = await load();
  const fixture = manifest.fixtures.find((candidate) => candidate.name === "truehd_st48_s24");
  const units = truehdScanUnits(m, streamOf(fixture)).units.map((unit) => Uint8Array.from(unit));
  // The middle of a unit well past the first restart: its substream segment, not a header.
  const damaged = 5;
  units[damaged][Math.floor(units[damaged].length / 2)] ^= 0x55;

  const strict = new TruehdDecoder(m);
  let failure = null;
  units.forEach((unit, index) => {
    try {
      strict.decode(unit);
    } catch (error) {
      assert.equal(index, damaged, "only the damaged unit fails");
      failure = error;
    }
  });
  assert.ok(failure instanceof TruehdError);
  assert.notEqual(failure.message, "");

  const lenient = new TruehdDecoder(m, { lenient: true });
  for (const unit of units) {
    lenient.decode(unit);
  }
  assert.ok(lenient.counters().checkFailures > 0, JSON.stringify(lenient.counters()));
  strict.close();
  lenient.close();
});

test("after lost data a decoder waits for a restart header; reset forgets the stream", { skip }, async () => {
  const m = await load();
  const fixture = manifest.fixtures.find((candidate) => candidate.name === "truehd_st48_s24");
  const units = truehdScanUnits(m, streamOf(fixture)).units;
  const decoder = new TruehdDecoder(m);
  assert.ok(decoder.decode(units[0]));
  assert.ok(decoder.decode(units[1]));
  decoder.lostData();
  assert.equal(decoder.decode(units[2]), null, "no restart header yet");
  decoder.reset();
  assert.equal(decoder.counters().units, 0, "reset clears the counters");
  assert.ok(decoder.decode(units[0]), "a unit with a major sync starts it again");
  decoder.close();
});
