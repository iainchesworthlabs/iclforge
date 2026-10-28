// @ts-check
'use strict';

// Every action the page offers, driven through the page against the stand-in
// device, and what the device was sent - planning/esp32-device-ui.md's "What
// the page does". The error paths are the device's own refusals (400, 409), a
// connection closed unanswered and a device that does not answer at all.

const { test, expect, holdClockAt } = require('./fixtures');
const { REPLIES } = require('./stub');

const DEMO = 'http://10.0.2.2:8000/demo.ec3';

// THE PAGE NO LONGER PLAYS ANYTHING. A server owns playback from B2 on
// (planning/hearth-reference-player.md): this page is what the BOARD is, and
// POST /play stays in the REST surface for a person with curl. So the plays
// these tests need are started through the device's own API rather than
// through the page, as hearth or Music Assistant would.
const startPlay = (page, stub, location = DEMO) => page.request.post(stub.url + 'play', { data: location });

test.describe('with the clock running', () => {
    test.beforeEach(async ({ page, stub }) => {
        await page.goto(stub.url);
        await expect(page.locator('#state')).toHaveText('Stopped');
    });

    test('Save sends the name to PUT /name', async ({ page, stub }) => {
        const name = page.getByLabel('Name');
        await expect(name).toHaveValue('hearth-a1b2c3');
        await name.fill('Sitting room');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect.poll(() => stub.sent('PUT /name').map((r) => r.body)).toEqual(['Sitting room']);
        await expect(page.getByRole('status')).toHaveText('This sink is called Sitting room.');
        // What the board answers to is what the page then shows itself as.
        await expect(page.locator('#title')).toHaveText('Sitting room');
    });

    test('a name of only spaces is not sent', async ({ page, stub }) => {
        await page.getByLabel('Name').fill('   ');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect(page.getByRole('status')).toHaveText('Enter a name.');
        expect(stub.sent('PUT /name')).toHaveLength(0);
    });

    test("a refused name shows the device's reply", async ({ page, stub }) => {
        stub.next('PUT /name', { status: 409, body: REPLIES.nameRefused });
        await page.getByLabel('Name').fill('Kitchen');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect(page.getByRole('status')).toHaveText(
            'Name Kitchen refused (409): not a name this player can hold: 1 to 32 bytes of text, with no control character in it and no space at either end, or it could not be stored',
        );
        await expect(page.getByRole('status')).toHaveClass(/error/);
    });

    test('the slot width goes to PUT /slot-width, and the sink grows with it', async ({ page, stub }) => {
        await expect(page.getByRole('radio', { name: '32-bit' })).toBeChecked();
        await expect(page.locator('#slot-help')).toHaveText('This sink has 2 slots.');
        await page.getByRole('radio', { name: '16-bit' }).check();
        await expect.poll(() => stub.sent('PUT /slot-width').map((r) => r.body)).toEqual(['16']);
        await expect(page.getByRole('status')).toHaveText('16-bit slots from the next play.');
        // Eight 16-bit slots a line where four 32-bit ones fitted.
        await expect(page.locator('#slot-help')).toHaveText('This sink has 8 slots.');
    });

    test('a slot width refused while a play runs shows the device\'s reply', async ({ page, stub }) => {
        stub.device.framesPerPoll = 1;
        await startPlay(page, stub);
        await expect(page.locator('#state')).toHaveText('Playing');
        await page.getByRole('radio', { name: '16-bit' }).check();
        await expect(page.getByRole('status')).toContainText('16-bit slots refused (409)');
        // The choice goes back to what the board still has.
        await expect(page.getByRole('radio', { name: '32-bit' })).toBeChecked();
    });

    test('the wiring checkbox goes to PUT /wiring and doubles the slots', async ({ page, stub }) => {
        const wiring = page.getByLabel('A second I2S line is wired to a DAC');
        await expect(wiring).not.toBeChecked();
        await wiring.check();
        await expect.poll(() => stub.sent('PUT /wiring').map((r) => r.body)).toEqual(['1']);
        await expect(page.getByRole('status')).toHaveText('A second I2S line, from the next play.');
        await expect(page.locator('#slot-help')).toHaveText('This sink has 8 slots.');
        await wiring.uncheck();
        await expect.poll(() => stub.sent('PUT /wiring').map((r) => r.body)).toEqual(['1', '0']);
        await expect(page.getByRole('status')).toHaveText('One I2S line, from the next play.');
    });

    test('the network form sends the SSID and passphrase, and keeps neither', async ({ page, stub }) => {
        await page.getByLabel('Wi-Fi network').fill('attic');
        await page.getByLabel('Passphrase').fill('hunter2');
        await page.locator('#network-form').getByRole('button', { name: 'Save' }).click();
        await expect.poll(() => stub.sent('PUT /network').map((r) => r.body)).toEqual(['attic\nhunter2']);
        await expect(page.getByRole('status')).toHaveText('Stored attic for the next restart.');
        // The passphrase is cleared from the page once it has gone: a board's
        // page is on an open network and this one is over plain HTTP.
        await expect(page.getByLabel('Passphrase')).toHaveValue('');
        await expect.poll(() => stub.device.ssid).toBe('attic');
    });

    test('the passphrase can be shown while it is typed, and is hidden again once sent', async ({ page, stub }) => {
        const pass = page.getByLabel('Passphrase');
        const show = page.getByRole('button', { name: 'Show' });
        await expect(pass).toHaveAttribute('type', 'password');
        await expect(show).toHaveAttribute('aria-pressed', 'false');
        await pass.fill('hunter2');
        await show.click();
        await expect(pass).toHaveAttribute('type', 'text');
        await expect(show).toHaveAttribute('aria-pressed', 'true');
        await show.click();
        await expect(pass).toHaveAttribute('type', 'password');
        await show.click();
        await page.getByLabel('Wi-Fi network').fill('attic');
        await page.locator('#network-form').getByRole('button', { name: 'Save' }).click();
        await expect.poll(() => stub.sent('PUT /network').map((r) => r.body)).toEqual(['attic\nhunter2']);
        await expect(pass).toHaveValue('');
        await expect(pass).toHaveAttribute('type', 'password');
        await expect(show).toHaveAttribute('aria-pressed', 'false');
    });

    test('a network with no name is not sent', async ({ page, stub }) => {
        await page.getByLabel('Wi-Fi network').fill('  ');
        await page.locator('#network-form').getByRole('button', { name: 'Save' }).click();
        await expect(page.getByRole('status')).toHaveText('Enter a network name.');
        expect(stub.sent('PUT /network')).toHaveLength(0);
    });

    test('a setting whose connection closes unanswered says so', async ({ page, stub }) => {
        // The stand-in closes its idle connections before a drop, so the
        // request is not one Chromium sends again (stub.js, next()).
        stub.next('PUT /name', 'drop');
        await page.getByLabel('Name').fill('Kitchen');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect(page.getByRole('status')).toHaveText('Name Kitchen: no connection.');
    });

    test('a connection closed unanswered says so on a connection that was reused too', async ({ page, stub }) => {
        // Chromium sends a request again that a reused connection closes unanswered, once for each such connection
        // it has, and a run that is slow between next() and the click can meet one (stub.js, resends): here every
        // connection is left open, so that it does. The page sent the request once, and the stand-in saw it once.
        stub.reuseConnections(true);
        stub.next('PUT /name', 'drop');
        await page.getByLabel('Name').fill('Kitchen');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect(page.getByRole('status')).toHaveText('Name Kitchen: no connection.');
        expect(stub.sent('PUT /name')).toHaveLength(1);
    });

    test('a play someone else started shows through to its end', async ({ page, stub }) => {
        // The page still reports a play; it just does not start one.
        stub.device.framesPerPoll = 125;
        await startPlay(page, stub, 'http://10.0.2.2:8000/other.ec3');
        await expect(page.locator('#state')).toHaveText('Playing');
        await expect(page.locator('#location')).toHaveText('http://10.0.2.2:8000/other.ec3');
        await expect(page.locator('#codec')).toHaveText('E-AC-3, 1 substream, dialnorm -31');
        await expect(page.locator('#state')).toHaveText('Finished (end of stream)');
        await expect(page.locator('#played')).toHaveText('0:08 (250 frames)');
    });

    test('Apply sends the output layout to PUT /layout, for the next play', async ({ page, stub }) => {
        const layout = page.getByLabel('Output layout');
        await expect(layout).toHaveValue('2.0');
        await layout.fill('1.0');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['1.0']);
        await expect(page.getByRole('status')).toHaveText('Output layout 1.0 from the next play.');
        await expect(page.locator('#next')).toHaveText('1.0');
    });

    test("a layout's confirmation comes after a play's end that a poll already out brings", async ({ page, stub }) => {
        stub.device.framesPerPoll = 1;
        await startPlay(page, stub);
        await expect(page.locator('#state')).toHaveText('Playing');
        // A poll out while Apply is answered, whose answer then brings the
        // play's end: board/layouts.spec.js on CI, where the test reads a
        // play's end from the device before the page has.
        const release = stub.hold('GET /status');
        const polls = stub.sent('GET /status').length;
        await expect.poll(() => stub.sent('GET /status').length).toBe(polls + 1);
        stub.device.framesPerPoll = 250;
        await page.getByLabel('Output layout').fill('1.0');
        const answered = page.waitForEvent('requestfinished', (request) => request.url().endsWith('/layout'));
        await page.getByRole('button', { name: 'Apply' }).click();
        await answered;
        // Time for the page to take Apply's answer in before the poll's comes.
        await page.waitForTimeout(300);
        release();
        await expect(page.locator('#state')).toHaveText('Finished (end of stream)');
        await expect(page.getByRole('status')).toHaveText('Output layout 1.0 from the next play.');
    });

    test('a speaker list is a layout too', async ({ page, stub }) => {
        await page.getByLabel('Output layout').fill('L,R');
        await page.getByLabel('Output layout').press('Enter');
        await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['L,R']);
        await expect(page.getByRole('status')).toHaveText('Output layout L,R from the next play.');
    });

    test('a layout the sink cannot carry is refused, and the page says why', async ({ page, stub }) => {
        await page.getByLabel('Output layout').fill('5.1');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['5.1']);
        await expect(page.getByRole('status')).toHaveText("Output layout 5.1 refused (409): 5.1 needs 6 slots; this board's output carries 2.");
        await expect(page.locator('#next')).toHaveText('2.0');
    });

    test("a layout refused for what it says is refused in the device's words", async ({ page, stub }) => {
        // Not a name OutputLayout has, so the page has no count to give.
        await page.getByLabel('Output layout').fill('6.1');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect(page.getByRole('status')).toHaveText(
            'Output layout 6.1 refused (409): not a layout this player can play: check the name or the list, and that it has no more slots than the sink',
        );
    });

    test('a preset sends its layout to PUT /layout at once', async ({ page, stub }) => {
        await expect(page.getByRole('radio', { name: '2.0', exact: true })).toBeChecked();
        await page.getByRole('radio', { name: '1.0', exact: true }).check();
        await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['1.0']);
        await expect(page.getByRole('status')).toHaveText('Output layout 1.0 from the next play.');
        await expect(page.getByLabel('Output layout')).toHaveValue('1.0');
        await expect(page.locator('#next')).toHaveText('1.0');
        await expect(page.getByRole('radio', { name: '1.0', exact: true })).toBeChecked();
    });

    test('a refused preset goes back to the layout the board has', async ({ page, stub }) => {
        stub.device.sinkSlots = 8;
        await page.reload();
        stub.next('PUT /layout', { status: 409, body: REPLIES.layoutRefused });
        await page.getByRole('radio', { name: '5.1', exact: true }).check();
        await expect(page.getByRole('status')).toHaveText(`Output layout 5.1 refused (409): ${REPLIES.layoutRefused.trim()}`);
        await expect(page.getByRole('radio', { name: '2.0', exact: true })).toBeChecked();
    });

    test('the presets the sink cannot carry cannot be chosen', async ({ page, stub }) => {
        const enabled = () =>
            page.locator('#layouts input').evaluateAll((presets) => presets.filter((p) => !p.disabled).map((p) => p.value));
        await expect.poll(enabled).toEqual(['1.0', '2.0']);
        await expect(page.getByRole('radio', { name: '5.1', exact: true })).toBeDisabled();
        await expect(page.getByLabel('Output layout')).toHaveAccessibleDescription(/This sink has 2 slots\.$/);
        stub.device.sinkSlots = 16;
        await page.reload();
        await expect.poll(enabled).toEqual(['1.0', '2.0', '5.1', '7.1', '5.1.2', '5.1.4', '7.1.4', '9.1.6']);
        await expect(page.getByLabel('Output layout')).toHaveAccessibleDescription(/This sink has 16 slots\.$/);
    });

    // GET /layout-options (iclforge/layout_options.hpp): the board's own verdict on each preset.
    const option = (layout, slots, verdict, reason) => ({ layout, slots, verdict, fix: 'none', verified: verdict === 'supported', ...(reason ? { reason } : {}) });

    test('a preset the board calls unavailable is greyed with its reason, one with a limit keeps its place', async ({ page, stub }) => {
        stub.device.sinkSlots = 8;
        stub.device.layoutOptions = {
            layout: '2.0', slots: 8, slot_bits: 16, profile: 'c6-16mb',
            options: [
                option('1.0', 1, 'supported'),
                option('2.0', 2, 'supported'),
                option('5.1', 6, 'limits', 'This build decodes streams of at most 2 channels, so a wider stream is decoded elsewhere and sent narrower, and 5.1 plays through no more than 2 of its speakers.'),
                option('7.1', 8, 'supported'),
                option('7.1.4', 12, 'unavailable', "7.1.4 needs 12 slots; this board's output carries 8 (16-bit slots, one line). The most this board can carry is 8."),
            ],
        };
        await page.reload();
        const preset = (name) => page.getByRole('radio', { name, exact: true });
        await expect(preset('7.1.4')).toBeDisabled();
        await expect(preset('5.1')).toBeEnabled();
        await expect(preset('7.1')).toBeEnabled();
        // A preset the answer does not list falls back to the page's own count of slots.
        await expect(preset('9.1.6')).toBeDisabled();
        await expect(preset('5.1.2')).toBeEnabled();
        await expect(page.locator('#layouts label').filter({ has: preset('7.1.4') })).toHaveAttribute('title', /^7\.1\.4 needs 12 slots; this board's output carries 8/);
        await expect(page.locator('#layouts label').filter({ has: preset('5.1') })).toHaveAttribute('title', /decodes streams of at most 2 channels/);
        await expect(page.locator('#layouts label').filter({ has: preset('7.1') })).toHaveAttribute('title', '');
        // The reasons, once each, with the layout named where the sentence does not.
        await expect(page.locator('#layout-why-sum')).toHaveText('Why 2 layouts are unavailable or limited');
        const why = page.locator('#layout-why li');
        await expect(why).toHaveCount(2);
        await expect(why.nth(0)).toHaveText(/^5\.1: This build decodes streams of at most 2 channels/);
        await expect(why.nth(1)).toHaveText(/^7\.1\.4 needs 12 slots; this board's output carries 8/);
    });

    test('one layout unavailable reads in the singular, and none hides the list', async ({ page, stub }) => {
        stub.device.layoutOptions = {
            layout: '2.0', slots: 2,
            options: [option('2.0', 2, 'supported'), option('5.1', 6, 'unavailable', '5.1 needs 6 slots; this board\'s output carries 2.')],
        };
        await page.reload();
        await expect(page.locator('#layout-why-sum')).toHaveText('Why 1 layout is unavailable or limited');
        stub.device.layoutOptions = { layout: '2.0', slots: 2, options: [option('2.0', 2, 'supported')] };
        stub.device.sinkSlots = 6; // the ceiling moved, so the options are read again
        await expect(page.locator('#layout-why-box')).toBeHidden();
    });

    test('a board without GET /layout-options leaves the presets to their own count', async ({ page, stub }) => {
        stub.device.layoutOptions = null;
        await page.reload();
        await expect.poll(() => stub.sent('GET /layout-options').length).toBeGreaterThan(0);
        await expect(page.getByRole('radio', { name: '5.1', exact: true })).toBeDisabled();
        await expect(page.getByRole('radio', { name: '2.0', exact: true })).toBeEnabled();
        await expect(page.locator('#layout-why-box')).toBeHidden();
        // Asked once for this ceiling, not at every poll.
        const asked = stub.sent('GET /layout-options').length;
        await page.waitForTimeout(2200);
        expect(stub.sent('GET /layout-options')).toHaveLength(asked);
    });

    test('an answer that is not the list is read again at the next poll', async ({ page, stub }) => {
        stub.next('GET /layout-options', { status: 200, body: 'not json' });
        stub.device.sinkSlots = 12;
        await page.reload();
        await expect.poll(() => stub.sent('GET /layout-options').length).toBeGreaterThan(1);
        await expect(page.getByRole('radio', { name: '7.1.4', exact: true })).toBeEnabled();
        await expect(page.getByRole('radio', { name: '9.1.6', exact: true })).toBeDisabled();
        await expect(page.locator('#layout-why li').first()).toContainText('needs');
    });

    test('a list that is not a list leaves the presets to their count', async ({ page, stub }) => {
        stub.device.layoutOptions = { layout: '2.0', options: 'none' };
        await page.reload();
        await expect(page.getByRole('radio', { name: '5.1', exact: true })).toBeDisabled();
    });

    test('the options are read again when the sink grows', async ({ page, stub }) => {
        const enabled = () =>
            page.locator('#layouts input').evaluateAll((presets) => presets.filter((p) => !p.disabled).map((p) => p.value));
        await expect.poll(enabled).toEqual(['1.0', '2.0']);
        stub.device.sinkSlots = 16;
        await expect.poll(enabled).toEqual(['1.0', '2.0', '5.1', '7.1', '5.1.2', '5.1.4', '7.1.4', '9.1.6']);
        expect(stub.sent('GET /layout-options').length).toBeGreaterThan(1);
        await expect(page.locator('#layout-why-box')).toBeHidden();
    });

    test("a play's own output, the speakers it leaves silent, and the next play's layout", async ({ page, stub }) => {
        Object.assign(stub.device, { sinkSlots: 12, layout: '7.1.4', framesPerPoll: 1 });
        await page.reload();
        // AC-3 5.1 in a 7.1.4 room: the rear surrounds and the heights have nothing.
        await startPlay(page, stub, 'http://10.0.2.2:8000/sample.ac3');
        await expect(page.locator('#output')).toHaveText('7.1.4, 12 slots: each channel on the speaker at its location');
        await expect(page.locator('#silent')).toHaveText('Lrs, Rrs, Vhl, Vhr, Lts, Rts: nothing in the stream for these');
        await expect(page.locator('#next')).toBeHidden();
        // A new layout is the next play's; this play keeps its own.
        await page.getByLabel('Output layout').fill('5.1');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect(page.getByRole('status')).toHaveText('Output layout 5.1 from the next play.');
        await expect(page.locator('#next')).toHaveText('5.1');
        await expect(page.locator('#output')).toHaveText(/^7\.1\.4, 12 slots/);
        // The 7.1.4 walk at 5.1: its rears and heights are spread over the room.
        await startPlay(page, stub, 'http://10.0.2.2:8000/714-walk.ec3');
        await expect(page.locator('#output')).toHaveText('5.1, 6 slots: each channel on the speaker at its location');
        await expect(page.locator('#channels')).toHaveText('12: L C R Ls Rs Lrs Rrs Vhl Vhr Lts Rts LFE');
        await expect(page.locator('#silent')).toBeHidden();
        await expect(page.locator('#next')).toBeHidden();
    });

    test('a layout of only spaces is not sent', async ({ page, stub }) => {
        await page.getByLabel('Output layout').fill('  ');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect(page.getByRole('status')).toHaveText('Enter an output layout.');
        expect(stub.sent('PUT /layout')).toHaveLength(0);
    });

    test('a time zone is chosen by region and then city, and sent by its name', async ({ page, stub }) => {
        const region = page.locator('#zone-select');
        const city = page.locator('#zone-city');
        await expect(page.locator('#zone-pick')).toBeVisible();
        // No zone chosen: both lists are at their prompts, and a region alone sends nothing.
        await expect(region).toHaveValue('');
        await expect(city).toHaveValue('');
        await region.selectOption('Australia');
        await expect(city.locator('option')).toHaveCount(13); // the prompt, and Australia's twelve
        await expect(city.locator('option', { hasText: 'Broken Hill' })).toHaveCount(1);
        expect(stub.sent('PUT /timezone')).toHaveLength(0);
        // A city is sent by its name, and the page then shows it as the board holds it.
        await city.selectOption({ label: 'Melbourne' });
        await expect.poll(() => stub.sent('PUT /timezone').map((r) => r.body)).toEqual(['Australia/Melbourne']);
        await expect(page.getByRole('status')).toHaveText('Time zone Australia/Melbourne.');
        // The page says where the board is as a place, and never fills the field with the string the board keeps it by.
        await expect(page.locator('#zone-now')).toHaveText('Now: Melbourne (Australia).');
        await expect(page.locator('#time-now')).toContainText('Melbourne (Australia)');
        await expect(page.locator('#zone-input')).toHaveValue('');
        await expect(region).toHaveValue('Australia');
        await expect(city).toHaveValue('Australia/Melbourne');
        // Save with nothing typed sends nothing: it is not a request for UTC.
        await page.locator('#zone-form').getByRole('button', { name: 'Save' }).click();
        await expect(page.getByRole('status')).toHaveText('Choose a region and a city, or type a zone.');
        expect(stub.sent('PUT /timezone')).toHaveLength(1);
        // A city with a place between its region and itself is named with it.
        await region.selectOption('America');
        await expect(city.locator('option', { hasText: 'Buenos Aires (Argentina)' })).toHaveCount(1);
        await expect(city.locator('option', { hasText: 'New Salem (North Dakota)' })).toHaveCount(1);
        await expect(city.locator('option', { hasText: 'New York' })).toHaveCount(1);
        // A name that has no region is in Other.
        await region.selectOption('Other');
        await city.selectOption({ label: 'UTC' });
        await expect.poll(() => stub.sent('PUT /timezone').map((r) => r.body)).toEqual(['Australia/Melbourne', 'UTC']);
        // A string typed instead has no name; the table has a place for it (Tokyo's), which the lists show.
        await page.locator('#zone-input').fill('JST-9');
        await page.locator('#zone-form').getByRole('button', { name: 'Save' }).click();
        await expect.poll(() => stub.sent('PUT /timezone').map((r) => r.body)).toEqual(['Australia/Melbourne', 'UTC', 'JST-9']);
        await expect(page.locator('#zone-now')).toHaveText('Now: Tokyo (Asia).');
        await expect(region).toHaveValue('Asia');
        await expect(city).toHaveValue('Asia/Tokyo');
        // One in no table is read as its offset and the person's own, and both lists go back to their prompts.
        await page.locator('#zone-input').fill('ZZZ-9:45');
        await page.locator('#zone-form').getByRole('button', { name: 'Save' }).click();
        await expect(page.locator('#zone-now')).toHaveText('Now: UTC+09:45 (custom).');
        await expect(region).toHaveValue('');
        await expect(city).toHaveValue('');
        await expect(page.locator('#zone-input')).not.toHaveValue('AEST-10AEDT,M10.1.0,M4.1.0/3');
    });

    test("a board that has no list of zones, or whose list cannot be read at first, still takes a typed zone", async ({
        page,
        stub,
    }) => {
        // No list: the choice of region and city is not offered, and the field under it is.
        stub.next('GET /time-zones', { status: 404, body: REPLIES.noClock });
        await page.reload();
        await expect(page.locator('#state')).toHaveText('Stopped');
        await expect(page.locator('#zone-input')).toBeVisible();
        await expect(page.locator('#zone-pick')).toBeHidden();
        await page.locator('#zone-input').fill('EST5EDT,M3.2.0,M11.1.0');
        await page.locator('#zone-form').getByRole('button', { name: 'Save' }).click();
        await expect.poll(() => stub.sent('PUT /timezone').map((r) => r.body)).toEqual(['EST5EDT,M3.2.0,M11.1.0']);
        // A read that fails is made again at the next poll, and the list then appears.
        stub.next('GET /time-zones', 'drop');
        await page.reload();
        await expect(page.locator('#zone-pick')).toBeVisible({ timeout: 10_000 });
        await expect(page.locator('#zone-select option')).toHaveCount(12); // the prompt, and the eleven regions
    });
});

// A poll a second up to `last`, each let finish before the clock moves on:
// moving it further at once would expire the answer's own four-second wait
// while the answer is still on its way.
async function everySecondUntil(page, last) {
    for (let second = 1; second <= last; second += 1) {
        await page.clock.runFor(1000);
        await expect(page.locator('#link')).toHaveText(`Status read at 12:00:0${second}.`);
    }
}

test.describe('with the clock held', () => {
    test.beforeEach(async ({ page, stub }) => {
        await holdClockAt(page, new Date('2026-09-11T12:00:00Z'));
        await page.goto(stub.url);
        await expect(page.locator('#state')).toHaveText('Stopped');
    });

    test('a setting the device never answers times out after four seconds', async ({ page, stub }) => {
        stub.next('PUT /name', 'hang');
        await page.getByLabel('Name').fill('Kitchen');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect.poll(() => stub.sent('PUT /name').length).toBe(1);
        await page.clock.runFor(4000);
        await expect(page.getByRole('status')).toHaveText('Name Kitchen: no answer in 4 s.');
    });

    test('a status read begun before a choice was answered does not undo it', async ({ page, stub }) => {
        // What the board said before the change, answered to a read that was
        // already out when the change went: the page keeps the choice, and the
        // read after it - held here until the clock moves - brings the board's.
        const before = await (await page.request.get(stub.url + 'status')).text();
        const release = stub.hold('GET /status');
        const polls = stub.sent('GET /status').length;
        await page.clock.runFor(1000);
        await expect.poll(() => stub.sent('GET /status').length).toBe(polls + 1);
        await page.getByRole('radio', { name: '16-bit' }).check();
        await expect.poll(() => stub.sent('PUT /slot-width').map((r) => r.body)).toEqual(['16']);
        await expect(page.getByRole('status')).toHaveText('16-bit slots from the next play.');
        stub.setStatus(before);
        release();
        stub.setStatus(null);
        await expect(page.locator('#link')).toHaveText('Status read at 12:00:01.');
        await expect(page.getByRole('radio', { name: '16-bit' })).toBeChecked();
        await page.clock.runFor(1);
        await expect.poll(() => stub.sent('GET /status').length).toBe(polls + 2);
        await expect(page.locator('#slot-help')).toHaveText('This sink has 8 slots.');
        await expect(page.getByRole('radio', { name: '16-bit' })).toBeChecked();
    });

    test('a message leaves after six seconds; an error stays until it is replaced or clicked', async ({ page, stub }) => {
        const toast = page.getByRole('status');
        // A second at a time, each poll let finish before the clock moves on
        // (see everySecondUntil below).
        const seconds = async (from, to) => {
            for (let second = from; second <= to; second += 1) {
                await page.clock.runFor(1000);
                await expect(page.locator('#link')).toHaveText(`Status read at 12:00:${String(second).padStart(2, '0')}.`);
            }
        };
        await page.getByLabel('Name').fill('Attic');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect(toast).toHaveText('This sink is called Attic.');
        await seconds(1, 5);
        await expect(toast).not.toHaveClass(/gone/);
        await seconds(6, 6);
        await expect(toast).toHaveClass(/gone/);
        stub.next('PUT /name', { status: 409, body: REPLIES.nameRefused });
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect(toast).toHaveClass(/error/);
        await expect(toast).not.toHaveClass(/gone/);
        await seconds(7, 13);
        await expect(toast).not.toHaveClass(/gone/);
        await toast.click();
        await expect(toast).toHaveClass(/gone/);
        // Still read out: the text stays where a screen reader found it.
        await expect(toast).toContainText('refused (409)');
    });

    test("a play someone else started keeps the page's own clock lines coming", async ({ page, stub }) => {
        stub.device.framesPerPoll = 1;
        await startPlay(page, stub);
        // The page learns of it at its next poll rather than at once: nothing
        // it did started this play, so nothing it did asks the device again.
        await everySecondUntil(page, 7);
        await expect(page.locator('#state')).toHaveText('Playing');
    });
});
