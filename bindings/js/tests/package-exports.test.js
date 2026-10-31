// package.json's `exports` map: every entry names files `npm run build` writes, and `./ac4` and
// `./truehd` (the AC-4 and TrueHD wrappers, bindings/js/src/ac4.ts and truehd.ts) are reachable
// through the package's own name the way a consumer reaches them, with declarations that name what
// they export.

import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const pkg = JSON.parse(readFileSync(join(root, "package.json"), "utf8"));

test("every entry of exports names a file the build wrote, with types before default", () => {
  for (const [entry, conditions] of Object.entries(pkg.exports)) {
    assert.deepEqual(Object.keys(conditions), ["types", "default"], `${entry}: types must come first`);
    for (const [condition, path] of Object.entries(conditions)) {
      assert.ok(existsSync(join(root, path)), `${entry} ${condition}: ${path} is missing`);
    }
  }
});

test("./ac4 is exported, loads through the package's own name, and its declarations name its exports", async () => {
  assert.deepEqual(pkg.exports["./ac4"], { types: "./dist/ac4.d.ts", default: "./dist/ac4.js" });
  // A package may import itself by name through its exports map.
  const ac4 = await import("iclforge-wasm-decoder/ac4");
  const values = [
    "Ac4Decoder",
    "Ac4Encoder",
    "loadAc4Module",
    "syncFrame",
    "Ac4BedChannel",
    "Ac4ObjectCoding",
    "Ac4AjocDownmix",
    "Ac4AdditionalPair",
    "Ac4CodecMode",
    "Ac4RateMode",
  ];
  for (const name of values) {
    assert.ok(name in ac4, `${name} is not exported`);
  }
  const declarations = readFileSync(join(root, "dist", "ac4.d.ts"), "utf8");
  const declared = [
    ...values,
    "Ac4EncoderOptions",
    "Ac4ObjectsConfig",
    "Ac4ObjectConfig",
    "Ac4ObjectProperties",
    "Ac4ObjectMetadataUpdate",
    "Ac4Experimental",
    "RawAc4Object",
    "RawAc4ObjectUpdate",
    "RawAc4ObjectProperties",
  ];
  for (const name of declared) {
    assert.match(declarations, new RegExp(`export (declare )?(class|interface|type|enum|function) ${name}\\b`), name);
  }
});

test("./truehd is exported, loads through the package's own name, and its declarations name its exports", async () => {
  assert.deepEqual(pkg.exports["./truehd"], { types: "./dist/truehd.d.ts", default: "./dist/truehd.js" });
  const truehd = await import("iclforge-wasm-decoder/truehd");
  const values = [
    "TruehdDecoder",
    "TruehdScannedStream",
    "TruehdError",
    "TruehdErrorKind",
    "TruehdPresentation",
    "loadTruehdModule",
    "truehdProbe",
    "truehdScanUnits",
    "TRUEHD_DEFAULT_RESTART_SPACING",
  ];
  for (const name of values) {
    assert.ok(name in truehd, `${name} is not exported`);
  }
  const declarations = readFileSync(join(root, "dist", "truehd.d.ts"), "utf8");
  const declared = [
    "TruehdDecoder",
    "TruehdScannedStream",
    "TruehdError",
    "TruehdErrorKind",
    "TruehdPresentation",
    "loadTruehdModule",
    "truehdProbe",
    "truehdScanUnits",
    "TruehdDecoderOptions",
    "TruehdStreamInfo",
    "TruehdUnits",
    "TruehdRestart",
    "TruehdDecoded",
    "TruehdCounters",
    "TruehdScanInfo",
    "TruehdEmbindModule",
  ];
  for (const name of declared) {
    assert.match(declarations, new RegExp(`export (declare )?(class|interface|type|enum|function) ${name}\\b`), name);
  }
});
