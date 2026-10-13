// @ts-check
'use strict';

const path = require('path');
const { test, expect } = require('@playwright/test');

// The decode demo's microphone (planning/karaoke-microphone.md, M7; D16): the library's pitch
// tracker, reference melody and scorer compiled into the demo's module (apps/demos/wasm/
// pitch_bindings.cpp), and the card that follows a microphone with them (mic.js).
//
// Two things are held. The arithmetic: the module's tracker finds a sung pitch, a contour is read
// from a channel in slices, and the scorer counts what is on the tune with octaves folded. And what
// the page promises: the microphone is off until the button is pressed, the score is withheld
// until the person says they wear headphones, nothing is sent anywhere, and closing it stops the
// tracks. The device is Chromium's fake one (a tone, with the permission prompt granted by the
// flags in playwright.config.js): no real microphone is opened by this suite.

const AULD_LANG_SYNE = path.join(__dirname, '../../../../examples/karaoke/auld-lang-syne/auld-lang-syne.ac3');
const AULD_LANG_SYNE_WORDS = path.join(__dirname, '../../../../examples/karaoke/auld-lang-syne/auld-lang-syne.lrc');

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
    // Sound reaches the meter (the fake device is a tone).
    await expect.poll(async () => page.locator('#micMeterFill').evaluate((e) => e.style.width), { timeout: 15000 })
        .not.toBe('0%');
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
