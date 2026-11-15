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
export var TruehdErrorKind;
(function (TruehdErrorKind) {
    /** The data ends inside a field or a unit. */
    TruehdErrorKind[TruehdErrorKind["Truncated"] = 0] = "Truncated";
    /** No major sync, or no access unit, where one was required. */
    TruehdErrorKind[TruehdErrorKind["BadSync"] = 1] = "BadSync";
    /** TH spec §3.2: the nibbles of the header and directory do not XOR to 0xF. */
    TruehdErrorKind[TruehdErrorKind["BadCheckNibble"] = 2] = "BadCheckNibble";
    /** A length, an end pointer or a size outside the data or outside its limit. */
    TruehdErrorKind[TruehdErrorKind["BadLength"] = 3] = "BadLength";
    /** TH spec §4.3: the major sync's signature is not 0xB752. */
    TruehdErrorKind[TruehdErrorKind["BadSignature"] = 4] = "BadSignature";
    /** TH spec §4.1: the CRC-16 of the major sync. */
    TruehdErrorKind[TruehdErrorKind["BadMajorSyncCrc"] = 5] = "BadMajorSyncCrc";
    /** TH spec §7.2. */
    TruehdErrorKind[TruehdErrorKind["BadRestartSync"] = 6] = "BadRestartSync";
    /** TH spec §7.1: the CRC-8 of the restart header. */
    TruehdErrorKind[TruehdErrorKind["BadRestartCrc"] = 7] = "BadRestartCrc";
    /** TH spec §6.2: the parity byte at the end of a substream segment. */
    TruehdErrorKind[TruehdErrorKind["BadParity"] = 8] = "BadParity";
    /** TH spec §6.2: the CRC-8 at the end of a substream segment. */
    TruehdErrorKind[TruehdErrorKind["BadSubstreamCrc"] = 9] = "BadSubstreamCrc";
    /** A field outside the range the specification allows. */
    TruehdErrorKind[TruehdErrorKind["BadParameter"] = 10] = "BadParameter";
    /** A block whose sample data does not end where it says (TH spec §8.1), or a bad block. */
    TruehdErrorKind[TruehdErrorKind["BadBlockData"] = 11] = "BadBlockData";
    /** TH spec §7.3: the decoded samples do not match the restart header's check. */
    TruehdErrorKind[TruehdErrorKind["BadLosslessCheck"] = 12] = "BadLosslessCheck";
    /** A substream needs a restart header (or a major sync) it has not seen. */
    TruehdErrorKind[TruehdErrorKind["NeedsRestart"] = 13] = "NeedsRestart";
    /** Valid, and a form this version does not decode (the detail names it). */
    TruehdErrorKind[TruehdErrorKind["Unsupported"] = 14] = "Unsupported";
})(TruehdErrorKind || (TruehdErrorKind = {}));
/** iclforge::truehd::Presentation's own numeric order (decoder.hpp) - kept in sync by hand. */
export var TruehdPresentation;
(function (TruehdPresentation) {
    /** The highest of substreams 0 to 2 the stream has: the stereo, 5.1 or 7.1 bed. */
    TruehdPresentation[TruehdPresentation["Best"] = 0] = "Best";
    /** Substream 0. */
    TruehdPresentation[TruehdPresentation["TwoChannel"] = 1] = "TwoChannel";
    /** Substreams 0 to 1. */
    TruehdPresentation[TruehdPresentation["SixChannel"] = 2] = "SixChannel";
    /** Substreams 0 to 2. */
    TruehdPresentation[TruehdPresentation["EightChannel"] = 3] = "EightChannel";
    /** Every substream, the sixteen-channel one last. */
    TruehdPresentation[TruehdPresentation["All"] = 4] = "All";
})(TruehdPresentation || (TruehdPresentation = {}));
/** What failed, as the library says it: the kind, its text, and the detail where it has one. */
export class TruehdError extends Error {
    kind;
    detail;
    constructor(kind, message, detail) {
        super(detail === "" ? message : `${message}: ${detail}`);
        this.name = "TruehdError";
        this.kind = kind;
        this.detail = detail;
    }
}
async function loadEmscriptenGlue(glueUrl) {
    const source = await (await fetch(glueUrl)).text();
    // createIclForgeTruehdModule is the MODULARIZE+EXPORT_NAME global the glue defines when evaluated
    // as a plain script - re-exporting it is what makes the Blob URL below `import`able, the same
    // technique ac4.ts uses for createIclForgeAc4Module.
    const blob = new Blob([source, "\nexport default createIclForgeTruehdModule;\n"], {
        type: "text/javascript",
    });
    const blobUrl = URL.createObjectURL(blob);
    try {
        const namespace = (await import(/* webpackIgnore: true */ blobUrl));
        return namespace.default;
    }
    finally {
        URL.revokeObjectURL(blobUrl);
    }
}
/**
 * Loads the TrueHD Embind module from `glueUrl` (the `iclforge_truehd.js` the build wrote). Its
 * `.wasm` is found next to it.
 */
export async function loadTruehdModule(glueUrl) {
    const factory = await loadEmscriptenGlue(glueUrl);
    // Without locateFile the glue resolves its `.wasm` against the `blob:` URL it was imported from
    // and fails inside its own async init chain - see ac4.ts's header comment.
    return factory({ locateFile: (path) => new URL(path, glueUrl).href });
}
function isFailure(value) {
    return value !== null && "error" in value;
}
function raise(failure) {
    throw new TruehdError(failure.error, failure.message, failure.detail);
}
/**
 * Reads the first major sync in `data` (it may start anywhere before it) and the restart headers
 * after it. Throws {@link TruehdError} of kind {@link TruehdErrorKind.BadSync} when the data holds
 * no major sync.
 */
export function truehdProbe(module, data) {
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
export function truehdScanUnits(module, data) {
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
    #native;
    #data;
    info;
    #closed = false;
    /**
     * Throws {@link TruehdError} (of kind {@link TruehdErrorKind.BadSync}) for data with no unit that
     * reads. Units are kept as joining points at least `restartSpacing` apart, whatever the stream
     * offers; 1 keeps every unit that carries a major sync, which are also the only places a cut or a
     * join may start.
     */
    constructor(module, data, restartSpacing = TRUEHD_DEFAULT_RESTART_SPACING) {
        const native = new module.TruehdScan(data, restartSpacing);
        const kind = native.errorKind();
        if (kind >= 0) {
            const failure = new TruehdError(kind, native.message(), native.detail());
            native.delete();
            throw failure;
        }
        this.#native = native;
        this.#data = data;
        this.info = native.info();
    }
    /** The units to join at, in order, the first unit among them and none closer to the last than the spacing. */
    restarts() {
        return this.#native.restarts();
    }
    /** The sample at a time, rounded down; a negative time is the first sample. */
    sampleAtSeconds(seconds) {
        return this.#native.sampleAtSeconds(seconds);
    }
    /** Where to start decoding to reach `sample`: the restart at or before its unit. Null for a sample past the end. */
    restartForSample(sample) {
        return this.#native.restartForSample(sample);
    }
    /** The stream from a restart on, as a view into the bytes that were scanned. */
    streamFrom(restart) {
        return this.#data.subarray(restart.offset);
    }
    /** Releases the underlying WASM object. Call when done - Embind instances are not garbage collected. */
    close() {
        if (this.#closed)
            return;
        this.#closed = true;
        this.#native.delete();
    }
}
/** A thin, typed wrapper over the Embind `TruehdDecoder` class - see this file's header comment. */
export class TruehdDecoder {
    #native;
    #closed = false;
    constructor(module, options = {}) {
        this.#native = new module.TruehdDecoder(options.presentation ?? TruehdPresentation.Best, options.lenient ?? false);
    }
    /**
     * Decodes one whole access unit (a unit from {@link truehdScanUnits}, or an MP4 sample). Null for a
     * unit that gave no samples because no restart header has come yet. Throws {@link TruehdError} for
     * a unit that failed.
     */
    decode(accessUnit) {
        const result = this.#native.decode(accessUnit);
        if (isFailure(result)) {
            raise(result);
        }
        return result;
    }
    /** Tells the decoder that data between the last unit and the next was lost: it waits for a restart header. */
    lostData() {
        this.#native.lostData();
    }
    /** Forgets everything carried between units. */
    reset() {
        this.#native.reset();
    }
    /** What the decoder has seen so far. */
    counters() {
        return this.#native.counters();
    }
    /** Releases the underlying WASM object. Call when done - Embind instances are not garbage collected. */
    close() {
        if (this.#closed)
            return;
        this.#closed = true;
        this.#native.delete();
    }
}
//# sourceMappingURL=truehd.js.map