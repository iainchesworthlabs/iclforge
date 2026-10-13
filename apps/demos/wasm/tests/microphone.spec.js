// @ts-check
'use strict';

const fs = require('fs');
const path = require('path');
const { test, expect } = require('@playwright/test');

// The decode demo's microphone (planning/karaoke-microphone.md, M7; D16): the library's pitch
// tracker, reference melody and scorer compiled into the demo's module (apps/demos/wasm/
// pitch_bindings.cpp), and the card that follows a microphone with them (mic.js).
//
// Two things are held. The arithmetic: the module's tracker finds a sung pitch, a contour is read
// from a channel in slices, and the scorer counts what is on the tune with octaves folded. And what
// the page promises: the microphone is off until the button is pressed, the score is withheld
// until the person says they wear headphones, nothing is sent anywhere, closing it stops the
// tracks, one that could not be opened all the way is not left open, and one that is closed while
// it is still opening (the browser asks the person, for as long as they like) never opens. The
// device is Chromium's fake one (with the permission prompt granted by the flags in
// playwright.config.js): no real microphone is opened by this suite. That device is not a tone: it
// is a beep of about 20 ms twice a second, and silence between.

const AULD_LANG_SYNE = path.join(__dirname, '../../../../examples/karaoke/auld-lang-syne/auld-lang-syne.ac3');
const AULD_LANG_SYNE_WORDS = path.join(__dirname, '../../../../examples/karaoke/auld-lang-syne/auld-lang-syne.lrc');
// A second of a sine in L and R: a stereo stream, which karaokeWithNoTune() relabels.
const STEREO = path.join(__dirname, '../../../../libs/ac3/fuzz/seeds/fuzz_ac3_decode/ac3-sine-stereo.ac3');

// A karaoke stream with nothing to score against: L and R, and no melody or vocal channel. No
// stream in the repository is coded that way, so a stereo one is relabelled: bsmod 7 above 1/0 is
// what says karaoke (Annex C), and it is three bits of each syncframe's header. Both of a
// syncframe's CRC words cover those bits and are made right again, so that the page is given a
// stream any decoder takes: crc1 stands before the first five eighths of the syncframe that it
// covers, and crc2 ends the whole.
function karaokeWithNoTune() {
    const bytes = fs.readFileSync(STEREO);
    // CRC-16 with x^16 + x^15 + x^2 + 1, most significant bit first, from zero.
    const crc16 = (data) => {
        let crc = 0;
        for (const byte of data) {
            crc ^= byte << 8;
            for (let bit = 0; bit < 8; bit++) {
                crc = ((crc << 1) ^ (crc & 0x8000 ? 0x8005 : 0)) & 0xffff;
            }
        }
        return crc;
    };
    const KBPS = [32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640];
    let frames = 0;
    for (let at = 0; at < bytes.length; frames++) {
        expect(bytes.readUInt16BE(at), 'a syncword').toBe(0x0b77);
        expect(bytes[at + 4] >> 6, 'a stream at 48 kHz').toBe(0);
        // At 48 kHz a syncframe is four bytes for each kbit/s.
        const size = 4 * KBPS[(bytes[at + 4] & 0x3f) >> 1];
        const frame = bytes.subarray(at, at + size);
        expect(frame[6] >> 5, 'a 2/0 stream').toBe(2);
        frame[5] |= 0x07;
        // crc1 is the word that, read before the rest of the first five eighths, leaves no
        // remainder: the remainder of the rest, taken back through every bit from crc1 on.
        const fiveEighths = ((size >> 2) + (size >> 4)) << 1;
        let crc1 = crc16(frame.subarray(4, fiveEighths));
        for (let bit = 0; bit < 8 * fiveEighths - 16; bit++) {
            crc1 = crc1 & 1 ? ((crc1 ^ 0x8005) >> 1) | 0x8000 : crc1 >> 1;
        }
        frame.writeUInt16BE(crc1, 2);
        expect(crc16(frame.subarray(2, fiveEighths)), 'crc1').toBe(0);
        frame.writeUInt16BE(crc16(frame.subarray(2, size - 2)), size - 2);
        at += size;
    }
    expect(frames).toBeGreaterThan(0);
    return bytes;
}

test('the tracker finds a sung pitch at the rates a browser runs at', async ({ page }) => {
    await page.goto('index.html');
    const result = await page.evaluate(async () => {
        // @ts-ignore
        const module = await window.createIclForgeModule();
        const rate = 48000;
        const tracker = new module.PitchTracker(rate);
        const frames = [];
        // A 220 Hz tone with a little vibrato, in the 128-sample quanta a worklet gives.
        const total = rate * 1;
        let phase = 0;
        for (let at = 0; at < total; at += 128) {
            const block = new Float32Array(128);
            for (let i = 0; i < 128; i++) {
                const t = (at + i) / rate;
                phase += (2 * Math.PI * (220 + 3 * Math.sin(2 * Math.PI * 5 * t))) / rate;
                block[i] = 0.4 * Math.sin(phase);
            }
            const flat = tracker.push(block);
            for (let i = 0; i + 3 < flat.length; i += 4) {
                frames.push({ centre: flat[i], hz: flat[i + 1], clarity: flat[i + 2] });
            }
        }
        const voiced = frames.filter((f) => f.hz > 0).map((f) => f.hz).sort((a, b) => a - b);
        const median = voiced[Math.floor(voiced.length / 2)];
        const hop = tracker.hopMs();
        tracker.delete();
        const validAt = (hz) => {
            const t = new module.PitchTracker(hz);
            const ok = t.valid();
            t.delete();
            return ok;
        };
        return { count: frames.length, voiced: voiced.length, median, hop,
                 rates: { r44100: validAt(44100), r48000: validAt(48000), r96000: validAt(96000), zero: validAt(0) } };
    });
    // About a frame every 10 ms for a second, less the first window.
    expect(result.count).toBeGreaterThan(80);
    expect(result.hop).toBe(10);
    expect(result.voiced).toBeGreaterThan(result.count * 0.9);
    expect(Math.abs(result.median - 220)).toBeLessThan(3);
    // The rates a browser's audio runs at are followed; no rate is not.
    expect(result.rates).toEqual({ r44100: true, r48000: true, r96000: true, zero: false });
});

test('a contour is read in slices and scored with octaves folded', async ({ page }) => {
    await page.goto('index.html');
    const result = await page.evaluate(async () => {
        // @ts-ignore
        const module = await window.createIclForgeModule();
        const { readReference } = await import('./mic.js');
        const rate = 48000;
        // A3 for a second, a gap, then C4 for a second.
        const pcm = new Float32Array(rate * 3);
        const tone = (hz, from, to) => {
            for (let i = from; i < to; i++) {
                pcm[i] = 0.4 * Math.sin((2 * Math.PI * hz * i) / rate);
            }
        };
        tone(220, 0, rate);
        tone(261.63, rate * 2, rate * 3);

        let progress = 0;
        const reference = await readReference(module, pcm, rate, {
            sliceSeconds: 0.5,
            onProgress: (share) => { progress = Math.max(progress, share); },
        });
        const notes = Array.from(reference.notes());
        const out = {
            progress,
            voicedFrames: reference.voicedFrames(),
            hop: reference.hopMs(),
            noteCount: notes.length / 3,
            midi: [notes[2], notes[5]],
            starts: [notes[0], notes[3]],
            hzInFirst: reference.hzAt(500),
            hzInGap: reference.hzAt(1500),
        };

        const scorer = new module.PitchScorer(reference, 100);
        // The singer is on the first note (A3), an octave down on the second (C3: 130.81 Hz), and
        // silent for the gap. Positions are the item's, in ms, every 10 ms.
        for (let ms = 100; ms < 900; ms += 10) scorer.add(ms, 220);
        for (let ms = 2100; ms < 2900; ms += 10) scorer.add(ms, 130.81);
        out.onTune = scorer.total();
        out.firstNote = scorer.tally(0, 1000);
        out.gap = scorer.tally(1100, 1900);
        scorer.clear();
        // The same, but a tritone away from the first note.
        for (let ms = 100; ms < 900; ms += 10) scorer.add(ms, 311.13);
        out.off = scorer.tally(0, 1000);
        scorer.delete();
        reference.delete();
        return out;
    });
    expect(result.progress).toBe(1);
    expect(result.hop).toBe(10);
    expect(result.voicedFrames).toBeGreaterThan(150);
    expect(result.noteCount).toBe(2);
    expect(Math.abs(result.midi[0] - 57)).toBeLessThan(0.3);   // A3
    expect(Math.abs(result.midi[1] - 60)).toBeLessThan(0.3);   // C4
    expect(Math.abs(result.hzInFirst - 220)).toBeLessThan(3);
    expect(result.hzInGap).toBe(0);
    // On the tune, and the octave below counts: octaves are folded.
    expect(result.onTune.fraction).toBeGreaterThan(0.6);
    expect(result.firstNote.scored).toBe(true);
    expect(result.firstNote.fraction).toBeGreaterThan(0.7);
    // A gap in the tune is not scored, which is not a score of zero.
    expect(result.gap.scored).toBe(false);
    expect(result.gap.counted).toBe(0);
    // A tritone away is voiced and wrong.
    expect(result.off.sung).toBeGreaterThan(0);
    expect(result.off.fraction).toBeLessThan(0.1);
});

test('the guide melody is the centre channel, and where a frame was sung follows the clock', async ({ page }) => {
    await page.goto('index.html');
    const result = await page.evaluate(async () => {
        const { guideChannelIndex, guidePartName, itemMsAt, frameContextTime, noteName } = await import('./mic.js');
        return {
            m: guideChannelIndex(['L', 'C', 'R', 'Ls', 'Rs']),
            v1Only: guideChannelIndex(['L', 'R', 'S']),
            none: guideChannelIndex(['L', 'R']),
            mName: guidePartName(['L', 'C', 'R', 'Ls', 'Rs'], 1),
            v1Name: guidePartName(['L', 'R', 'S'], 2),
            // The voice reached the worklet at 12.000 s of the context; playback began at 10.000 s;
            // the microphone says 30 ms, the output 20 ms; the person moved 40 ms later.
            ms: itemMsAt(12.0, { playStartCtxTime: 10.0, inputLatencySec: 0.03, outputLatencySec: 0.02, offsetMs: 40 }),
            noDelays: itemMsAt(12.0, { playStartCtxTime: 10.0 }),
            at: frameContextTime(96000, 2400, 48000),
            note: noteName(440),
            nothing: noteName(0),
        };
    });
    expect(result.m).toBe(1);
    expect(result.v1Only).toBe(2);
    expect(result.none).toBe(-1);
    expect(result.mName).toBe('the melody guide (M)');
    expect(result.v1Name).toBe('vocal 1 (V1)');
    expect(result.ms).toBeCloseTo(2000 - 30 - 20 + 40, 6);
    expect(result.noDelays).toBeCloseTo(2000, 6);
    expect(result.at).toBeCloseTo(2.05, 9);
    expect(result.note).toBe('A4');
    expect(result.nothing).toBe('');
});

test('a session places the voice where the sound it sang along to was, and scores it', async ({ page }) => {
    await page.goto('index.html');
    const out = await page.evaluate(async () => {
        // @ts-ignore
        const module = await window.createIclForgeModule();
        const { MicrophoneSession, readReference } = await import('./mic.js');
        const rate = 48000;

        // The tune: A3 for the first second of the item, C4 for the second.
        const tune = new Float32Array(rate * 2);
        for (let i = 0; i < tune.length; i++) {
            const hz = i < rate ? 220 : 261.63;
            tune[i] = 0.4 * Math.sin((2 * Math.PI * hz * i) / rate);
        }
        const reference = await readReference(module, tune, rate, {});

        // A voice that is on the tune: the item's time t is sung at pitch(t), and reaches the worklet
        // when the sound it sang along to has been heard: playback began at 10 s on the context's
        // clock, the microphone says 30 ms and the output 20 ms.
        const startCtx = 10.0;
        const firstFrame = Math.round((startCtx + 0.05) * rate);
        // `awayFrom` is a sample from which the voice is six semitones off: a line half sung right.
        const quanta = (fromSample, toSample, semitones = 0, awayFrom = Infinity) => {
            const list = [];
            for (let at = fromSample; at < toSample; at += 128) {
                const samples = new Float32Array(128);
                for (let i = 0; i < 128; i++) {
                    const n = at + i;
                    const off = n >= awayFrom ? 6 : semitones;
                    const hz = (n < rate ? 220 : 261.63) * Math.pow(2, off / 12);
                    samples[i] = 0.4 * Math.sin((2 * Math.PI * hz * n) / rate);
                }
                list.push({ frame: firstFrame + at, samples });
            }
            return list;
        };

        const fakeTrack = { readyState: 'live', getSettings: () => ({ latency: 0.03 }), stop() { this.readyState = 'ended'; } };
        const run = async ({ offsetMs = 0, headphones = true, playing = true, semitones = 0, drop = false,
                            awayFrom = Infinity }) => {
            const nodes = [];
            const ctx = {
                sampleRate: rate, outputLatency: 0.02, baseLatency: 0.01, destination: {},
                audioWorklet: { addModule: async () => {} },
                createMediaStreamSource: () => ({ connect() {}, disconnect() {} }),
                createGain: () => ({ gain: { value: 1 }, connect() {}, disconnect() {} }),
            };
            fakeTrack.readyState = 'live';
            const mediaDevices = { getUserMedia: async () => ({ getAudioTracks: () => [fakeTrack], getTracks: () => [fakeTrack] }) };
            const state = { playing, startCtxTime: startCtx };
            const session = new MicrophoneSession({
                module, reference, audioContext: ctx, playback: () => state, mediaDevices,
                createCaptureNode: () => { const node = { port: {}, connect() {}, disconnect() {} }; nodes.push(node); return node; },
            });
            session.headphones = headphones;
            session.offsetMs = offsetMs;
            await session.start();
            const list = quanta(0, rate * 2, semitones, awayFrom);
            list.forEach((q, index) => {
                // A dropped stretch: a quarter of a second of quanta never arrive, in the middle of
                // the first note. What follows must still be placed where it was sung.
                if (drop && index >= 100 && index < 200) return;
                nodes[0].port.onmessage({ data: q });
            });
            const voiced = session.trace.filter((p) => p.midi >= 0);
            const result = {
                latency: session.inputLatencySec,
                voicedCount: voiced.length,
                firstMs: voiced.length ? voiced[0].ms : null,
                // The first frame of the C4 half: where its pitch first reads as C4.
                secondNoteMs: (voiced.find((p) => Math.abs(p.midi - 60) < 0.5) || {}).ms ?? null,
                hits: voiced.filter((p) => p.hit).length,
                score: session.score,
                // A line's result: the first note (0 to 1 s of the item), and a stretch the tune has
                // no note in.
                firstLine: session.lineResult(0, 1000),
                beyondTune: session.lineResult(5000, 6000),
            };
            session.stop();
            result.summary = session.summary;
            result.trackEnded = fakeTrack.readyState === 'ended';
            result.portClosed = nodes[0].port.onmessage === null;
            return result;
        };

        const onTune = await run({});
        const shifted = await run({ offsetMs: 200 });
        const withheld = await run({ headphones: false });
        const away = await run({ semitones: 6 });
        const octave = await run({ semitones: -12 });
        const notPlaying = await run({ playing: false });
        const dropped = await run({ drop: true });
        // The first line on the tune for 0.6 s of its second, and off it for the rest.
        const partly = await run({ awayFrom: Math.round(rate * 0.6) });
        return { onTune, shifted, withheld, away, octave, notPlaying, dropped, partly };
    });

    // The track's own latency figure is where the delay starts.
    expect(out.onTune.latency).toBeCloseTo(0.03, 6);
    // Placed at the item's own time: the first voiced frame is near the start, the C4 near 1 s.
    expect(out.onTune.firstMs).toBeGreaterThanOrEqual(0);
    expect(out.onTune.firstMs).toBeLessThan(120);
    expect(Math.abs(out.onTune.secondNoteMs - 1000)).toBeLessThan(120);
    // On the tune: almost all voiced frames are within the tolerance, and the score says so.
    expect(out.onTune.hits).toBeGreaterThan(out.onTune.voicedCount * 0.9);
    expect(out.onTune.score.withheld).toBe(false);
    expect(out.onTune.score.percent).toBeGreaterThan(80);
    // What it came to is kept when it is stopped, with the basis.
    expect(out.onTune.summary.percent).toBeGreaterThan(80);
    expect(out.onTune.summary.toleranceCents).toBe(100);
    // Closing it ends the track and the capture.
    expect(out.onTune.trackEnded).toBe(true);
    expect(out.onTune.portClosed).toBe(true);

    // A line's mark: on the tune it is the top level with a high percentage; off it, the bottom; a
    // stretch the tune has no note in has none; and without the headphones answer there is no number.
    expect(out.onTune.firstLine.scored).toBe(true);
    expect(out.onTune.firstLine.level).toBe(3);
    expect(out.onTune.firstLine.percent).toBeGreaterThanOrEqual(75);
    expect(out.onTune.beyondTune.scored).toBe(false);
    expect(out.away.firstLine.scored).toBe(true);
    expect(out.away.firstLine.level).toBe(1);
    expect(out.away.firstLine.percent).toBeLessThan(25);
    // In between is the middle level, and its percentage is the share that was right.
    expect(out.partly.firstLine.level).toBe(2);
    expect(out.partly.firstLine.percent).toBeGreaterThanOrEqual(45);
    expect(out.partly.firstLine.percent).toBeLessThan(75);
    expect(out.withheld.firstLine.scored).toBe(false);
    expect(out.withheld.firstLine.withheld).toBe(true);
    // Nothing was sung while paused: the line is "not sung" (the tune has notes in it and the singer
    // was not voiced for any), which carries no number and is not a bad score.
    expect(out.notPlaying.firstLine.scored).toBe(true);
    expect(out.notPlaying.firstLine.notSung).toBe(true);
    expect(out.notPlaying.firstLine.percent).toBe(-1);

    // "Move your timing" moves where the voice is placed, by what was asked.
    expect(Math.abs(out.shifted.firstMs - out.onTune.firstMs - 200)).toBeLessThan(30);

    // Without the headphones answer the score is withheld, and no summary is made.
    expect(out.withheld.score.withheld).toBe(true);
    expect(out.withheld.summary).toBeNull();

    // A tritone away is voiced and wrong: a score of nearly none, and the line drawn off the tune.
    expect(out.away.voicedCount).toBeGreaterThan(100);
    expect(out.away.score.percent).toBeLessThan(15);
    expect(out.away.hits).toBeLessThan(out.away.voicedCount * 0.15);

    // An octave below is right: it is scored as on the tune, and drawn on the tune's octave.
    expect(out.octave.score.percent).toBeGreaterThan(80);
    expect(out.octave.hits).toBeGreaterThan(out.octave.voicedCount * 0.9);

    // Nothing is placed or scored while the item is not playing.
    expect(out.notPlaying.voicedCount).toBe(0);
    expect(out.notPlaying.score.percent).toBe(-1);

    // A dropped stretch of quanta does not shift what follows it: the C4 is still near 1 s.
    expect(Math.abs(out.dropped.secondNoteMs - 1000)).toBeLessThan(100);
    expect(out.dropped.score.percent).toBeGreaterThan(40);
});

test('a session that cannot open all the way closes what it had opened', async ({ page }) => {
    await page.goto('index.html');
    const out = await page.evaluate(async () => {
        // @ts-ignore
        const module = await window.createIclForgeModule();
        const { MicrophoneSession, readReference } = await import('./mic.js');
        const rate = 48000;
        // A tune, so that a session has a scorer to make: the last thing it makes.
        const tune = new Float32Array(rate);
        for (let i = 0; i < tune.length; i++) {
            tune[i] = 0.4 * Math.sin((2 * Math.PI * 220 * i) / rate);
        }
        const reference = await readReference(module, tune, rate, {});

        // Opening takes its steps in order: the tracker, the microphone, the worklet's module, the
        // nodes, the scorer, and a word to the page. `step` is the one that fails, with `thrown`.
        const run = async (step, thrown) => {
            let fail = step;
            const trip = (at) => {
                if (fail === at) {
                    throw thrown;
                }
            };
            // The module's own tracker and scorer, each kept so that it can be asked whether it was
            // deleted: one that is not is memory the page never gets back.
            const made = [];
            const kept = (Type, at) => function (...args) {
                trip(at);
                const object = new Type(...args);
                made.push(object);
                return object;
            };
            const tracks = [];
            const nodes = [];
            const node = (more) => {
                const one = { disconnected: false, connect() {}, disconnect() { this.disconnected = true; }, ...more };
                nodes.push(one);
                return one;
            };
            const ctx = {
                // A rate the tracker cannot follow is the first thing that can stop it.
                get sampleRate() { return fail === 'rate' ? 0 : rate; },
                outputLatency: 0.02, baseLatency: 0.01, destination: {},
                audioWorklet: { addModule: async () => { trip('addModule'); } },
                createMediaStreamSource: () => node({}),
                createGain: () => node({ gain: { value: 1 } }),
            };
            const mediaDevices = {
                getUserMedia: async () => {
                    trip('getUserMedia');
                    const track = { readyState: 'live', getSettings: () => ({}), stop() { this.readyState = 'ended'; } };
                    tracks.push(track);
                    return { getAudioTracks: () => [track], getTracks: () => [track] };
                },
            };
            const session = new MicrophoneSession({
                module: { PitchTracker: kept(module.PitchTracker, 'tracker'), PitchScorer: kept(module.PitchScorer, 'scorer') },
                reference, audioContext: ctx, playback: () => ({ playing: false, startCtxTime: 0 }), mediaDevices,
                onUpdate: () => trip('update'),
                createCaptureNode: () => { trip('captureNode'); return node({ port: {} }); },
            });

            let caught = null;
            try {
                await session.start();
            } catch (err) {
                caught = err;
            }
            const ended = () => tracks.every((t) => t.readyState === 'ended');
            const deleted = () => made.every((o) => o.isDeleted());
            const result = {
                threw: caught === null ? 'nothing' : (caught === thrown ? 'what the step threw' : String(caught.message)),
                on: session.on,
                tracks: tracks.length,
                tracksEnded: ended(),
                made: made.length,
                deleted: deleted(),
                nodes: nodes.length,
                disconnected: nodes.every((n) => n.disconnected),
                listening: nodes.some((n) => n.port && Boolean(n.port.onmessage)),
            };
            // The next press opens it, and closing that leaves nothing open either.
            fail = null;
            await session.start();
            result.opensAfter = session.on;
            result.tracksAfter = tracks.length;
            session.stop();
            result.closedAfter = !session.on && ended() && deleted();
            return result;
        };

        const out = {
            // The errors are the ones a browser gives at each step.
            rate: await run('rate', null),
            refused: await run('getUserMedia', new DOMException('Permission denied', 'NotAllowedError')),
            noWorklet: await run('addModule', new DOMException("Unable to load a worklet's module.", 'AbortError')),
            noNode: await run('captureNode', new DOMException('The node name is not defined in AudioWorkletGlobalScope.', 'InvalidStateError')),
            noScorer: await run('scorer', new Error('the scorer could not be made')),
            pageThrew: await run('update', new Error('the page could not draw it')),
        };
        reference.delete();
        return out;
    });

    // A rate the tracker cannot follow: the page is given a sentence, and the tracker that said so
    // is deleted. No microphone was asked for.
    expect(out.rate.threw).toContain('which the pitch tracker cannot follow');
    expect(out.rate).toMatchObject({ on: false, tracks: 0, made: 1, deleted: true, nodes: 0 });

    // The person refuses the microphone (the common one): the page is given the browser's own error,
    // by which it picks its sentence, and the tracker made for the session is deleted.
    expect(out.refused.threw).toBe('what the step threw');
    expect(out.refused).toMatchObject({ on: false, tracks: 0, made: 1, deleted: true, nodes: 0 });

    // The microphone opened and the worklet's module would not load (a page that may not load one
    // from a Blob): the track is ended, so the microphone is not left open with nothing to close it.
    expect(out.noWorklet.threw).toBe('what the step threw');
    expect(out.noWorklet).toMatchObject({ on: false, tracks: 1, tracksEnded: true, made: 1, deleted: true, nodes: 0 });

    // The capture node could not be made: the same, and the source made from the stream is let go.
    expect(out.noNode.threw).toBe('what the step threw');
    expect(out.noNode).toMatchObject({ on: false, tracks: 1, tracksEnded: true, made: 1, deleted: true,
                                       nodes: 1, disconnected: true });

    // The scorer could not be made, with the graph already connected: every node is disconnected.
    expect(out.noScorer.threw).toBe('what the step threw');
    expect(out.noScorer).toMatchObject({ on: false, tracks: 1, tracksEnded: true, made: 1, deleted: true,
                                         nodes: 3, disconnected: true, listening: false });

    // Everything was open and the page's own callback threw: start() failed, so it is closed, the
    // scorer with the rest.
    expect(out.pageThrew.threw).toBe('what the step threw');
    expect(out.pageThrew).toMatchObject({ on: false, tracks: 1, tracksEnded: true, made: 2, deleted: true,
                                          nodes: 3, disconnected: true, listening: false });

    // After any of them the next press opens the microphone (one more track), and closing it ends
    // every track and deletes every object made along the way.
    for (const [name, result] of Object.entries(out)) {
        expect(result.opensAfter, name).toBe(true);
        expect(result.tracksAfter, name).toBe(result.tracks + 1);
        expect(result.closedAfter, name).toBe(true);
    }
});

test('a session closed while it is still opening lets go of it and never opens', async ({ page }) => {
    await page.goto('index.html');
    const out = await page.evaluate(async () => {
        // @ts-ignore
        const module = await window.createIclForgeModule();
        const { MicrophoneSession, readReference } = await import('./mic.js');
        const rate = 48000;
        const tune = new Float32Array(rate);
        for (let i = 0; i < tune.length; i++) {
            tune[i] = 0.4 * Math.sin((2 * Math.PI * 220 * i) / rate);
        }
        const tick = () => new Promise((resolve) => setTimeout(resolve, 0));

        // A session over stand-ins whose two waits are the test's to end: the browser asking the
        // person for the microphone (`asks`), which a person can leave unanswered for as long as
        // they like, and the worklet's module loading (`loads`), by when the microphone is open.
        // Each wait is an entry in its list from when start() gets there until it is answered.
        const make = (reference) => {
            // The module's own tracker and scorer, each kept so that it can be asked whether it
            // was deleted.
            const made = [];
            const kept = (Type) => function (...args) {
                const object = new Type(...args);
                made.push(object);
                return object;
            };
            const tracks = [];
            const nodes = [];
            const asks = [];
            const loads = [];
            const held = (list) => new Promise((resolve, reject) => list.push({ resolve, reject }));
            const node = (more) => {
                const one = { connect() {}, disconnect() {}, ...more };
                nodes.push(one);
                return one;
            };
            const ctx = {
                sampleRate: rate, outputLatency: 0.02, baseLatency: 0.01, destination: {},
                audioWorklet: { addModule: () => held(loads) },
                createMediaStreamSource: () => node({}),
                createGain: () => node({ gain: { value: 1 } }),
            };
            const mediaDevices = {
                getUserMedia: async () => {
                    await held(asks);
                    const track = { readyState: 'live', getSettings: () => ({}), stop() { this.readyState = 'ended'; } };
                    tracks.push(track);
                    return { getAudioTracks: () => [track], getTracks: () => [track] };
                },
            };
            let updates = 0;
            const session = new MicrophoneSession({
                module: { PitchTracker: kept(module.PitchTracker), PitchScorer: kept(module.PitchScorer) },
                reference, audioContext: ctx, playback: () => ({ playing: false, startCtxTime: 0 }), mediaDevices,
                onUpdate: () => { updates += 1; },
                createCaptureNode: () => node({ port: {} }),
            });
            // What there is to see: whether it is on, the microphones the browser has handed over
            // and how many of them are still open, the module's objects made and how many of them
            // are still held, the nodes made, and how often the page was told to draw.
            const look = () => ({
                on: session.on,
                tracks: tracks.length,
                open: tracks.filter((t) => t.readyState !== 'ended').length,
                made: made.length,
                held: made.filter((o) => !o.isDeleted()).length,
                nodes: nodes.length,
                updates,
            });
            // Answers every wait there is, and every one that follows, until `promise` settles.
            const through = async (promise) => {
                let settled = false;
                const mark = () => { settled = true; };
                promise.then(mark, mark);
                while (!settled) {
                    for (const list of [asks, loads]) {
                        while (list.length > 0) {
                            list.shift().resolve();
                        }
                    }
                    await tick();
                }
            };
            return { session, asks, loads, look, through };
        };

        // stop() comes while start() waits at `step`, and then the wait ends as `answer` says.
        const run = async (step, answer, { forget = false } = {}) => {
            const reference = await readReference(module, tune, rate, {});
            const s = make(reference);
            let threw = 'nothing';
            const started = s.session.start().catch((err) => { threw = String((err && err.name) || err); });
            if (step === 'loading') {
                s.asks.shift().resolve();
                await tick();
            }
            const waiting = step === 'asking' ? s.asks : s.loads;
            const result = { waits: waiting.length, before: s.look() };
            s.session.stop();
            result.atOnce = s.look();
            // What the page does next for a stream that had a tune: the contour the session was
            // given is deleted, with the session still on its way.
            if (forget) {
                reference.delete();
            }
            const wait = waiting.shift();
            if (answer === 'yes') {
                wait?.resolve();
            } else {
                wait?.reject(answer);
            }
            await s.through(started);
            result.threw = threw;
            result.after = s.look();
            if (!forget) {
                // The next press opens it, and closing that leaves nothing open either.
                await s.through(s.session.start());
                result.opened = s.look();
                s.session.stop();
                result.closed = s.look();
                reference.delete();
            }
            return result;
        };

        // Closed while the browser was asking and started again before it has answered: the
        // second asks for itself, and the microphone the first is handed is closed.
        const startedAgain = async () => {
            const reference = await readReference(module, tune, rate, {});
            const s = make(reference);
            const first = s.session.start();
            s.session.stop();
            const second = s.session.start();
            const result = { asked: s.asks.length };
            s.asks.shift().resolve();
            await tick();
            result.firstAnswered = s.look();
            await s.through(Promise.allSettled([first, second]));
            result.opened = s.look();
            s.session.stop();
            result.closed = s.look();
            reference.delete();
            return result;
        };

        // Started twice with the browser still asking: there is one microphone to open.
        const startedTwice = async () => {
            const reference = await readReference(module, tune, rate, {});
            const s = make(reference);
            const first = s.session.start();
            const second = s.session.start();
            const result = { asked: s.asks.length };
            await s.through(Promise.allSettled([first, second]));
            result.opened = s.look();
            s.session.stop();
            result.closed = s.look();
            reference.delete();
            return result;
        };

        return {
            allowed: await run('asking', 'yes'),
            refused: await run('asking', new DOMException('Permission denied', 'NotAllowedError')),
            tuneGone: await run('asking', 'yes', { forget: true }),
            loading: await run('loading', 'yes'),
            noWorklet: await run('loading', new DOMException("Unable to load a worklet's module.", 'AbortError')),
            startedAgain: await startedAgain(),
            startedTwice: await startedTwice(),
        };
    });

    const nothing = { on: false, open: 0, held: 0, nodes: 0, updates: 0 };

    // Closed while the browser was asking, and then the person allows the microphone: the tracker
    // made for the press is deleted at once, the microphone the browser hands over is closed, and
    // the session is not on. Nothing is thrown: the page asked for this.
    expect(out.allowed.waits).toBe(1);
    expect(out.allowed.before).toMatchObject({ on: false, tracks: 0, made: 1, held: 1 });
    expect(out.allowed.atOnce).toMatchObject({ ...nothing, tracks: 0, made: 1 });
    expect(out.allowed.threw).toBe('nothing');
    expect(out.allowed.after).toMatchObject({ ...nothing, tracks: 1, made: 1 });

    // The person refuses instead: there is no microphone, and still nothing is thrown, because
    // the refusal is of something the page no longer wants.
    expect(out.refused.threw).toBe('nothing');
    expect(out.refused.after).toMatchObject({ ...nothing, tracks: 0, made: 1 });

    // The page deleted the tune while the browser was asking, as it does for the next stream: no
    // scorer is made from a contour that is gone.
    expect(out.tuneGone.threw).toBe('nothing');
    expect(out.tuneGone.after).toMatchObject({ ...nothing, tracks: 1, made: 1 });

    // Closed with the microphone open and the worklet's module still loading: the microphone is
    // closed at once, without waiting for the module, and when the module arrives nothing is made.
    expect(out.loading.waits).toBe(1);
    expect(out.loading.before).toMatchObject({ on: false, tracks: 1, open: 1, made: 1, held: 1 });
    expect(out.loading.atOnce).toMatchObject({ ...nothing, tracks: 1, made: 1 });
    expect(out.loading.threw).toBe('nothing');
    expect(out.loading.after).toMatchObject({ ...nothing, tracks: 1, made: 1 });
    // And the same when the module does not load.
    expect(out.noWorklet.atOnce).toMatchObject({ ...nothing, tracks: 1, made: 1 });
    expect(out.noWorklet.threw).toBe('nothing');
    expect(out.noWorklet.after).toMatchObject({ ...nothing, tracks: 1, made: 1 });

    // After any of them the next press opens the microphone (one more track, a tracker and a
    // scorer, the three nodes, a word to the page), and closing it leaves nothing open or held.
    for (const [name, result] of Object.entries({ allowed: out.allowed, refused: out.refused, loading: out.loading,
                                                  noWorklet: out.noWorklet })) {
        expect(result.opened, name).toEqual({ on: true, tracks: result.after.tracks + 1, open: 1, made: 3, held: 2,
                                              nodes: 3, updates: 1 });
        expect(result.closed, name).toMatchObject({ on: false, open: 0, held: 0, updates: 2 });
    }

    // Started again before the browser has answered the first: asked twice, the first answer's
    // microphone closed, and the second's the one that is open.
    expect(out.startedAgain.asked).toBe(2);
    expect(out.startedAgain.firstAnswered).toMatchObject({ on: false, tracks: 1, open: 0, made: 2, held: 1 });
    expect(out.startedAgain.opened).toEqual({ on: true, tracks: 2, open: 1, made: 3, held: 2, nodes: 3, updates: 1 });
    expect(out.startedAgain.closed).toMatchObject({ on: false, open: 0, held: 0 });

    // Started twice: asked once, one microphone, one tracker and one scorer.
    expect(out.startedTwice.asked).toBe(1);
    expect(out.startedTwice.opened).toEqual({ on: true, tracks: 1, open: 1, made: 2, held: 2, nodes: 3, updates: 1 });
    expect(out.startedTwice.closed).toMatchObject({ on: false, open: 0, held: 0 });
});

test('a line that was not sung is marked as such under the words, with the headphones answer', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));
    await page.goto('index.html');
    await page.locator('#fileInput').setInputFiles(AULD_LANG_SYNE);
    await expect(page.locator('#micStatus')).toContainText('Scored against the melody guide (M).', { timeout: 60000 });
    await page.locator('#lyricsInput').setInputFiles(AULD_LANG_SYNE_WORDS);
    await expect(page.locator('#lyricsStatus')).toContainText('timed by word');

    await page.locator('#micHeadphones').check();
    await page.locator('#micToggle').click();
    await expect(page.locator('#micIndicator')).toBeVisible({ timeout: 20000 });

    // Start the song at 5.6 s: the first line (1 to 5 s) went by with nobody singing it. The line
    // being sung is the second, and the one before it is marked.
    await page.evaluate(() => {
        const seek = /** @type {HTMLInputElement} */ (document.getElementById('seek'));
        seek.value = String((5.6 / 16.5) * 1000);
        seek.dispatchEvent(new Event('input', { bubbles: true }));
        seek.dispatchEvent(new Event('change', { bubbles: true }));
    });
    await page.locator('#playPause').click();
    const mark = page.locator('#lyricsView .lyric.previous .mark');
    await expect(mark).toHaveText('not sung', { timeout: 15000 });
    await expect(mark).toHaveClass(/not-sung/);

    // Without the headphones answer there is no mark at all.
    await page.locator('#micHeadphones').uncheck();
    await expect(page.locator('#lyricsView .lyric .mark')).toHaveCount(0, { timeout: 5000 });

    await page.locator('#micToggle').click();
    await expect(page.locator('#micIndicator')).toBeHidden();
    expect(pageErrors).toEqual([]);
});

test('the card keeps its promises: off until pressed, score withheld, nothing sent, tracks stopped', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));
    // Anything the page sends with a body is a request that is not a plain GET of its own files.
    const sent = [];
    page.on('request', (request) => {
        if (request.method() !== 'GET') {
            sent.push(`${request.method()} ${request.url()}`);
        }
    });

    // The fake device's tracks and the calls made for them are recorded from before the page's own
    // script runs, so that "nothing is open until it is pressed" is something that is seen.
    await page.addInitScript(() => {
        const w = /** @type {any} */ (window);
        w.__micCalls = 0;
        w.__micTracks = [];
        const original = navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);
        navigator.mediaDevices.getUserMedia = async (constraints) => {
            w.__micCalls += 1;
            w.__micConstraints = constraints;
            const stream = await original(constraints);
            w.__micTracks.push(...stream.getAudioTracks());
            return stream;
        };
    });

    await page.goto('index.html');
    // Off, and no card for a stream that is not karaoke.
    await expect(page.locator('#micPanel')).toBeHidden();
    // The bundled stream is not karaoke: still no card after it is decoded.
    await page.locator('#loadDemo').click();
    await expect(page.locator('#status')).toContainText('Decoded', { timeout: 20000 });
    await expect(page.locator('#micPanel')).toBeHidden();

    // A karaoke stream: the card appears, reads the melody, and then offers the microphone.
    await page.locator('#fileInput').setInputFiles(AULD_LANG_SYNE);
    await expect(page.locator('#micPanel')).toBeVisible({ timeout: 20000 });
    await expect(page.locator('#micStatus')).toContainText('Scored against the melody guide (M).', { timeout: 60000 });
    await expect(page.locator('#micToggle')).toBeEnabled();
    // Nothing is open: no indicator, and the page has not asked the browser for a microphone.
    await expect(page.locator('#micIndicator')).toBeHidden();
    expect(await page.evaluate(() => (/** @type {any} */ (window)).__micCalls)).toBe(0);

    // The meter is watched from before the press, in the page, and the widest it has been is kept.
    // It is back at 0% between two beeps of the fake device, so what it reads at the moment of a
    // look says nothing: expect.poll settles at a look a second, which is two beeps exactly, and
    // every look can then fall in the same silence.
    await page.evaluate(() => {
        const w = /** @type {any} */ (window);
        const fill = /** @type {HTMLElement} */ (document.getElementById('micMeterFill'));
        w.__micMeterPeak = 0;
        new MutationObserver(() => {
            w.__micMeterPeak = Math.max(w.__micMeterPeak, parseFloat(fill.style.width) || 0);
        }).observe(fill, { attributes: true, attributeFilter: ['style'] });
    });

    // Press it: on, with the headphones answer not given.
    await page.locator('#micToggle').click();
    await expect(page.locator('#micIndicator')).toBeVisible({ timeout: 20000 });
    await expect(page.locator('#micToggle')).toHaveText('Turn the microphone off');
    // The browser's own echo cancellation is asked for, and the voice is not shaped.
    expect(await page.evaluate(() => (/** @type {any} */ (window)).__micCalls)).toBe(1);
    const constraints = await page.evaluate(() => (/** @type {any} */ (window)).__micConstraints);
    expect(constraints.audio.echoCancellation).toBe(true);
    expect(constraints.audio.noiseSuppression).toBe(false);
    expect(constraints.audio.autoGainControl).toBe(false);
    // Sound reaches the meter: it has risen since the press (the first beep is half a second in).
    await expect.poll(async () => page.evaluate(() => (/** @type {any} */ (window)).__micMeterPeak), { timeout: 15000 })
        .toBeGreaterThan(0);
    // And the score is withheld, in words.
    await expect(page.locator('#micScore')).toHaveText('Say you are wearing headphones to see a score.');

    // The answer given: the words change.
    await page.locator('#micHeadphones').check();
    await expect(page.locator('#micScore')).not.toHaveText('Say you are wearing headphones to see a score.');

    // Off: the indicator goes, and every track the page opened has ended.
    await page.locator('#micToggle').click();
    await expect(page.locator('#micIndicator')).toBeHidden();
    await expect(page.locator('#micToggle')).toHaveText('Turn the microphone on');
    const states = await page.evaluate(() => (/** @type {any} */ (window)).__micTracks.map((t) => t.readyState));
    expect(states.length).toBeGreaterThan(0);
    expect(states.every((s) => s === 'ended')).toBe(true);

    expect(sent, 'nothing is sent from the page').toEqual([]);
    expect(pageErrors).toEqual([]);
});

test('a microphone the card opened and could not follow is closed again', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));

    // The page as a site that embeds it may serve it: scripts from its own origin and no other,
    // which leaves out the Blob the capture worklet is loaded from. So the microphone opens (the
    // fake device, with the prompt granted) and the step after it fails.
    await page.route('**/index.html', async (route) => {
        const response = await route.fetch();
        await route.fulfill({
            response,
            headers: { ...response.headers(), 'content-security-policy': "script-src 'self' 'unsafe-eval'" },
        });
    });
    // The tracks the browser hands the page, kept from before the page's own script runs.
    await page.addInitScript(() => {
        const w = /** @type {any} */ (window);
        w.__micTracks = [];
        const original = navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);
        navigator.mediaDevices.getUserMedia = async (constraints) => {
            const stream = await original(constraints);
            w.__micTracks.push(...stream.getAudioTracks());
            return stream;
        };
    });

    await page.goto('index.html');
    await page.locator('#fileInput').setInputFiles(AULD_LANG_SYNE);
    await expect(page.locator('#micStatus')).toContainText('Scored against the melody guide (M).', { timeout: 60000 });

    // Press it: the card says it did not work (the words are the browser's), and offers the button
    // again with nothing shown as on.
    await page.locator('#micToggle').click();
    await expect(page.locator('#micStatus')).toHaveClass(/error/, { timeout: 20000 });
    await expect(page.locator('#micToggle')).toBeEnabled();
    await expect(page.locator('#micToggle')).toHaveText('Turn the microphone on');
    await expect(page.locator('#micIndicator')).toBeHidden();

    // The microphone did open, or this shows nothing; and what the card says is so: its track has
    // ended. A track left live here is a microphone that is on with no indicator and no button
    // that closes it.
    const states = await page.evaluate(() => (/** @type {any} */ (window)).__micTracks.map((t) => t.readyState));
    expect(states.length).toBeGreaterThan(0);
    expect(states).toEqual(states.map(() => 'ended'));

    expect(pageErrors).toEqual([]);
});

// What the two cases below put in the page before its own script runs: the microphone's tracks are
// kept as the browser hands them over, as above, and each of the two things a press waits on the
// browser for can be held open. `__hold.asking` holds the prompt, which in a browser stays for as
// long as the person leaves it, and nothing is opened before it is answered; `__hold.starting`
// holds the sound output starting, which a press waits for before it asks. `__letGo()` ends what
// is being held.
function holdWhatAPressWaitsFor() {
    const w = /** @type {any} */ (window);
    w.__micAsked = 0;
    w.__micTracks = [];
    w.__outputStarted = 0;
    w.__hold = { asking: false, starting: false };
    const waits = [];
    w.__letGo = () => waits.splice(0).forEach((end) => end());
    const held = () => new Promise((resolve) => waits.push(() => resolve(undefined)));
    const ask = navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);
    navigator.mediaDevices.getUserMedia = async (constraints) => {
        w.__micAsked += 1;
        if (w.__hold.asking) {
            await held();
        }
        const stream = await ask(constraints);
        w.__micTracks.push(...stream.getAudioTracks());
        return stream;
    };
    const resume = AudioContext.prototype.resume;
    AudioContext.prototype.resume = async function () {
        if (w.__hold.starting) {
            await held();
        }
        await resume.call(this);
        w.__outputStarted += 1;
    };
}

const NO_TUNE = 'This stream has no melody or vocal channel to score against; the microphone shows your pitch only.';

test('a stream loaded while the browser is still asking leaves the microphone closed', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));
    await page.addInitScript(holdWhatAPressWaitsFor);
    const asked = () => page.evaluate(() => (/** @type {any} */ (window)).__micAsked);
    const tracks = () => page.evaluate(() => (/** @type {any} */ (window)).__micTracks.map((t) => t.readyState));
    const hold = (asking) => page.evaluate((on) => { (/** @type {any} */ (window)).__hold.asking = on; }, asking);
    const noTune = { name: 'karaoke-two-channel.ac3', mimeType: 'audio/ac3', buffer: karaokeWithNoTune() };

    await page.goto('index.html');
    // A karaoke stream with no tune: the card offers the microphone for the pitch alone.
    await page.locator('#fileInput').setInputFiles(noTune);
    await expect(page.locator('#micStatus')).toHaveText(NO_TUNE, { timeout: 20000 });
    await expect(page.locator('#micToggle')).toBeEnabled();

    // Press it, and leave the browser's question unanswered.
    await hold(true);
    await page.locator('#micToggle').click();
    await expect.poll(asked).toBe(1);
    // Meanwhile another stream is loaded, and it is not karaoke: the card goes.
    await page.locator('#loadDemo').click();
    await expect(page.locator('#status')).toContainText('Decoded the bundled demo stream', { timeout: 20000 });
    await expect(page.locator('#micPanel')).toBeHidden();
    // Now the person allows the microphone. The browser hands it over, and the page closes it: a
    // track left live here is a microphone that is on behind a card that is not there, with no
    // indicator to say so and no button to close it.
    await page.evaluate(() => (/** @type {any} */ (window)).__letGo());
    await expect.poll(tracks, { timeout: 10000 }).toEqual(['ended']);
    // And the card that is not shown does not say it is on.
    await expect(page.locator('#micIndicator')).toHaveCSS('display', 'none');
    expect(await asked()).toBe(1);

    // The same with a card that stays: a stream with a tune, and while the browser asks, a
    // karaoke stream loaded over it. The tune the press began with is deleted for the new stream.
    await page.locator('#fileInput').setInputFiles(AULD_LANG_SYNE);
    await expect(page.locator('#micStatus')).toContainText('Scored against the melody guide (M).', { timeout: 60000 });
    await expect(page.locator('#micToggle')).toBeEnabled();
    await page.locator('#micToggle').click();
    await expect.poll(asked).toBe(2);
    await page.locator('#fileInput').setInputFiles(noTune);
    await expect(page.locator('#micStatus')).toHaveText(NO_TUNE, { timeout: 20000 });
    await page.evaluate(() => (/** @type {any} */ (window)).__letGo());
    await expect.poll(tracks, { timeout: 10000 }).toEqual(['ended', 'ended']);
    // The card still says what it said for the new stream. Nothing failed (the person loaded a
    // stream), so there is no error to show. It offers the microphone, which is off.
    await expect(page.locator('#micStatus')).toHaveText(NO_TUNE);
    await expect(page.locator('#micStatus')).not.toHaveClass(/error/);
    await expect(page.locator('#micToggle')).toBeEnabled();
    await expect(page.locator('#micToggle')).toHaveText('Turn the microphone on');
    await expect(page.locator('#micIndicator')).toBeHidden();

    // The next press, answered, opens it as any press does, and the one after closes it.
    await hold(false);
    await page.locator('#micToggle').click();
    await expect(page.locator('#micIndicator')).toBeVisible({ timeout: 20000 });
    await expect(page.locator('#micStatus')).toHaveText('Showing your pitch.');
    expect(await tracks()).toEqual(['ended', 'ended', 'live']);
    await page.locator('#micToggle').click();
    await expect(page.locator('#micIndicator')).toBeHidden();
    expect(await tracks()).toEqual(['ended', 'ended', 'ended']);

    expect(pageErrors).toEqual([]);
});

test('a press that a new stream overtakes before the browser is asked asks for nothing', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));
    await page.addInitScript(holdWhatAPressWaitsFor);
    const seen = () => page.evaluate(() => {
        const w = /** @type {any} */ (window);
        return { asked: w.__micAsked, tracks: w.__micTracks.map((t) => t.readyState) };
    });

    await page.goto('index.html');
    await page.locator('#fileInput').setInputFiles(
        { name: 'karaoke-two-channel.ac3', mimeType: 'audio/ac3', buffer: karaokeWithNoTune() });
    await expect(page.locator('#micStatus')).toHaveText(NO_TUNE, { timeout: 20000 });
    await expect(page.locator('#micToggle')).toBeEnabled();

    // A press starts the sound output before it asks for the microphone, so that the voice and
    // what it sings along to share a clock. Here the output is slow to start, and in that time a
    // stream that is not karaoke is loaded: the card goes.
    await page.evaluate(() => { (/** @type {any} */ (window)).__hold.starting = true; });
    await page.locator('#micToggle').click();
    await expect(page.locator('#micToggle')).toBeDisabled();
    await page.locator('#loadDemo').click();
    await expect(page.locator('#status')).toContainText('Decoded the bundled demo stream', { timeout: 20000 });
    await expect(page.locator('#micPanel')).toBeHidden();
    expect(await seen()).toEqual({ asked: 0, tracks: [] });

    // The output starts. The press was for a card that has gone: the browser is not asked for a
    // microphone at all. (The press goes on from the output's start within the same turn of the
    // page, so the look after it sees whatever the press did.)
    await page.evaluate(() => (/** @type {any} */ (window)).__letGo());
    await expect.poll(() => page.evaluate(() => (/** @type {any} */ (window)).__outputStarted)).toBe(1);
    expect(await seen()).toEqual({ asked: 0, tracks: [] });
    await expect(page.locator('#micIndicator')).toHaveCSS('display', 'none');

    expect(pageErrors).toEqual([]);
});
