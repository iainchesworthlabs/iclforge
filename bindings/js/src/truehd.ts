// A typed wrapper over the TrueHD and MLP Embind module apps/demos/wasm/truehd_bindings.cpp builds
// (apps/demos/wasm/CMakeLists.txt's `iclforge_wasm_truehd` target,
// `-sEXPORT_NAME=createIclForgeTruehdModule`): the decode side of iclforge::truehd, the same
// surface bindings/python's iclforge.truehd and bindings/rust's iclforge::truehd have. The
// encoder, the Atmos layer and the metadata edit are not in the module.
//
// A plain ES module like ac4.ts, for the reasons ac4.ts gives: no Worker protocol (a decode here is
// a call on bytes, and the bytes are a whole stream or a whole access unit), and the
// loadEmscriptenGlue() technique copied exactly, since the glue is a classic script and its
// `.wasm` is found relative to `glueUrl` only because of the `locateFile` override.
//
// Three things differ from the AC-3 and AC-4 wrappers:
//   - A failure is thrown, as a TruehdError that carries the library's ErrorKind. The native side
//     returns `{ error, message, detail }` where the result would be (an Embind throw reaches the
//     page as a bare pointer number), and this file is the one place that turns it into an
//     exception. A unit that gives no samples because no restart header has come yet is not a
//     failure: decode() returns null for it.
//   - What leaves the module is owned. Each decoded unit's `pcm` and `extraData` are copies, so a
//     result stays valid across the next decode().
//   - A stream is a whole file in memory: TruehdScannedStream says how long it is and where a decode
//     can join it, and streamFrom() gives the bytes from there. A decoder can only start at an
//     access unit that carries a major sync; the restart interval is the seek granule.
//
// Counts, sample numbers and byte offsets are JS numbers, exact to 2^53.

/** iclforge::truehd::ErrorKind's own numeric order (error.hpp) - kept in sync by hand. */
export enum TruehdErrorKind {
  /** The data ends inside a field or a unit. */
  Truncated = 0,
  /** No major sync, or no access unit, where one was required. */
  BadSync = 1,
  /** TH spec §3.2: the nibbles of the header and directory do not XOR to 0xF. */
  BadCheckNibble = 2,
  /** A length, an end pointer or a size outside the data or outside its limit. */
  BadLength = 3,
  /** TH spec §4.3: the major sync's signature is not 0xB752. */
  BadSignature = 4,
  /** TH spec §4.1: the CRC-16 of the major sync. */
  BadMajorSyncCrc = 5,
  /** TH spec §7.2. */
  BadRestartSync = 6,
  /** TH spec §7.1: the CRC-8 of the restart header. */
  BadRestartCrc = 7,
  /** TH spec §6.2: the parity byte at the end of a substream segment. */
  BadParity = 8,
  /** TH spec §6.2: the CRC-8 at the end of a substream segment. */
  BadSubstreamCrc = 9,
  /** A field outside the range the specification allows. */
  BadParameter = 10,
  /** A block whose sample data does not end where it says (TH spec §8.1), or a bad block. */
  BadBlockData = 11,
  /** TH spec §7.3: the decoded samples do not match the restart header's check. */
  BadLosslessCheck = 12,
  /** A substream needs a restart header (or a major sync) it has not seen. */
  NeedsRestart = 13,
  /** Valid, and a form this version does not decode (the detail names it). */
  Unsupported = 14,
}

/** iclforge::truehd::Presentation's own numeric order (decoder.hpp) - kept in sync by hand. */
export enum TruehdPresentation {
  /** The highest of substreams 0 to 2 the stream has: the stereo, 5.1 or 7.1 bed. */
  Best = 0,
  /** Substream 0. */
  TwoChannel = 1,
  /** Substreams 0 to 1. */
  SixChannel = 2,
  /** Substreams 0 to 2. */
  EightChannel = 3,
  /** Every substream, the sixteen-channel one last. */
  All = 4,
}

/** What failed, as the library says it: the kind, its text, and the detail where it has one. */
export class TruehdError extends Error {
  readonly kind: TruehdErrorKind;
  readonly detail: string;

  constructor(kind: TruehdErrorKind, message: string, detail: string) {
    super(detail === "" ? message : `${message}: ${detail}`);
    this.name = "TruehdError";
    this.kind = kind;
    this.detail = detail;
  }
}

export interface TruehdDecoderOptions {
  /** Which substreams to decode. Default {@link TruehdPresentation.Best}. */
  presentation?: TruehdPresentation;
  /**
   * `false` (the default): a check that fails (check nibble, CRC, parity, the lossless check of a
   * restart interval) is an error and the unit is dropped. `true`: the failure is counted
   * ({@link TruehdDecoder.counters}) and the unit is decoded as far as it reads.
   */
  lenient?: boolean;
}

/** What the first major sync and the restart headers after it say of a stream. */
export interface TruehdStreamInfo {
  /** `false` for TrueHD, `true` for MLP (DVD-Audio). */
  isMlp: boolean;
  sampleRate: number;
  substreams: number;
  /** The channels a decode of substream `i` delivers (the substream and the ones it builds on), 0 for one the probe did not reach. */
  channels: number[];
  samplesPerAccessUnit: number;
  /** Counted in the data given. */
  accessUnits: number;
  variableRate: boolean;
  /** The 15-bit field of the major sync, as written. */
  peakDataRateField: number;
  /** A fourth substream, or the major sync's Atmos bit. */
  hasObjectSubstream: boolean;
  /** Where the first access unit's header starts in the data given. */
  firstUnitOffset: number;
}

/** The access units of a bare stream, as views into the bytes that were scanned. */
export interface TruehdUnits {
  units: Uint8Array[];
  /** Bytes of the data that were part of no unit, those ahead of the first one included. */
  skippedBytes: number;
}

/** A unit a decoder can start at: its number counted from the first unit of the stream, and where it begins in the data. */
export interface TruehdRestart {
  unit: number;
  offset: number;
}

/** One decoded access unit. */
export interface TruehdDecoded {
  sampleRate: number;
  channels: number;
  /** Samples a channel. */
  samples: number;
  /** The substream the PCM is from. */
  substream: number;
  /** Whether a restart header was read in the substream decoded. */
  restarted: boolean;
  /**
   * The last unit of a stream that does not end on a unit boundary carries a marker counting the
   * samples at its end that are padding. They are not in `samples` or `pcm`.
   */
  paddingSamples: number;
  /** Interleaved, `samples * channels` values, each 24 bits wide and sign-extended (a 16-bit stream's values are multiples of 256). */
  pcm: Int32Array;
  /** The unit's bytes after its last substream segment, uninterpreted (the object audio metadata is in them). */
  extraData: Uint8Array;
  /** The speaker of each channel by the major sync's channel assignment ("L", "R", "C", "LFE", "Ls", ...); empty where it does not name them. */
  layout: string[];
}

/** What a decoder has seen so far. */
export interface TruehdCounters {
  /** Units decode() was given. */
  units: number;
  /** ... that gave samples. */
  unitsDecoded: number;
  /** ... that gave none because no restart header had come yet. */
  unitsWaiting: number;
  restarts: number;
  losslessChecksOk: number;
  losslessCheckFailures: number;
  /** Check nibble, major sync CRC, restart CRC, parity and segment CRC together. */
  checkFailures: number;
  matrixHeaders: number;
  lsbBypassMatrices: number;
}

/** What a whole-stream scan says about the stream. */
export interface TruehdScanInfo {
  isMlp: boolean;
  sampleRate: number;
  /** The samples in each access unit, from the sample rate. */
  samplesPerUnit: number;
  substreams: number;
  channels: number[];
  variableRate: boolean;
  peakDataRateField: number;
  hasObjectSubstream: boolean;
  /** Units that read, counting from the first one. */
  units: number;
  unitBytes: number;
  skippedBytes: number;
  /** The part of `skippedBytes` that came before the first unit, which is not a loss of the stream. */
  leadingBytes: number;
  /** Units that carry a major sync, all of them and not only those the scan keeps as restarts. */
  majorSyncs: number;
  paddingSamples: number;
  /** Every sample the stream holds, padding left out. */
  samples: number;
  durationSeconds: number;
}

interface RawFailure {
  error: number;
  message: string;
  detail: string;
}

/** The Embind class truehd_bindings.cpp's `TruehdScan` builds. */
export interface NativeTruehdScan {
  errorKind(): number;
  message(): string;
  detail(): string;
  info(): TruehdScanInfo;
  restarts(): TruehdRestart[];
  restartForSample(sample: number): TruehdRestart | null;
  sampleAtSeconds(seconds: number): number;
  delete(): void;
}

/** The Embind class truehd_bindings.cpp's `TruehdDecoder` builds. */
export interface NativeTruehdDecoder {
  decode(accessUnit: Uint8Array): TruehdDecoded | RawFailure | null;
  lostData(): void;
  reset(): void;
  counters(): TruehdCounters;
  delete(): void;
}

/** The Embind module `apps/demos/wasm/truehd_bindings.cpp` builds - what `createIclForgeTruehdModule()` resolves to. */
export interface TruehdEmbindModule {
  probe(data: Uint8Array): TruehdStreamInfo | RawFailure;
  scanUnits(data: Uint8Array): { units: { offset: number; size: number }[]; skippedBytes: number } | RawFailure;
  TruehdScan: new (data: Uint8Array, restartSpacing: number) => NativeTruehdScan;
  TruehdDecoder: new (presentation: number, lenient: boolean) => NativeTruehdDecoder;
}

/** The MODULARIZE factory Emscripten attaches as `createIclForgeTruehdModule` - see apps/demos/wasm/CMakeLists.txt's link options. */
export type TruehdModuleFactory = (moduleOverrides?: Record<string, unknown>) => Promise<TruehdEmbindModule>;

async function loadEmscriptenGlue(glueUrl: string): Promise<TruehdModuleFactory> {
  const source = await (await fetch(glueUrl)).text();
  // createIclForgeTruehdModule is the MODULARIZE+EXPORT_NAME global the glue defines when evaluated
  // as a plain script - re-exporting it is what makes the Blob URL below `import`able, the same
  // technique ac4.ts uses for createIclForgeAc4Module.
  const blob = new Blob([source, "\nexport default createIclForgeTruehdModule;\n"], {
    type: "text/javascript",
  });
  const blobUrl = URL.createObjectURL(blob);
  try {
    const namespace = (await import(/* webpackIgnore: true */ blobUrl)) as { default: TruehdModuleFactory };
    return namespace.default;
  } finally {
    URL.revokeObjectURL(blobUrl);
  }
}

/**
 * Loads the TrueHD Embind module from `glueUrl` (the `iclforge_truehd.js` the build wrote). Its
 * `.wasm` is found next to it.
 */
export async function loadTruehdModule(glueUrl: string): Promise<TruehdEmbindModule> {
  const factory = await loadEmscriptenGlue(glueUrl);
  // Without locateFile the glue resolves its `.wasm` against the `blob:` URL it was imported from
  // and fails inside its own async init chain - see ac4.ts's header comment.
  return factory({ locateFile: (path: string) => new URL(path, glueUrl).href });
}

function isFailure(value: object | null): value is RawFailure {
  return value !== null && "error" in value;
}

function raise(failure: RawFailure): never {
  throw new TruehdError(failure.error as TruehdErrorKind, failure.message, failure.detail);
}

/**
 * Reads the first major sync in `data` (it may start anywhere before it) and the restart headers
 * after it. Throws {@link TruehdError} of kind {@link TruehdErrorKind.BadSync} when the data holds
 * no major sync.
 */
export function truehdProbe(module: TruehdEmbindModule, data: Uint8Array): TruehdStreamInfo {
  const found = module.probe(data);
  if (isFailure(found)) {
    raise(found);
  }
  return found;
}

/**
 * Splits a bare TrueHD or MLP stream into its access units, as views into `data` (nothing is
 * copied). Throws {@link TruehdError} of kind {@link TruehdErrorKind.BadSync} for data with no unit
 * that reads.
 */
export function truehdScanUnits(module: TruehdEmbindModule, data: Uint8Array): TruehdUnits {
  const found = module.scanUnits(data);
  if (isFailure(found)) {
    raise(found);
  }
  return {
    units: found.units.map(({ offset, size }) => data.subarray(offset, offset + size)),
    skippedBytes: found.skippedBytes,
  };
}

/** The spacing a scan keeps its restarts at when none is given: 0.21 s at 48 kHz. */
export const TRUEHD_DEFAULT_RESTART_SPACING = 256;

/**
 * A stream held in memory, read once: what it is, how long it is, and where a decoder can join it.
 * The restart interval is the seek granule: reaching a place in the stream is a decode from the
 * last restart before it.
 *
 * ```ts
 * const stream = new TruehdScannedStream(module, bytes);
 * const restart = stream.restartForSample(stream.sampleAtSeconds(90));
 * // decode the units of stream.streamFrom(restart) from there
 * ```
 */
export class TruehdScannedStream {
  readonly #native: NativeTruehdScan;
  readonly #data: Uint8Array;
  readonly info: TruehdScanInfo;
  #closed = false;

  /**
   * Throws {@link TruehdError} (of kind {@link TruehdErrorKind.BadSync}) for data with no unit that
   * reads. Units are kept as joining points at least `restartSpacing` apart, whatever the stream
   * offers; 1 keeps every unit that carries a major sync, which are also the only places a cut or a
   * join may start.
   */
  constructor(module: TruehdEmbindModule, data: Uint8Array, restartSpacing: number = TRUEHD_DEFAULT_RESTART_SPACING) {
    const native = new module.TruehdScan(data, restartSpacing);
    const kind = native.errorKind();
    if (kind >= 0) {
      const failure = new TruehdError(kind as TruehdErrorKind, native.message(), native.detail());
      native.delete();
      throw failure;
    }
    this.#native = native;
    this.#data = data;
    this.info = native.info();
  }

  /** The units to join at, in order, the first unit among them and none closer to the last than the spacing. */
  restarts(): TruehdRestart[] {
    return this.#native.restarts();
  }

  /** The sample at a time, rounded down; a negative time is the first sample. */
  sampleAtSeconds(seconds: number): number {
    return this.#native.sampleAtSeconds(seconds);
  }

  /** Where to start decoding to reach `sample`: the restart at or before its unit. Null for a sample past the end. */
  restartForSample(sample: number): TruehdRestart | null {
    return this.#native.restartForSample(sample);
  }

  /** The stream from a restart on, as a view into the bytes that were scanned. */
  streamFrom(restart: TruehdRestart): Uint8Array {
    return this.#data.subarray(restart.offset);
  }

  /** Releases the underlying WASM object. Call when done - Embind instances are not garbage collected. */
  close(): void {
    if (this.#closed) return;
    this.#closed = true;
    this.#native.delete();
  }
}

/** A thin, typed wrapper over the Embind `TruehdDecoder` class - see this file's header comment. */
export class TruehdDecoder {
  readonly #native: NativeTruehdDecoder;
  #closed = false;

  constructor(module: TruehdEmbindModule, options: TruehdDecoderOptions = {}) {
    this.#native = new module.TruehdDecoder(options.presentation ?? TruehdPresentation.Best, options.lenient ?? false);
  }

  /**
   * Decodes one whole access unit (a unit from {@link truehdScanUnits}, or an MP4 sample). Null for a
   * unit that gave no samples because no restart header has come yet. Throws {@link TruehdError} for
   * a unit that failed.
   */
  decode(accessUnit: Uint8Array): TruehdDecoded | null {
    const result = this.#native.decode(accessUnit);
    if (isFailure(result)) {
      raise(result);
    }
    return result;
  }

  /** Tells the decoder that data between the last unit and the next was lost: it waits for a restart header. */
  lostData(): void {
    this.#native.lostData();
  }

  /** Forgets everything carried between units. */
  reset(): void {
    this.#native.reset();
  }

  /** What the decoder has seen so far. */
  counters(): TruehdCounters {
    return this.#native.counters();
  }

  /** Releases the underlying WASM object. Call when done - Embind instances are not garbage collected. */
  close(): void {
    if (this.#closed) return;
    this.#closed = true;
    this.#native.delete();
  }
}
