// @ts-check
'use strict';

// The Speakers section: a level and a distance for each speaker the layout in force has at an output, and a test
// tone out of each (planning/p4x-preferences.md), driven through the page against the stand-in device. The routes
// are GET and PUT /trim and GET and POST /identify; what the firmware writes for them is held to here, key for key,
// against the headers that write it.

const fs = require('fs');
const path = require('path');
const { test, expect } = require('./fixtures');
const { soundBoard, startStub } = require('./stub');

const RENDER = path.resolve(__dirname, '../../../../../libs/render/include/iclforge/render');
const SPEAKER_TRIM = fs.readFileSync(path.join(RENDER, 'speaker_trim.hpp'), 'utf8');
const IDENTIFY_REQUEST = fs.readFileSync(path.join(RENDER, 'identify_request.hpp'), 'utf8');

const rows = (page) => page.locator('#trim-rows tr');
const field = (page, name) => page.getByLabel(name, { exact: true });
// A number typed into a field is sent when the field is left.
const set = async (locator, value) => {
    await locator.fill(value);
    await locator.blur();
};

test.describe('a board with a Sendspin player', () => {
    test.beforeEach(async ({ page, stub }) => {
        stub.device.sound = soundBoard();
        await page.goto(stub.url);
        await expect(page.locator('#sound')).toBeVisible();
    });

    test("lists the layout's speakers by name, each with a level, a distance, a delay and a tone", async ({ page }) => {
        await expect(rows(page)).toHaveCount(6);
        await expect(rows(page).first().getByRole('rowheader')).toHaveText('Front left');
        await expect(rows(page).nth(5).getByRole('rowheader')).toHaveText('Subwoofer');
        await expect(field(page, 'Front left level')).toHaveValue('0');
        await expect(field(page, 'Front left distance')).toHaveValue('');
        await expect(rows(page).first()).toContainText('0.0 ms');
        await expect(page.getByRole('button', { name: 'Play a tone from the Front left' })).toBeVisible();
        // No note when nothing is wrong.
        await expect(page.locator('#sound-note')).toBeHidden();
    });

    test('a level is one statement to PUT /trim, and the row then shows what the board took', async ({ page, stub }) => {
        await set(field(page, 'Front left level'), '-1.5');
        await expect.poll(() => stub.sent('PUT /trim').map((r) => r.body)).toEqual(['L level=-1.5']);
        await expect(field(page, 'Front left level')).toHaveValue('-1.5');
        // An emptied field is 0 dB, not a missing number.
        await set(field(page, 'Front left level'), '');
        await expect.poll(() => stub.sent('PUT /trim').map((r) => r.body)).toEqual(['L level=-1.5', 'L level=0']);
        await expect(field(page, 'Front left level')).toHaveValue('0');
    });

    test("a level the board refuses shows the device's words", async ({ page, stub }) => {
        await set(field(page, 'Centre level'), '0.3');
        await expect(page.getByRole('status')).toHaveText(
            'Centre level refused (400): PUT /trim: a level is -12 to 12 dB, in steps of 0.5',
        );
        await expect(page.getByRole('status')).toHaveClass(/error/);
        expect(stub.device.sound.speakers[1].level).toBe(0);
    });

    test('a distance is sent in metres, the nearer speaker shows its delay, and an emptied field clears it', async ({ page, stub }) => {
        await set(field(page, 'Front left distance'), '2');
        await expect.poll(() => stub.sent('PUT /trim').map((r) => r.body)).toEqual(['L distance=2']);
        // One speaker with a distance has nothing to be nearer than.
        await expect(rows(page).first()).toContainText('0.0 ms');
        await set(field(page, 'Front right distance'), '2.5');
        await expect.poll(() => stub.sent('PUT /trim').map((r) => r.body)).toEqual(['L distance=2', 'R distance=2.5']);
        // 50 cm of sound is 70 samples at 48 kHz: 1.5 ms. The farthest speaker is not delayed.
        await expect(rows(page).first()).toContainText('1.5 ms');
        await expect(rows(page).nth(2)).toContainText('0.0 ms');
        await set(field(page, 'Front left distance'), '');
        await expect.poll(() => stub.sent('PUT /trim').map((r) => r.body).pop()).toBe('L distance=none');
        await expect(field(page, 'Front left distance')).toHaveValue('');
        await expect(rows(page).first()).toContainText('0.0 ms');
    });

    test('a span of distances the delay lines cannot hold is refused, and a smaller one is not', async ({ page, stub }) => {
        await set(field(page, 'Front left distance'), '1');
        await expect.poll(() => stub.sent('PUT /trim').length).toBe(1);
        await set(field(page, 'Front right distance'), '9');
        await expect(page.getByRole('status')).toHaveText(
            'Front right distance refused (409): the nearest and the farthest speaker can be at most 6.86 m apart on this board (its delay lines hold 20 ms)',
        );
        expect(stub.device.sound.speakers[2].distance).toBe(0);
        // Nearer is a smaller span, and is taken.
        await set(field(page, 'Front right distance'), '7');
        await expect.poll(() => stub.sent('PUT /trim').map((r) => r.body).pop()).toBe('R distance=7');
        expect(stub.device.sound.speakers[2].distance).toBe(7);
        // A table stored past the span (by a build with longer lines) still takes a level, and can be made smaller.
        stub.device.sound.speakers[2].distance = 9;
        await page.reload();
        await set(field(page, 'Front left level'), '1');
        await expect.poll(() => stub.sent('PUT /trim').map((r) => r.body).pop()).toBe('L level=1');
        await set(field(page, 'Front right distance'), '8');
        await expect.poll(() => stub.sent('PUT /trim').map((r) => r.body).pop()).toBe('R distance=8');
    });

    test('a distance past the range is refused in the device words', async ({ page }) => {
        await set(field(page, 'Front left distance'), '11');
        await expect(page.getByRole('status')).toHaveText(
            'Front left distance refused (400): PUT /trim: a distance is 0.1 to 10 metres, or none',
        );
    });

    test('Play sends the speaker to POST /identify; a refusal says why', async ({ page, stub }) => {
        await page.getByRole('button', { name: 'Play a tone from the Front left' }).click();
        await expect.poll(() => stub.sent('POST /identify').map((r) => r.body)).toEqual(['L']);
        expect(stub.device.sound.identify.active).toBe(true);
        expect(stub.device.sound.identify.role).toBe('L');
        // A stream is playing: the tone is not played over it.
        stub.device.sound.identify.refuse = 'playing';
        await page.getByRole('button', { name: 'Play a tone from the Subwoofer' }).click();
        await expect(page.getByRole('status')).toHaveText(
            'Tone from the Subwoofer refused (409): a stream is playing; the tone would play over it',
        );
        await expect(page.getByRole('status')).toHaveClass(/error/);
    });

    test('Play each speaker and Stop the tone send all and off', async ({ page, stub }) => {
        await page.getByRole('button', { name: 'Play each speaker' }).click();
        await expect(page.getByRole('status')).toHaveText('Each speaker in turn, quietly.');
        await page.getByRole('button', { name: 'Stop the tone' }).click();
        await expect.poll(() => stub.sent('POST /identify').map((r) => r.body)).toEqual(['all', 'off']);
        expect(stub.device.sound.identify.active).toBe(false);
        await expect(page.getByRole('status')).toHaveText('Stopped.');
    });

    test('Reset asks first, and clears every level and distance when told to', async ({ page, stub }) => {
        stub.device.sound.speakers[0].level = -3;
        stub.device.sound.speakers[2].distance = 3;
        await page.reload();
        await expect(field(page, 'Front left level')).toHaveValue('-3');
        const dialog = page.getByRole('dialog', { name: 'Reset every speaker?' });
        await page.getByRole('button', { name: 'Reset levels and distances' }).click();
        await expect(dialog).toBeVisible();
        await dialog.getByRole('button', { name: 'Cancel' }).click();
        await expect(dialog).toBeHidden();
        expect(stub.sent('PUT /trim')).toHaveLength(0);
        await page.getByRole('button', { name: 'Reset levels and distances' }).click();
        await dialog.getByRole('button', { name: 'Reset', exact: true }).click();
        await expect.poll(() => stub.sent('PUT /trim').map((r) => r.body)).toEqual(['reset']);
        await expect(field(page, 'Front left level')).toHaveValue('0');
        await expect(field(page, 'Front right distance')).toHaveValue('');
    });

    test('leaving the page ends a tone that is still going', async ({ page, stub }) => {
        await page.getByRole('button', { name: 'Play a tone from the Centre' }).click();
        await expect.poll(() => stub.sent('POST /identify').length).toBe(1);
        await page.goto('about:blank');
        await expect.poll(() => stub.sent('POST /identify').map((r) => r.body)).toEqual(['C', 'off']);
    });

    test("a change of layout shows the new layout's speakers at the next poll", async ({ page, stub }) => {
        await expect(rows(page)).toHaveCount(6);
        // The stand-in's layout is 2.0 to begin with: a different one is a change the page reads the speakers for.
        stub.device.layout = '5.1';
        stub.device.sound.layout = '5.1';
        stub.device.sound.speakers = stub.device.sound.speakers.slice(0, 1).concat(stub.device.sound.speakers.slice(2, 3));
        await expect(rows(page)).toHaveCount(2);
        await expect(rows(page).nth(1).getByRole('rowheader')).toHaveText('Front right');
    });
});

test.describe('what the build cannot do, and what a server has set', () => {
    test('a board that has no delay lines greys the distance with the reason, and still takes levels', async ({ page, stub }) => {
        stub.device.sound = soundBoard({
            delay: { available: false, reason: 'this board has no PSRAM to hold delay lines', maxSpan: 0, samples: 0 },
        });
        await page.goto(stub.url);
        await expect(page.locator('#sound-note')).toHaveText('Distance is not available: this board has no PSRAM to hold delay lines.');
        await expect(field(page, 'Front left distance')).toBeDisabled();
        await expect(field(page, 'Front left level')).toBeEnabled();
        await set(field(page, 'Front left level'), '2');
        await expect.poll(() => stub.sent('PUT /trim').map((r) => r.body)).toEqual(['L level=2']);
    });

    test('levels a server set are said to be what plays now', async ({ page, stub }) => {
        stub.device.sound = soundBoard({ serverLevel: true });
        await page.goto(stub.url);
        await expect(page.locator('#sound-note')).toHaveText('A server has set the levels that play now, in place of these.');
    });

    test('delays a server set are said too', async ({ page, stub }) => {
        stub.device.sound = soundBoard({ serverDelay: true });
        await page.goto(stub.url);
        await expect(page.locator('#sound-note')).toHaveText('A server has set the distances that play now, in place of these.');
    });

    test('a board with no player has no section, and a page that left no tone running sends nothing on leaving', async ({ page, stub }) => {
        stub.device.sound = null;
        await page.goto(stub.url);
        await expect(page.locator('#state')).toHaveText('Stopped');
        await expect(page.locator('#sound')).toBeHidden();
        expect(stub.sent('GET /trim')).toHaveLength(1);
        await page.goto('about:blank');
        expect(stub.sent('POST /identify')).toHaveLength(0);
    });

    test('a read that fails is tried again at the next poll', async ({ page, stub }) => {
        stub.device.sound = soundBoard();
        stub.next('GET /trim', { status: 500, body: 'no\n' });
        await page.goto(stub.url);
        await expect(page.locator('#sound')).toBeVisible();
        expect(stub.sent('GET /trim').length).toBeGreaterThanOrEqual(2);
    });
});

test.describe('the stand-in against the firmware', () => {
    // Every key the firmware writes, in the order it writes them: the quoted names in the function that writes the
    // JSON, which is straight-line code.
    const keys = (source, from, to) =>
        [...source.slice(source.indexOf(from), source.indexOf(to)).matchAll(/\\"([a-z_]+)\\":/g)].map((m) => m[1]);
    const flatten = (body, nested) =>
        Object.keys(body).flatMap((key) => {
            if (nested[key] && Array.isArray(body[key])) {
                return [key, ...Object.keys(body[key][0])];
            }
            return nested[key] ? [key, ...Object.keys(body[key])] : [key];
        });

    test("GET /trim has the keys trim_json writes, in its order", async () => {
        const firmware = keys(SPEAKER_TRIM, 'inline std::string trim_json', '}  // namespace speaker_trim');
        expect(firmware.length).toBeGreaterThan(20);
        const stub = await startStub();
        try {
            stub.device.sound = soundBoard();
            const body = JSON.parse(await (await fetch(`${stub.url}trim`)).text());
            expect(flatten(body, { level: true, delay: true, speakers: true })).toEqual(firmware);
        } finally {
            await stub.close();
        }
    });

    test('GET /identify has the keys identify_json writes, in its order', async () => {
        const firmware = keys(IDENTIFY_REQUEST, 'inline std::string identify_json', '}  // namespace iclforge::render');
        expect(firmware.length).toBeGreaterThan(8);
        const stub = await startStub();
        try {
            stub.device.sound = soundBoard();
            const body = JSON.parse(await (await fetch(`${stub.url}identify`)).text());
            expect(flatten(body, {})).toEqual(firmware);
        } finally {
            await stub.close();
        }
    });

    test('the routes answer 404 on a board with no player, and 400 for words they do not know', async () => {
        const stub = await startStub();
        try {
            stub.device.sound = null;
            expect((await fetch(`${stub.url}trim`)).status).toBe(404);
            expect((await fetch(`${stub.url}trim`, { method: 'PUT', body: 'L level=1' })).status).toBe(404);
            expect((await fetch(`${stub.url}identify`)).status).toBe(404);
            expect((await fetch(`${stub.url}identify`, { method: 'POST', body: 'L' })).status).toBe(404);
            stub.device.sound = soundBoard();
            for (const bad of ['', ' ; ', 'Left level=1', 'L', 'L volume=1', 'L level=abc', 'L level=13', 'L distance=0.05', 'L level=1; R level=99']) {
                const r = await fetch(`${stub.url}trim`, { method: 'PUT', body: bad });
                expect([bad, r.status]).toEqual([bad, 400]);
            }
            // All or nothing: the good statement before the bad one was not taken.
            expect(stub.device.sound.speakers[0].level).toBe(0);
            for (const bad of ['', 'on', 'left', 'L now', 'L loop loop', 'all loop']) {
                const r = await fetch(`${stub.url}identify`, { method: 'POST', body: bad });
                expect([bad, r.status]).toEqual([bad, 400]);
            }
            expect((await fetch(`${stub.url}identify`, { method: 'POST', body: 'Rrs' })).status).toBe(409);
            expect((await fetch(`${stub.url}identify`, { method: 'POST', body: 'FL loop' })).status).toBe(200);
            expect((await fetch(`${stub.url}trim`, { method: 'PUT', body: 'sub level=-2.5dB, distance=none; fr reset' })).status).toBe(200);
            expect(stub.device.sound.speakers[5].level).toBe(-2.5);
        } finally {
            await stub.close();
        }
    });
});
