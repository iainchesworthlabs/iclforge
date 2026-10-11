// @ts-check
'use strict';

const fs = require('fs');
const path = require('path');
const { test, expect } = require('@playwright/test');

// Closes WASM/mobile headless coverage(a): docs/platforms/wasm.md records channel count,
// sample rate, object count and "an object's position differs between two
// timestamps" as manual-only verification, because build-wasm only proves
// the module compiles. WASM streaming decoder package replaced the demo's own bespoke Embind
// Decoder class with the published iclforge-wasm-decoder package
// (bindings/js/src/decode-file.ts's decodeFile(), built on PushDecoder) - this test
// now drives THAT, the same call apps/demos/wasm/demo.js itself makes, against the
// bundled 8-second, 3-object Atmos-in-DD+ fixture, and asserts the same real
// values a human previously checked by eye.
test('decodes the bundled Atmos-in-DD+ fixture with real, moving object positions', async ({
    page,
}) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));

    // Relative on purpose: baseURL includes the demo's subdirectory (see
    // playwright.config.js), and a leading "/" would resolve back to the
    // server root.
    await page.goto('index.html');

    const result = await page.evaluate(async () => {
        // @ts-ignore - ./package/ is bindings/js/dist/, copied in alongside the
        // Emscripten build output (see this repo's build-wasm CI job).
        const { decodeFile, DownmixTarget } = await import('./package/index.js');
        // @ts-ignore - createIclForgeModule is the Emscripten MODULARIZE
        // factory iclforge_decode.js attaches to window; see index.html's
        // plain <script> tag.
        const moduleInstance = await window.createIclForgeModule();
        const response = await fetch('assets/demo.ec3');
        const bytes = new Uint8Array(await response.arrayBuffer());

        let program;
        try {
            program = decodeFile(moduleInstance, bytes, {
                fold: { target: DownmixTarget.LoRo, applyDialnorm: true },
            });
        } catch (error) {
            return { ok: false, error: String(error && error.message) };
        }

        // [x, y, z, gain_db, width, depth, height] - decode-file.ts's own
        // fixed stride - copied out with Array.from immediately since
        // Float32Array views into decoder state are only valid until the
        // next push()/flush() call (bindings/js/src/push-decoder.ts's own contract);
        // decodeFile() has already finished, but its returned arrays are
        // plain copies either way.
        const stride = 7;
        const positions = Array.from(program.objectPositions[0] ?? []);
        const firstFrame = positions.slice(0, stride);
        const lastFrame = positions.slice(positions.length - stride, positions.length);

        return {
            ok: true,
            sampleRate: program.sampleRate,
            channelCount: program.channelCount,
            objectCount: program.objectCount,
            durationSeconds: program.durationSeconds,
            foldChannelCount: program.fold.length,
            stride,
            firstFrame,
            lastFrame,
        };
    });

    expect(result.ok, `decode failed: ${result.error}`).toBe(true);
    // docs/platforms/wasm.md: "E-AC-3, 48000 Hz, 6 ch (L, C, R, Ls, Rs,
    // LFE), 3 Atmos object(s), 8.0s".
    expect(result.sampleRate).toBe(48000);
    expect(result.channelCount).toBe(6);
    expect(result.objectCount).toBe(3);
    expect(result.durationSeconds).toBeCloseTo(8.0, 1);
    // The real iclforge::ac3::OutputStage Lo/Ro fold this test asked for.
    expect(result.foldChannelCount).toBe(2);

    // x, y, z are the first three values of each stride-wide frame. A real
    // decode of moving object metadata puts the first and last frame at
    // different positions - a decoder that silently returned a
    // static placement (or garbage) would not clear this bar reliably.
    const [x0, y0, z0] = result.firstFrame;
    const [x1, y1, z1] = result.lastFrame;
    const moved = Math.hypot(x1 - x0, y1 - y0, z1 - z0);
    expect(moved).toBeGreaterThan(0.01);

    expect(pageErrors).toEqual([]);
});

// The karaoke example (examples/karaoke/auld-lang-syne/): a 3/2 AC-3 stream flagged bsmod 7, which
// the library's output stage reproduces (Annex C of ETSI TS 102 366). The same stream that
// iclforge-forge-cli-tests probes as karaoke and `forge decode karaoke` reproduces, so what the
// page does here is what the command line writes: a native decode of each of these is held against
// this module's by hand (planning/karaoke-phase2.md, W8), and a mismatch would show as a frame
// that is not the library's.
const KARAOKE_EXAMPLE = path.resolve(
    __dirname, '../../../../examples/karaoke/auld-lang-syne/auld-lang-syne.ac3');

test('a karaoke AC-3 stream is said to be one, and which vocals are heard is a choice', async ({
    page,
}) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));
    await page.goto('index.html');

    const bytes = Array.from(fs.readFileSync(KARAOKE_EXAMPLE));
    const result = await page.evaluate(async (data) => {
        // @ts-ignore - see the first test's own comment on ./package/.
        const { decodeFile, DownmixTarget, KaraokeReproduction, KaraokeVocals } =
            await import('./package/index.js');
        // @ts-ignore
        const moduleInstance = await window.createIclForgeModule();
        const input = Uint8Array.from(data);
        const decode = (fold) => decodeFile(moduleInstance, input, { fold });
        const energy = (program, channel) => {
            const pcm = program.fold[channel];
            let sum = 0;
            for (const sample of pcm) sum += sample * sample;
            return sum / pcm.length;
        };
        const lo = DownmixTarget.LoRo;
        const capable = KaraokeReproduction.Capable;
        const coded = decode({ target: DownmixTarget.AsCoded });
        const aware = decode({ target: DownmixTarget.AsCoded, karaoke: KaraokeReproduction.Aware });
        const stereo = decode({ target: lo });
        const mix = (vocals) => decode({ target: lo, karaoke: capable, karaokeVocals: vocals });
        const choice = (vocals) => energy(mix(vocals), 0);
        // How far apart two mixes are: the mean square of their left channels' difference, as a share
        // of the louder one's energy. Their energies alone can agree by coincidence: the reproduction is
        // scaled so that no output sums past 1, and with the vocals out it is scaled less, so a mix with
        // the lead vocal in and one without can be the same loudness and not the same mix.
        const apart = (a, b) => {
            const x = a.fold[0];
            const y = b.fold[0];
            const n = Math.min(x.length, y.length);
            let sum = 0;
            for (let i = 0; i < n; i++) {
                const d = x[i] - y[i];
                sum += d * d;
            }
            return sum / n / Math.max(energy(a, 0), energy(b, 0));
        };
        return {
            karaoke: coded.karaoke,
            codedFold: coded.fold.length,
            codedChannels: coded.channelCount,
            awareFold: aware.fold.length,
            stereoFold: stereo.fold.length,
            stereo: energy(stereo, 0),
            v1: choice(KaraokeVocals.V1),
            v2: choice(KaraokeVocals.V2),
            noneFromBoth: apart(mix(KaraokeVocals.None), mix(KaraokeVocals.Both)),
        };
    }, bytes);

    expect(result.karaoke).toBe(true);
    // No fold asked for and no reproduction: the coded channels only, which are five.
    expect(result.codedFold).toBe(0);
    expect(result.codedChannels).toBe(5);
    // The reproduction at the coded layout is L, C and R; the stereo fold is two.
    expect(result.awareFold).toBe(3);
    expect(result.stereoFold).toBe(2);
    // Each choice of vocals is a different mix, none of them the stream's own.
    const differs = (a, b) => Math.abs(a - b) > 0.01 * Math.max(a, b);
    expect(differs(result.v1, result.v2)).toBe(true);
    expect(differs(result.v1, result.stereo)).toBe(true);
    // With none of the vocals and with both of them the left channel is not the same mix, though its
    // energy is within a percent of it.
    expect(result.noneFromBoth).toBeGreaterThan(0.1);
    expect(pageErrors).toEqual([]);
});

test('the page offers the vocals of a karaoke stream and of no other, and decodes again on a choice', async ({
    page,
}) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));
    await page.goto('index.html');

    // The bundled Atmos-in-DD+ stream is not karaoke: no control.
    await page.click('#loadDemo');
    await expect(page.locator('#status')).toContainText('Decoded the bundled demo stream');
    await expect(page.locator('#karaokePanel')).toBeHidden();

    await page.setInputFiles('#fileInput', KARAOKE_EXAMPLE);
    await expect(page.locator('#status')).toContainText('Decoded auld-lang-syne.ac3');
    await expect(page.locator('#streamInfo')).toContainText('AC-3');
    await expect(page.locator('#karaokePanel')).toBeVisible();

    // A choice decodes the stream again with it.
    await page.selectOption('#karaokeChoice', 'v1');
    await expect(page.locator('#status')).toContainText('Decoded auld-lang-syne.ac3');
    await expect(page.locator('#karaokePanel')).toBeVisible();
    await expect(page.locator('#karaokeChoice')).toHaveValue('v1');

    // And a stream that is not karaoke takes it away again.
    await page.click('#loadDemo');
    await expect(page.locator('#status')).toContainText('Decoded the bundled demo stream');
    await expect(page.locator('#karaokePanel')).toBeHidden();
    expect(pageErrors).toEqual([]);
});
