// A singer's microphone for the decode demo (planning/karaoke-microphone.md, M7; D16).
//
//   getUserMedia (the browser's own echo cancellation on: U6) -> an AudioWorklet that hands over
//   each render quantum -> the library's tracker, compiled into this page's module
//   (apps/demos/wasm/pitch_bindings.cpp) -> the library's scorer against the guide melody.
//
// What it promises, and what holds it to that: the microphone is off until a person presses the
// button; nothing is recorded (there is no encoder here and no copy of the samples is kept past the
// tracker); and nothing leaves the page (no fetch is made with them). The score is shown only when
// the person says they wear headphones, because through open speakers the microphone hears the
// backing and a score reads as nearly perfect for no singing (the plan's F6).
//
// The page owns playback (one AudioContext, one BufferSource); this module owns the microphone. The
// two are tied by that context's clock: a frame of the voice is placed in the item at where the
// playing sound was when the voice was sung, which is the time the frame reached the worklet less
// the microphone's own latency, the output's latency and the playback's start.

// The worklet, inlined as a Blob so that the demo stays a flat directory of files (the encode page
// does the same). It forwards 128-frame quanta, with the frame number of the first sample, so that
// a dropped quantum is seen and not placed.
export const CAPTURE_WORKLET = `
class IclForgeMicCapture extends AudioWorkletProcessor {
    process(inputs) {
        const input = inputs[0];
        if (input.length > 0 && input[0].length > 0) {
            this.port.postMessage({ frame: currentFrame, samples: Float32Array.from(input[0]) });
        }
        return true;
    }
}
registerProcessor('iclforge-mic-capture', IclForgeMicCapture);
`;

// --- the tune -------------------------------------------------------------------------------

// Which coded channel carries the tune of a karaoke stream: the guide melody M where the stream
// has a centre (Annex C, Table C.2.1: L M R, with V1 and V2 where the surrounds would be), else
// the first vocal. Labels are the decoder's channel labels. -1 when there is nothing to follow.
export function guideChannelIndex(labels) {
    const find = (...names) => names.map((n) => labels.indexOf(n)).find((i) => i >= 0) ?? -1;
    const centre = find('C');
    if (centre >= 0) {
        return centre;
    }
    return find('Ls', 'S', 'Rs');
}

// Which part that channel is, for the page to say: "the melody guide (M)" or "vocal 1 (V1)".
export function guidePartName(labels, index) {
    if (index < 0 || index >= labels.length) {
        return '';
    }
    return labels[index] === 'C' ? 'the melody guide (M)'
         : (labels[index] === 'Rs' ? 'vocal 2 (V2)' : 'vocal 1 (V1)');
}

// Reads the contour of one channel in slices, so that the page stays alive while it is read (a
// song is minutes of audio). `onProgress(0..1)` is called between slices. Resolves with the
// module's ReferenceContour, or null when the channel cannot be tracked or `signal` was aborted.
export async function readReference(module, pcm, sampleRate, { onProgress, signal, sliceSeconds = 2 } = {}) {
    const builder = new module.ReferenceBuilder(sampleRate, pcm.length);
    try {
        if (!builder.valid()) {
            return null;
        }
        const slice = Math.max(1, Math.round(sampleRate * sliceSeconds));
        for (let at = 0; at < pcm.length; at += slice) {
            if (signal?.aborted) {
                return null;
            }
            builder.push(pcm.subarray(at, Math.min(pcm.length, at + slice)));
            if (onProgress) {
                onProgress(Math.min(1, (at + slice) / pcm.length));
            }
            // Let the page paint and answer a press between slices.
            await new Promise((resolve) => setTimeout(resolve, 0));
        }
        return builder.finish();
    } finally {
        builder.delete();
    }
}

// --- time -----------------------------------------------------------------------------------

// Where in the item the sound the singer was hearing was when a frame of their voice was sung.
// `contextTime` is when the frame reached the worklet, on the playing context's clock, in seconds;
// the voice was sung `inputLatencySec` before that, and what was heard then left the output
// `outputLatencySec` after it was rendered. `offsetMs` is what the person moved.
export function itemMsAt(contextTime, { playStartCtxTime, inputLatencySec = 0, outputLatencySec = 0, offsetMs = 0 }) {
    return (contextTime - inputLatencySec - outputLatencySec - playStartCtxTime) * 1000 + offsetMs;
}

// A frame's context time: the frame number the worklet gave the stream's first sample, plus the
// frame's centre sample, over the rate.
export function frameContextTime(originFrame, centreSample, sampleRate) {
    return (originFrame + centreSample) / sampleRate;
}

// --- the ribbon -------------------------------------------------------------------------------

// Draws the tune as notes and the singer's voice over them into a 2D canvas context: the same
// picture as Hearth's pitch ribbon (apps/hearth/ui/assets/qml/PitchRibbon.qml). `trace` is the
// singer's recent frames as { ms, midi (-1: not voiced), hit }. A voice an octave away is drawn on
// the tune's octave, as it is scored.
export function drawRibbon(ctx, width, height, { notes, trace, positionMs, windowMs = 6000, nowFraction = 0.3,
                                                  lowMidi = 55, highMidi = 79, colors }) {
    const palette = { note: '#60a5fa', line: '#e2e8f0', miss: '#94a3b8', now: '#94a3b8', ...colors };
    const from = positionMs - windowMs * nowFraction;
    const x = (ms) => ((ms - from) / windowMs) * width;
    const span = Math.max(1, highMidi - lowMidi);
    const margin = height * 0.08;
    const y = (midi) => margin + (1 - (midi - lowMidi) / span) * (height - 2 * margin);

    ctx.clearRect(0, 0, width, height);
    const barHeight = Math.max(4, height * 0.07);
    for (let i = 0; i + 2 < notes.length; i += 3) {
        const x0 = x(notes[i]);
        const x1 = x(notes[i + 1]);
        if (x1 < 0 || x0 > width) {
            continue;
        }
        const current = positionMs >= notes[i] && positionMs < notes[i + 1];
        ctx.globalAlpha = current ? 0.85 : 0.4;
        ctx.fillStyle = palette.note;
        ctx.fillRect(x0, y(notes[i + 2]) - barHeight / 2, Math.max(2, x1 - x0 - 1), barHeight);
    }
    ctx.globalAlpha = 1;
    ctx.strokeStyle = palette.now;
    ctx.lineWidth = 1;
    ctx.setLineDash([]);
    ctx.beginPath();
    ctx.moveTo(x(positionMs), 0);
    ctx.lineTo(x(positionMs), height);
    ctx.stroke();

    let previous = null;
    for (const point of trace) {
        if (point.midi < 0) {
            previous = null;
            continue;
        }
        const here = { x: x(point.ms), y: y(point.midi), ms: point.ms };
        if (previous && here.ms - previous.ms <= 60 && here.ms >= previous.ms) {
            ctx.strokeStyle = point.hit ? palette.line : palette.miss;
            ctx.lineWidth = point.hit ? Math.max(3, height * 0.035) : Math.max(1.5, height * 0.015);
            ctx.setLineDash(point.hit ? [] : [4, 3]);
            ctx.beginPath();
            ctx.moveTo(previous.x, previous.y);
            ctx.lineTo(here.x, here.y);
            ctx.stroke();
        }
        previous = here;
    }
    ctx.setLineDash([]);
}

// A line's mark has three levels: under 45 %, under 75 %, and 75 % or more. Where they fall is
// presentation, not measurement; the percentage is shown beside the mark (Hearth's bands).
export function levelFor(percent) {
    return percent >= 75 ? 3 : (percent >= 45 ? 2 : 1);
}

// A line the singer was voiced for less than this share of its notes' time is "not sung".
export const NOT_SUNG_COVERAGE = 0.2;

// MIDI note number of a frequency, and its name with the octave: 69 is A4.
export function midiOfHz(hz) {
    return 69 + 12 * Math.log2(hz / 440);
}
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
export function noteName(hz) {
    if (!(hz > 0)) {
        return '';
    }
    const midi = Math.round(midiOfHz(hz));
    return NOTE_NAMES[((midi % 12) + 12) % 12] + (Math.floor(midi / 12) - 1);
}

// --- the session ------------------------------------------------------------------------------

// How long a trace is kept, in item milliseconds: more than the ribbon's window.
const TRACE_KEEP_MS = 12000;

export class MicrophoneSession {
    // module        the Embind module (PitchTracker, PitchScorer)
    // reference     a ReferenceContour from readReference(), or null: the pitch is shown, not scored
    // audioContext  the context playback uses
    // playback()    -> { playing, startCtxTime } as of now: where playback began on that context's clock
    // onUpdate()    called whenever there is something new to show
    // mediaDevices  navigator.mediaDevices, or a stand-in in a test
    // createCaptureNode(audioContext) -> a node with a `port`, or a stand-in in a test
    constructor({ module, reference, audioContext, playback, onUpdate = () => {}, mediaDevices = navigator.mediaDevices,
                  toleranceCents = 100,
                  createCaptureNode = (ctx) => new AudioWorkletNode(ctx, 'iclforge-mic-capture',
                                                                    { numberOfInputs: 1, numberOfOutputs: 1 }) }) {
        this.module = module;
        this.reference = reference;
        this.ctx = audioContext;
        this.playback = playback;
        this.onUpdate = onUpdate;
        this.mediaDevices = mediaDevices;
        this.createCaptureNode = createCaptureNode;
        this.toleranceCents = toleranceCents;

        this.headphones = false;
        this.inputLatencySec = 0;   // declared; the track's own figure when it gives one
        this.offsetMs = 0;
        this.on = false;
        this.level = 0;             // 0..1, smoothed RMS over -60..0 dBFS
        this.live = { voiced: false, hz: 0, itemMs: null, cents: null };
        this.trace = [];            // { ms, midi, hit }
        this.lastMs = 0;

        this._tracker = null;
        this._scorer = null;
        this._stream = null;
        this._node = null;
        this._source = null;
        this._silence = null;
        this._origin = null;        // the frame number of the stream's first sample
        this._expected = null;      // the frame number the next quantum should start at
        this._streamSamples = 0;
        this._summary = null;
        this._opening = null;       // a start() that has not finished: { stopped }
    }

    get score() {
        if (!this._scorer || !this.reference) {
            return null;
        }
        const total = this._scorer.total();
        return {
            withheld: !this.headphones,
            percent: total.counted > 0 && total.sung > 0 ? Math.round(total.fraction * 100) : -1,
            coveragePercent: Math.round(total.coverage * 100),
            meanCents: total.meanCents,
            lagMs: this._scorer.lagMs(),
            counted: total.counted,
        };
    }

    // A line's result once it has been sung, for [startMs, endMs) of the item: `scored` is false for
    // a line the tune has no note in (no mark, which is not a zero), when there is no tune, when the
    // microphone is off, and without the headphones answer (`withheld`); `notSung` is a line the
    // singer was voiced for under a fifth of its notes' time, which is not a bad score.
    lineResult(startMs, endMs) {
        const out = { scored: false, withheld: !this.headphones, notSung: false, percent: -1, level: 0 };
        if (!this.on || !this._scorer || !this.reference || !this.headphones || !(endMs > startMs)) {
            return out;
        }
        const tally = this._scorer.tally(startMs, endMs);
        if (!tally.scored) {
            return out;
        }
        out.scored = true;
        if (tally.coverage < NOT_SUNG_COVERAGE) {
            out.notSung = true;
            return out;
        }
        out.percent = Math.round(tally.fraction * 100);
        out.level = levelFor(out.percent);
        return out;
    }

    // What the last performance came to; null before one has been scored, and when the person never
    // said they wore headphones (that score is never shown).
    get summary() {
        return this._summary;
    }

    // Opens the microphone: a person's press, and the only thing that does. Throws with a sentence
    // for the page when it will not open, and then nothing is left open: opening takes its steps in
    // order and any of them can fail (the person refuses, or the page may not load a worklet from a
    // Blob), so what the steps before it made is let go here. A microphone that had opened is
    // closed, as the page will say it is.
    //
    // Opening can take as long as a person likes: the browser asks them, and waits. stop() in that
    // time ends it, and then this resolves with the session off and throws nothing, whatever the
    // browser goes on to answer, because closing it is what was asked for. So once this has
    // returned, `on` says whether the microphone is open. A call while one is already opening
    // does nothing: there is one microphone to open.
    async start({ deviceId } = {}) {
        if (this.on || this._opening) {
            return;
        }
        this._summary = null;
        const opening = { stopped: false };
        this._opening = opening;
        try {
            await this._open(deviceId, opening);
        } catch (err) {
            // stop() came while it was opening and let go of what there was: what is held by now
            // may be a later start()'s. What went wrong after that is not the page's to hear.
            if (opening.stopped) {
                return;
            }
            this._opening = null;
            this.on = false;
            this._release();
            throw err;
        }
    }

    // The steps of start(), which lets go of what they made when one of them throws. After each
    // wait, `opening.stopped` says that stop() came during it: stop() let go of what was held
    // then, so all that is left is what the wait itself handed over.
    async _open(deviceId, opening) {
        this._tracker = new this.module.PitchTracker(this.ctx.sampleRate);
        if (!this._tracker.valid()) {
            throw new Error(`This browser's audio runs at ${this.ctx.sampleRate} Hz, which the pitch tracker cannot follow.`);
        }
        const stream = await this.mediaDevices.getUserMedia({
            audio: {
                ...(deviceId ? { deviceId: { exact: deviceId } } : {}),
                channelCount: { ideal: 1 },
                // The browser's own echo cancellation is what stands between the backing and the
                // score (plan U6); noise suppression and gain control would shape the voice.
                echoCancellation: true,
                noiseSuppression: false,
                autoGainControl: false,
            },
        });
        if (opening.stopped) {
            // The microphone the browser has just handed over: nothing else has hold of it, and
            // nothing would ever close it.
            for (const track of stream.getTracks?.() ?? []) {
                track.stop();
            }
            return;
        }
        this._stream = stream;
        const settings = this._stream.getAudioTracks()[0]?.getSettings?.() ?? {};
        if (typeof settings.latency === 'number' && settings.latency > 0) {
            this.inputLatencySec = settings.latency;
        }

        const url = URL.createObjectURL(new Blob([CAPTURE_WORKLET], { type: 'text/javascript' }));
        try {
            await this.ctx.audioWorklet.addModule(url);
        } finally {
            URL.revokeObjectURL(url);
        }
        if (opening.stopped) {
            return;
        }
        this._source = this.ctx.createMediaStreamSource(this._stream);
        this._node = this.createCaptureNode(this.ctx);
        // A node nothing listens to may not be pulled: a muted gain keeps it in the graph without
        // putting the microphone in the speakers.
        this._silence = this.ctx.createGain();
        this._silence.gain.value = 0;
        this._source.connect(this._node);
        this._node.connect(this._silence);
        this._silence.connect(this.ctx.destination);

        if (this.reference) {
            this._scorer = new this.module.PitchScorer(this.reference, this.toleranceCents);
        }
        this._origin = null;
        this._expected = null;
        this._streamSamples = 0;
        this.trace = [];
        this._opening = null;
        this.on = true;
        this._node.port.onmessage = (event) => this.onQuantum(event.data);
        this.onUpdate();
    }

    // Closes it and keeps what it came to (when the headphones answer was given). One that is
    // still opening is ended here: what start() holds so far is let go at once, the microphone
    // with it when the browser had already handed it over, and start() closes one that the
    // browser hands over later. It was never on, so there is nothing to keep and nothing new for
    // the page to draw.
    stop() {
        if (this._opening) {
            this._opening.stopped = true;
            this._opening = null;
            this._release();
            return;
        }
        if (!this.on) {
            return;
        }
        this._summary = this._takeSummary();
        this.on = false;
        this._release();
        this.onUpdate();
    }

    // Lets go of whatever is held, which after a start that failed part-way is only some of it: the
    // nodes out of the graph, the microphone's tracks ended, the module's objects deleted.
    _release() {
        if (this._node) {
            this._node.port.onmessage = null;
            this._node.disconnect();
        }
        this._source?.disconnect();
        this._silence?.disconnect();
        for (const track of this._stream?.getTracks?.() ?? []) {
            track.stop();
        }
        this._node = this._source = this._silence = this._stream = null;
        this._tracker?.delete();
        this._tracker = null;
        this._scorer?.delete();
        this._scorer = null;
        this.level = 0;
        this.live = { voiced: false, hz: 0, itemMs: null, cents: null };
    }

    _takeSummary() {
        const score = this.score;
        if (!score || score.withheld || score.counted === 0 || score.percent < 0) {
            return null;
        }
        return {
            percent: score.percent,
            coveragePercent: score.coveragePercent,
            meanCents: score.meanCents,
            lagMs: score.lagMs,
            toleranceCents: this.toleranceCents,
        };
    }

    // One render quantum of the microphone: { frame, samples }.
    onQuantum({ frame, samples }) {
        if (!this.on || !this._tracker) {
            return;
        }
        // A quantum that does not follow the last was dropped: the stream starts again from here,
        // so that nothing is placed against a time it was not captured at.
        if (this._origin === null || frame !== this._expected) {
            this._tracker.reset();
            this._origin = frame;
            this._streamSamples = 0;
        }
        this._expected = frame + samples.length;

        let energy = 0;
        for (let i = 0; i < samples.length; i++) {
            energy += samples[i] * samples[i];
        }
        const db = 20 * Math.log10(Math.sqrt(energy / samples.length) + 1e-9);
        const level = Math.min(1, Math.max(0, (db + 60) / 60));
        this.level = Math.max(level, this.level * 0.92);

        const flat = this._tracker.push(samples);
        const rate = this.ctx.sampleRate;
        const playback = this.playback();
        for (let i = 0; i + 3 < flat.length; i += 4) {
            const centre = flat[i];
            const hz = flat[i + 1];
            const contextTime = frameContextTime(this._origin, centre, rate);
            const itemMs = itemMsAt(contextTime, {
                playStartCtxTime: playback.startCtxTime,
                inputLatencySec: this.inputLatencySec,
                outputLatencySec: this.ctx.outputLatency || this.ctx.baseLatency || 0,
                offsetMs: this.offsetMs,
            });
            this._place(itemMs, hz, playback.playing);
        }
        this._streamSamples += samples.length;
        this.onUpdate();
    }

    _place(itemMs, hz, playing) {
        const voiced = hz > 0;
        let midi = -1;
        let hit = false;
        let cents = null;
        if (voiced) {
            midi = midiOfHz(hz);
            const target = this.reference ? this.reference.hzAt(itemMs) : 0;
            if (target > 0) {
                const targetMidi = midiOfHz(target);
                midi += 12 * Math.round((targetMidi - midi) / 12);
                cents = (midi - targetMidi) * 100;
                hit = Math.abs(cents) <= this.toleranceCents;
            }
        }
        this.live = { voiced, hz, itemMs, cents };
        // Nothing is scored while the item is not playing: the voice is not placed anywhere.
        if (playing && this._scorer && itemMs >= 0) {
            this._scorer.add(itemMs, voiced ? hz : 0);
        }
        if (playing) {
            this.lastMs = itemMs;
            this.trace.push({ ms: itemMs, midi, hit });
            const cut = itemMs - TRACE_KEEP_MS;
            while (this.trace.length > 0 && this.trace[0].ms < cut) {
                this.trace.shift();
            }
        }
    }
}
