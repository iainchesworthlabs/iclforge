// truehd.ts, run in Node over a stand-in for the Embind module (the compiled one is held to
// FFmpeg's samples by truehd-wasm.test.js). What is checked here is what truehd.ts itself does: that
// the options reach the native constructors as the numbers the C++ header defines, that a native
// `{ error, message, detail }` becomes a TruehdError of the right kind, that a null stays null, that
// units are views into the caller's bytes, and that close() releases once. The glue loading is
// ac4.test.js's harness, with a data: URL in place of blob:, which Node cannot import.

import { test } from "node:test";
import assert from "node:assert/strict";
import {
  loadTruehdModule,
  truehdProbe,
  truehdScanUnits,
  TruehdScannedStream,
  TruehdDecoder,
  TruehdError,
  TruehdErrorKind,
  TruehdPresentation,
  TRUEHD_DEFAULT_RESTART_SPACING,
} from "../dist/truehd.js";

const fetched = [];
let factoryArgs = null;
let fakeModule = null;

globalThis.__truehdFakeFactory = async (overrides) => {
  factoryArgs = overrides;
  return fakeModule;
};

globalThis.fetch = async (url) => {
  fetched.push(String(url));
  return { text: async () => "var createIclForgeTruehdModule = globalThis.__truehdFakeFactory;" };
};

const RealBlob = globalThis.Blob;
const revoked = [];
globalThis.Blob = class extends RealBlob {
  constructor(parts, options) {
    super(parts, options);
    this.source = parts.join("");
  }
};
URL.createObjectURL = (blob) => `data:text/javascript;base64,${Buffer.from(blob.source).toString("base64")}`;
URL.revokeObjectURL = (url) => revoked.push(url);

const GLUE_URL = "https://example.test/wasm/iclforge_truehd.js";

/** The native module truehd_bindings.cpp builds, scripted: what each call returns is set by the test. */
function makeFake(script = {}) {
  const log = { probed: [], scanned: [], scans: [], decoders: [], decoded: [], deleted: 0, calls: [] };
  const module = {
    probe(data) {
      log.probed.push(data);
      return script.probe ?? { isMlp: false, sampleRate: 48000 };
    },
    scanUnits(data) {
      log.scanned.push(data);
      return script.scanUnits ?? { units: [], skippedBytes: 0 };
    },
    TruehdScan: class {
      constructor(data, spacing) {
        log.scans.push({ data, spacing });
        this.failure = script.scanFailure ?? null;
      }
      errorKind() {
        return this.failure ? this.failure.error : -1;
      }
      message() {
        return this.failure.message;
      }
      detail() {
        return this.failure.detail;
      }
      info() {
        return { sampleRate: 48000, samples: 12000 };
      }
      restarts() {
        return [{ unit: 0, offset: 0 }];
      }
      restartForSample(sample) {
        log.calls.push(["restartForSample", sample]);
        return sample < 12000 ? { unit: 4, offset: 7 } : null;
      }
      sampleAtSeconds(seconds) {
        log.calls.push(["sampleAtSeconds", seconds]);
        return seconds * 48000;
      }
      delete() {
        log.deleted += 1;
      }
    },
    TruehdDecoder: class {
      constructor(presentation, lenient) {
        log.decoders.push({ presentation, lenient });
        this.queue = [...(script.decode ?? [])];
      }
      decode(unit) {
        log.decoded.push(unit);
        return this.queue.length > 0 ? this.queue.shift() : null;
      }
      lostData() {
        log.calls.push(["lostData"]);
      }
      reset() {
        log.calls.push(["reset"]);
      }
      counters() {
        return { units: log.decoded.length };
      }
      delete() {
        log.deleted += 1;
      }
    },
  };
  return { module, log };
}

test("loadTruehdModule fetches the glue, resolves the wasm beside it, and returns the fake module", async () => {
  fakeModule = makeFake().module;
  fetched.length = 0;
  revoked.length = 0;
  const module = await loadTruehdModule(GLUE_URL);
  assert.deepEqual(fetched, [GLUE_URL]);
  assert.equal(factoryArgs.locateFile("iclforge_truehd.wasm"), "https://example.test/wasm/iclforge_truehd.wasm");
  assert.equal(revoked.length > 0, true, "the object URL is revoked once imported");
  assert.equal(module, fakeModule);
});

test("the ErrorKind and Presentation enums keep the C++ headers' numbers", () => {
  // error.hpp and decoder.hpp declare these in this order and the native side sends the numbers.
  assert.deepEqual(
    [TruehdErrorKind.Truncated, TruehdErrorKind.BadSync, TruehdErrorKind.BadLosslessCheck, TruehdErrorKind.Unsupported],
    [0, 1, 12, 14],
  );
  assert.deepEqual(
    [TruehdPresentation.Best, TruehdPresentation.TwoChannel, TruehdPresentation.EightChannel, TruehdPresentation.All],
    [0, 1, 3, 4],
  );
});

test("truehdProbe returns what the native probe found, and throws a TruehdError for a failure", () => {
  const { module, log } = makeFake({ probe: { isMlp: true, sampleRate: 96000 } });
  const data = new Uint8Array([1, 2, 3]);
  assert.deepEqual(truehdProbe(module, data), { isMlp: true, sampleRate: 96000 });
  assert.equal(log.probed[0], data);

  const failing = makeFake({ probe: { error: TruehdErrorKind.BadSync, message: "no sync", detail: "no unit" } });
  assert.throws(
    () => truehdProbe(failing.module, data),
    (error) => {
      assert.ok(error instanceof TruehdError);
      assert.ok(error instanceof Error);
      assert.equal(error.name, "TruehdError");
      assert.equal(error.kind, TruehdErrorKind.BadSync);
      assert.equal(error.detail, "no unit");
      assert.equal(error.message, "no sync: no unit");
      return true;
    },
  );
});

test("a failure of kind 0 is still a failure, and a failure without detail says only its message", () => {
  const { module } = makeFake({ probe: { error: 0, message: "truncated", detail: "" } });
  assert.throws(
    () => truehdProbe(module, new Uint8Array(1)),
    (error) => error.kind === TruehdErrorKind.Truncated && error.message === "truncated",
  );
});

test("truehdScanUnits returns views into the caller's bytes, not copies", () => {
  const { module } = makeFake({
    scanUnits: {
      units: [
        { offset: 2, size: 3 },
        { offset: 5, size: 1 },
      ],
      skippedBytes: 2,
    },
  });
  const data = new Uint8Array([9, 9, 1, 2, 3, 4]);
  const scanned = truehdScanUnits(module, data);
  assert.equal(scanned.skippedBytes, 2);
  assert.deepEqual(
    scanned.units.map((unit) => [...unit]),
    [[1, 2, 3], [4]],
  );
  for (const unit of scanned.units) {
    assert.equal(unit.buffer, data.buffer);
  }
  data[2] = 77;
  assert.equal(scanned.units[0][0], 77, "a view sees a later write");
});

test("truehdScanUnits throws for data with no unit that reads", () => {
  const { module } = makeFake({ scanUnits: { error: TruehdErrorKind.BadSync, message: "no sync", detail: "" } });
  assert.throws(() => truehdScanUnits(module, new Uint8Array(4)), { name: "TruehdError", kind: TruehdErrorKind.BadSync });
});

test("TruehdScannedStream passes the spacing on, defaults it, and reads its info once", () => {
  const { module, log } = makeFake();
  const data = new Uint8Array(32);
  const stream = new TruehdScannedStream(module, data, 1);
  assert.deepEqual(log.scans[0], { data, spacing: 1 });
  assert.deepEqual(stream.info, { sampleRate: 48000, samples: 12000 });
  assert.deepEqual(stream.restarts(), [{ unit: 0, offset: 0 }]);
  new TruehdScannedStream(module, data);
  assert.equal(log.scans[1].spacing, TRUEHD_DEFAULT_RESTART_SPACING);
  assert.equal(TRUEHD_DEFAULT_RESTART_SPACING, 256);
});

test("TruehdScannedStream places time and samples through the native scan, and streamFrom is a view", () => {
  const { module, log } = makeFake();
  const data = Uint8Array.from({ length: 16 }, (_, i) => i);
  const stream = new TruehdScannedStream(module, data);
  assert.equal(stream.sampleAtSeconds(0.5), 24000);
  assert.deepEqual(log.calls.at(-1), ["sampleAtSeconds", 0.5]);
  assert.deepEqual(stream.restartForSample(100), { unit: 4, offset: 7 });
  assert.equal(stream.restartForSample(12000), null, "a sample past the end has no restart");
  const from = stream.streamFrom({ unit: 4, offset: 7 });
  assert.equal(from.length, 9);
  assert.equal(from[0], 7);
  assert.equal(from.buffer, data.buffer);
});

test("TruehdScannedStream throws the native failure and releases the native object", () => {
  const { module, log } = makeFake({
    scanFailure: { error: TruehdErrorKind.BadSync, message: "no sync", detail: "no access unit with a major sync" },
  });
  assert.throws(
    () => new TruehdScannedStream(module, new Uint8Array(4)),
    (error) => error instanceof TruehdError && error.kind === TruehdErrorKind.BadSync && error.detail.startsWith("no access"),
  );
  assert.equal(log.deleted, 1, "a scan that failed is not left allocated");
});

test("TruehdScannedStream.close releases the native object once", () => {
  const { module, log } = makeFake();
  const stream = new TruehdScannedStream(module, new Uint8Array(4));
  stream.close();
  stream.close();
  assert.equal(log.deleted, 1);
});

test("TruehdDecoder defaults to the best presentation and a strict decode", () => {
  const { module, log } = makeFake();
  new TruehdDecoder(module);
  assert.deepEqual(log.decoders[0], { presentation: TruehdPresentation.Best, lenient: false });
});

test("TruehdDecoder passes its options through", () => {
  const { module, log } = makeFake();
  new TruehdDecoder(module, { presentation: TruehdPresentation.SixChannel, lenient: true });
  assert.deepEqual(log.decoders[0], { presentation: 2, lenient: true });
});

test("decode returns the unit, then null for a unit with no samples, and throws for a failure", () => {
  const unit = { sampleRate: 48000, channels: 2, samples: 40, pcm: new Int32Array(80) };
  const { module, log } = makeFake({
    decode: [
      unit,
      null,
      { error: TruehdErrorKind.BadLosslessCheck, message: "lossless check", detail: "restart interval" },
    ],
  });
  const decoder = new TruehdDecoder(module);
  const bytes = new Uint8Array(8);
  assert.equal(decoder.decode(bytes), unit);
  assert.equal(log.decoded[0], bytes, "the unit goes to the native decoder as given");
  assert.equal(decoder.decode(bytes), null);
  assert.throws(
    () => decoder.decode(bytes),
    (error) => error instanceof TruehdError && error.kind === TruehdErrorKind.BadLosslessCheck,
  );
});

test("lostData, reset and counters go to the native decoder", () => {
  const { module, log } = makeFake();
  const decoder = new TruehdDecoder(module);
  decoder.decode(new Uint8Array(1));
  decoder.lostData();
  decoder.reset();
  assert.deepEqual(log.calls, [["lostData"], ["reset"]]);
  assert.deepEqual(decoder.counters(), { units: 1 });
});

test("TruehdDecoder.close releases the native object once", () => {
  const { module, log } = makeFake();
  const decoder = new TruehdDecoder(module);
  decoder.close();
  decoder.close();
  assert.equal(log.deleted, 1);
});
