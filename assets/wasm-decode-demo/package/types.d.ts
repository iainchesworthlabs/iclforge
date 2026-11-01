/** iclforge::ac3::DownmixTarget's own numeric order (core/downmix_target.hpp) - kept in sync by hand, there being only four values. */
export declare enum DownmixTarget {
    AsCoded = 0,
    LoRo = 1,
    LtRt = 2,
    Mono = 3
}
/**
 * Annex C's reproduction of a karaoke stream (bsmod 7 above 1/0: AC-3, or E-AC-3 of one independent
 * substream), in iclforge::ac3::KaraokeReproduction's
 * own numeric order. Applied to the fold, so it only changes anything for a stream that is karaoke.
 */
export declare enum KaraokeReproduction {
    /** The coded channels, as they are. */
    AsCoded = 0,
    /** L, C and R with the vocals at the stream's own level (Table C.2.2). */
    Aware = 1,
    /** The listener chooses which vocals are heard (Table C.2.3; see {@link KaraokeVocals}). */
    Capable = 2
}
/** Which of a karaoke stream's vocals the {@link KaraokeReproduction.Capable} reproduction plays. */
export declare enum KaraokeVocals {
    None = 0,
    V1 = 1,
    V2 = 2,
    Both = 3
}
/** Which of PushDecoder's outputs feeds the AudioWorklet ring buffer - see decoder-worker.ts/decoder-node.ts. */
export type WriteTarget = "channels" | "fold";
export interface FoldOptions {
    target: DownmixTarget;
    /** §5.4.2.8 dialnorm normalisation - implied by nothing else, must be asked for explicitly. */
    applyDialnorm?: boolean;
    /** §7.8's optional LFE contribution to the fold. */
    mixLfe?: boolean;
    /**
     * Annex C's reproduction of a karaoke stream (AC-3, or E-AC-3 of one independent substream). At a stereo
     * or mono `target` it is Annex C's own 2/0
     * reproduction; at `AsCoded` it is three channels (four with the LFE): L, C and R. Default `AsCoded` (none).
     */
    karaoke?: KaraokeReproduction;
    /** Which vocals the `Capable` reproduction plays. Default `Both`. */
    karaokeVocals?: KaraokeVocals;
}
export interface PushMetadata {
    sampleRate: number;
    /** Samples per channel this call wrote - 256/512/768/1536 depending on numblkscod. */
    frameSamples: number;
    dialnorm: number;
    channelCount: number;
    channelLabels: string[];
    /** 0 when no fold was requested at construction. */
    foldChannelCount: number;
    /** Whether this unit is Annex C karaoke type: an AC-3 syncframe, or an E-AC-3 syncframe that is one independent substream with no object layer, flagged bsmod 7 above 1/0. Never an AC-4 unit. */
    karaoke: boolean;
    objectCount: number;
    objectLabels: string[];
}
export type PushOutcome = {
    ok: true;
    holdBack: true;
} | ({
    ok: true;
    holdBack: false;
} & PushMetadata) | {
    ok: false;
    error: string;
};
export interface FlushEntry {
    sampleRate: number;
    frameSamples: number;
    dialnorm: number;
    channelCount: number;
    channelLabels: string[];
    flushIndex: number;
}
export interface ObjectFrame {
    label: string;
    /** [x, y, z, gain_db, width, depth, height] - see iclforge::objects::oba::DisplayObject's own comment. */
    position: Float32Array;
}
export type ScanOutcome = {
    ok: true;
    kind: string;
    sampleRate: number;
    accessUnits: readonly {
        offset: number;
        length: number;
    }[];
} | {
    ok: false;
    error: string;
};
/** The Embind class iclforge::ac3's WASM build exposes (apps/demos/wasm/decoder_bindings.cpp's `PushDecoder`). */
export interface NativePushDecoder {
    pushAccessUnit(bytes: Uint8Array): RawPushResult;
    flush(): RawFlushEntry[];
    flushedChannelPcm(index: number, channel: number): Float32Array | null;
    channelPcm(channel: number): Float32Array | null;
    foldChannelCount(): number;
    foldPcm(channel: number): Float32Array | null;
    objectCount(): number;
    objectPosition(object: number): Float32Array | null;
    objectAudioPcm(object: number): Float32Array | null;
    objectLabel(object: number): string;
    delete(): void;
}
export interface RawPushResult {
    ok: boolean;
    error?: string;
    holdBack?: boolean;
    sampleRate?: number;
    frameSamples?: number;
    dialnorm?: number;
    channelCount?: number;
    channelLabels?: string[];
    foldChannelCount?: number;
    karaoke?: boolean;
    objectCount?: number;
    objectLabels?: string[];
}
export interface RawFlushEntry {
    ok: boolean;
    holdBack: boolean;
    sampleRate: number;
    frameSamples: number;
    dialnorm: number;
    channelCount: number;
    channelLabels: string[];
    objectCount: number;
    flushIndex: number;
}
export interface RawScanResult {
    ok: boolean;
    error?: string;
    kind?: string;
    sampleRate?: number;
    accessUnits?: {
        offset: number;
        length: number;
    }[];
}
/** The Embind module `apps/demos/wasm/decoder_bindings.cpp` builds - what `createIclForgeModule()` resolves to. */
export interface IclForgeEmbindModule {
    PushDecoder: new (foldTarget: number, foldApplyDialnorm: boolean, foldMixLfe: boolean, karaoke: number, karaokeVocals: number) => NativePushDecoder;
    scanStream(bytes: Uint8Array): RawScanResult;
}
/** The MODULARIZE factory Emscripten attaches as `createIclForgeModule` - see apps/demos/wasm/CMakeLists.txt's link options. */
export type IclForgeModuleFactory = (moduleOverrides?: Record<string, unknown>) => Promise<IclForgeEmbindModule>;
//# sourceMappingURL=types.d.ts.map