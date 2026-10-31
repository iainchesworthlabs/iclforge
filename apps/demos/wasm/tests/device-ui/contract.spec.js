// @ts-check
'use strict';

// The stand-in device (stub.js) against the firmware it stands in for, and the
// page against the routes the firmware registers. If control.cpp's replies,
// headers or routes change, these fail until the stand-in follows; a request
// the page makes to a route the firmware does not have fails here, before it
// reaches a board.

const fs = require('fs');
const http = require('http');
const path = require('path');
const zlib = require('zlib');
const { test, expect } = require('@playwright/test');
const {
    REPLIES,
    FIRMWARE,
    FIRMWARE_PARTS,
    ROUTES,
    POLICY,
    UI_DIR,
    startStub,
    idleSendspin,
    playingSendspin,
    pairedServer,
    firmwareTrial,
    updateNotBuilt,
    updateOffer,
    cableNetwork,
    screenBoard,
    timeBoard,
    PREFERENCES,
    validTimeZone,
    zonePlace,
    zoneReadable,
} = require('./stub');

const COMPONENT = path.resolve(__dirname, '../../../../../firmware/esp-idf/iclforge');
// The two firmware headers below are not the component's: they live in
// libs/device, which the component and the host tests both compile.
const LIBS_DEVICE = path.resolve(__dirname, '../../../../../libs/device/include/iclforge');
const CONTROL = fs.readFileSync(path.join(COMPONENT, 'src/control.cpp'), 'utf8');
const COMPONENT_CMAKE = fs.readFileSync(path.join(COMPONENT, 'CMakeLists.txt'), 'utf8');
// iclforge::Firmware answers the firmware routes that Control carries, and
// firmware_image.hpp words a refused image; firmware_status.hpp writes
// GET /firmware.
const FIRMWARE_CPP = fs.readFileSync(path.join(COMPONENT, 'src/firmware.cpp'), 'utf8');
const FIRMWARE_IMAGE = fs.readFileSync(path.join(LIBS_DEVICE, 'firmware_image.hpp'), 'utf8');
const FIRMWARE_STATUS = fs.readFileSync(path.join(LIBS_DEVICE, 'firmware_status.hpp'), 'utf8');
// What a firmware without updates from GitHub releases says (the routes are control.cpp's, and the reply
// is this file's), and the body firmware_update.hpp writes for GET /firmware/available.
const PULL_NONE = fs.readFileSync(path.join(COMPONENT, 'src/firmware_pull_none.cpp'), 'utf8');
const FIRMWARE_UPDATE = fs.readFileSync(path.join(LIBS_DEVICE, 'firmware_update.hpp'), 'utf8');
const SCRIPT = fs.readFileSync(path.join(UI_DIR, 'iclforge_ui.js'), 'utf8');
const PAGE = fs.readFileSync(path.join(UI_DIR, 'iclforge_ui.html'), 'utf8');

// Every string control.cpp spells out, read the way the compiler reads them:
// past comments and character literals, adjacent literals joined, and the
// escapes the file uses resolved.
function literals(source) {
    const found = [];
    let end = -1;
    for (let i = 0; i < source.length; ) {
        if (source.startsWith('//', i)) {
            i = source.indexOf('\n', i) + 1 || source.length;
        } else if (source.startsWith('/*', i)) {
            i = source.indexOf('*/', i) + 2;
        } else if (source[i] === "'") {
            i += source[i + 1] === '\\' ? 4 : 3;
        } else if (source[i] === '"') {
            let j = i + 1;
            let text = '';
            for (; source[j] !== '"'; j += 1) {
                if (source[j] === '\\') {
                    j += 1;
                    text += source[j] === 'n' ? '\n' : source[j];
                } else {
                    text += source[j];
                }
            }
            if (end >= 0 && /^\s*$/.test(source.slice(end, i))) {
                found[found.length - 1] += text;
            } else {
                found.push(text);
            }
            i = end = j + 1;
        } else {
            i += 1;
        }
    }
    return found;
}

const STRINGS = literals(CONTROL);

test("the stand-in answers with the firmware's reply texts", () => {
    const strings = [...STRINGS, ...literals(PULL_NONE)];
    for (const [name, text] of Object.entries(REPLIES)) {
        expect(strings, `REPLIES.${name} is not a string in control.cpp or firmware_pull_none.cpp`).toContain(text);
    }
});

test("the stand-in answers the firmware routes with iclforge::Firmware's texts", () => {
    // reply_text adds the newline, so the literals have none.
    const strings = [...literals(FIRMWARE_CPP), ...literals(FIRMWARE_IMAGE)];
    for (const [name, text] of Object.entries(FIRMWARE)) {
        expect(strings, `FIRMWARE.${name} is not a string in firmware.cpp or firmware_image.hpp`).toContain(
            text.replace(/\n$/, ''),
        );
    }
    for (const [name, parts] of Object.entries(FIRMWARE_PARTS)) {
        for (const part of parts) {
            expect(strings, `FIRMWARE_PARTS.${name} has "${part}", which is not a string in firmware.cpp`).toContain(part);
        }
    }
});

test("the stand-in sends the page with the firmware's headers", () => {
    expect(STRINGS[STRINGS.indexOf('Content-Security-Policy') + 1]).toBe(POLICY);
    expect(STRINGS[STRINGS.indexOf('Cache-Control') + 1]).toBe('no-cache');
    expect(STRINGS[STRINGS.indexOf('Content-Encoding') + 1]).toBe('gzip');
    expect(STRINGS).toContain('text/html; charset=utf-8');
    expect(STRINGS).toContain('text/javascript; charset=utf-8');
});

test('the stand-in and the firmware both send the page gzipped, and a browser reads it as written', async () => {
    // The component embeds the gzip of each file (tools/gzip_asset.py) and send_file says so; the stand-in does the same.
    expect(COMPONENT_CMAKE).toContain('gzip_asset.py');
    expect(COMPONENT_CMAKE).not.toContain('EMBED_FILES');
    for (const symbol of ['iclforge_ui_html_gz_start', 'iclforge_ui_html_gz_end', 'iclforge_ui_js_gz_start', 'iclforge_ui_js_gz_end']) {
        expect(CONTROL, symbol).toContain(`asm("_binary_${symbol}")`);
    }
    const stub = await startStub();
    try {
        for (const [route, source] of [
            ['', 'iclforge_ui.html'],
            ['ui.js', 'iclforge_ui.js'],
        ]) {
            const reply = await new Promise((resolve, reject) => {
                http.get(`${stub.url}${route}`, (res) => {
                    const parts = [];
                    res.on('data', (part) => parts.push(part));
                    res.on('end', () => resolve({ headers: res.headers, body: Buffer.concat(parts) }));
                }).on('error', reject);
            });
            expect(reply.headers['content-encoding'], route).toBe('gzip');
            const written = fs.readFileSync(path.join(UI_DIR, source));
            expect(zlib.gunzipSync(reply.body).equals(written), route).toBe(true);
            // Smaller by a good deal (the page is 69 KB as written), which is the reason for it.
            expect(reply.body.length, route).toBeLessThan(written.length / 2);
        }
    } finally {
        await stub.close();
    }
});

test("the stand-in has the firmware's routes and no others", () => {
    // The designated initialisers sit on one line or several, depending on
    // how long the route's name is, so the gap between them is any whitespace
    // rather than one space - a route whose name pushed it onto two lines was
    // silently not checked here.
    const registered = [...CONTROL.matchAll(/\.uri = "([^"]+)",\s*\.method = HTTP_(GET|POST|PUT|DELETE)/g)].map(
        (m) => `${m[2]} ${m[1]}`,
    );
    const byName = (a, b) => a.localeCompare(b);
    expect(registered.sort(byName)).toEqual([...ROUTES].sort(byName));
});

test('every request the page makes is to a route the firmware registers', () => {
    // A hyphen is part of a route (/slot-width), and so is a slash
    // (/firmware/rollback), so the name is [a-z/-]+ and not [a-z]+ - which
    // matched a route up to the hyphen and then nothing, leaving a real
    // request out of this list rather than failing it. The requests are
    // call()'s and act()'s, the firmware section's restart() around act(),
    // and the upload's XMLHttpRequest open().
    const made = [...SCRIPT.matchAll(/(?:call|act|restart|open)\((?:[^,()]+, )?'(GET|POST|PUT)', '([a-z/-]+)'/g)].map(
        (m) => `${m[1]} /${m[2]}`,
    );
    // No POST /play, /stop or /volume: a server owns playback from B2 on, and
    // the page is what the board itself is. One call each: the pairing
    // actions share one, and the layout's presets and its field another. No
    // PUT /firmware/mode: an upload enters flash mode by itself, and a
    // restart leaves it.
    expect(made.sort((a, b) => a.localeCompare(b))).toEqual([
        'GET /firmware',
        'GET /hardware',
        'GET /layout-options',
        'GET /pairing',
        'GET /status',
        'GET /time-zones',
        'GET /trim',
        'POST /identify',
        'POST /pairing',
        'POST /restart',
        'PUT /firmware',
        'PUT /firmware/rollback',
        'PUT /layout',
        'PUT /name',
        'PUT /network',
        'PUT /ntp-server',
        'PUT /slot-width',
        'PUT /time-sync',
        'PUT /timezone',
        'PUT /trim',
        'PUT /wiring',
    ]);
    for (const route of made) {
        expect(ROUTES).toContain(route);
    }
    // Two ways out - fetch, and for the upload's progress an XMLHttpRequest -
    // and no address but the device's own.
    expect(SCRIPT.match(/fetch\(/g)).toHaveLength(1);
    expect(SCRIPT.match(/new XMLHttpRequest\(/g)).toHaveLength(1);
    expect(SCRIPT).not.toMatch(/https?:\/\//);
});

test('the page asks for nothing the device does not serve', () => {
    const links = [...PAGE.matchAll(/(?:src|href)="([^"]*)"/g)].map((m) => m[1]);
    // The icon is inline, a single pixel of the signal red, so that a browser
    // does not ask for /favicon.ico, which the firmware does not serve.
    const inline = links.filter((link) => link.startsWith('data:'));
    expect(inline).toEqual([expect.stringMatching(/^data:image\/png;base64,[A-Za-z0-9+/=]+$/)]);
    const routes = links.filter((link) => !inline.includes(link));
    // The core dump and the console's recent output are links: the browser
    // saves the one and shows the other as text.
    expect(routes.sort((a, b) => a.localeCompare(b))).toEqual(['api', 'api', 'firmware/coredump', 'log', 'status', 'ui.js']);
    for (const route of ['GET /firmware/coredump', 'GET /log']) {
        expect(ROUTES).toContain(route);
    }
    expect(PAGE).not.toMatch(/url\(|@import|https?:\/\//);
});

test("the stand-in writes GET /status's keys in the firmware's order", async () => {
    // Every key on_status writes, in the order it writes them: the stream's own
    // come straight after "stream", and the Sendspin player's, which
    // append_sendspin writes, straight after "sendspin".
    const written = (from, to) =>
        [
            ...CONTROL.slice(CONTROL.indexOf(from), CONTROL.indexOf(to)).matchAll(
                /append_(?:key|number|bool|signed|string|optional|levels)\(out, "([a-z_]+)"/g,
            ),
        ].map((m) => m[1]);
    const player = written('void append_sendspin', 'esp_err_t send_text');
    const firmware = written('static esp_err_t on_status', 'static esp_err_t on_play').flatMap((key) =>
        key === 'sendspin' ? [key, ...player] : [key],
    );
    expect(player.length).toBeGreaterThan(30);
    const stub = await startStub();
    try {
        await fetch(`${stub.url}play`, { method: 'POST', body: 'http://10.0.2.2:8000/demo.ec3' });
        for (const sendspin of [idleSendspin(), playingSendspin()]) {
            stub.device.sendspin = sendspin;
            const body = JSON.parse(await (await fetch(`${stub.url}status`)).text());
            const keys = Object.keys(body).flatMap((key) =>
                ['stream', 'sendspin', 'network', 'time'].includes(key) ? [key, ...Object.keys(body[key])] : [key],
            );
            expect(keys).toEqual(firmware);
        }
    } finally {
        await stub.close();
    }
});

test("the stand-in writes GET /pairing's keys in the firmware's order", async () => {
    // on_pairing_get writes the object, then each server's keys inside
    // "servers"; on_pairing, the POST, follows it.
    const firmware = [
        ...CONTROL.slice(
            CONTROL.indexOf('static esp_err_t on_pairing_get'),
            CONTROL.indexOf('static esp_err_t on_pairing('),
        ).matchAll(/append_(?:key|number|bool|string)\(out, "([a-z_]+)"/g),
    ].map((m) => m[1]);
    expect(firmware).toEqual(['capacity', 'servers', 'server_id', 'name', 'connected', 'last_playback', 'seen']);
    const stub = await startStub();
    try {
        stub.device.pairings = [pairedServer({ server_id: 'Yx3kP0aZtYk4Q9mLr2vNw8sJc1bXe5hGd7fAu3oKp6i', name: 'Hearth on the desk' })];
        const body = JSON.parse(await (await fetch(`${stub.url}pairing`)).text());
        const keys = Object.keys(body).flatMap((key) =>
            key === 'servers' ? [key, ...Object.keys(body.servers[0])] : [key],
        );
        expect(keys).toEqual(firmware);
    } finally {
        await stub.close();
    }
});

test("the stand-in writes GET /firmware's keys in the firmware's order", async () => {
    // render_firmware_status writes each part with a JsonObject of its own -
    // `object` for the body, then trial, upload, last and part - and
    // append_slot writes a slot. A part that does not apply is written with
    // null() under the same key, so a key comes twice in a row there.
    const keys = (from, to, name) => {
        const text = FIRMWARE_STATUS.slice(FIRMWARE_STATUS.indexOf(from), FIRMWARE_STATUS.indexOf(to));
        return [...text.matchAll(new RegExp(`\\b${name}\\.(?:text|number|null|key)\\("([a-z_0-9]+)"`, 'g'))]
            .map((m) => m[1])
            .filter((key, i, all) => key !== all[i - 1]);
    };
    const render = (name) => keys('inline std::string render_firmware_status', 'bootloader_version);', name);
    const slot = keys('inline void append_slot', '}  // namespace detail', 'object');
    const firmware = {
        body: [...render('object'), 'bootloader_version'].filter((key, i, all) => all.indexOf(key) === i),
        slot,
        trial: render('trial'),
        upload: render('upload'),
        last: render('last'),
        dump: render('dump'),
        part: render('part'),
    };
    expect(firmware.body).toEqual(expect.arrayContaining(['mode', 'running', 'other', 'partitions']));
    expect(firmware.slot.length).toBe(8);
    const stub = await startStub();
    try {
        // Every part present: a trial, an upload, a last update and a core dump.
        Object.assign(stub.device.firmware, {
            trial: firmwareTrial(),
            upload: { received: 0, total: 4096, stage: 'waiting' },
            last_update: { version: 'v0.11.0', result: 'refused', reason: 'no' },
            coredump: { bytes: 23456, intact: true, task: 'fw_trial', pc: '0x4037a1b2', reason: 'abort()', elf_sha256: '2366bde99' },
        });
        const body = JSON.parse(await (await fetch(`${stub.url}firmware`)).text());
        expect(Object.keys(body)).toEqual(firmware.body);
        expect(Object.keys(body.running)).toEqual(firmware.slot);
        expect(Object.keys(body.other)).toEqual(firmware.slot);
        expect(Object.keys(body.trial)).toEqual(firmware.trial);
        expect(Object.keys(body.upload)).toEqual(firmware.upload);
        expect(Object.keys(body.last_update)).toEqual(firmware.last);
        expect(Object.keys(body.coredump)).toEqual(firmware.dump);
        expect(Object.keys(body.partitions[0])).toEqual(firmware.part);
    } finally {
        await stub.close();
    }
});

test("the stand-in writes GET /firmware/available's keys in the firmware's order", async () => {
    // render_status writes the body with a JsonObject named `object`, and the parts that are objects with one
    // each: latest, trust and error. A part that does not apply is written with null() under the same key,
    // so a key comes twice in a row there; a text that may be empty goes through text_or_null.
    const start = FIRMWARE_UPDATE.indexOf('inline std::string render_status');
    const render = FIRMWARE_UPDATE.slice(start, FIRMWARE_UPDATE.indexOf("out += '\\n';", start));
    const keys = (name) =>
        [
            ...render.matchAll(
                new RegExp(`\\b${name}\\.(?:text|number|null|key)\\("([a-z_0-9]+)"|text_or_null\\(${name}, "([a-z_0-9]+)"`, 'g'),
            ),
        ]
            .map((m) => m[1] ?? m[2])
            .filter((key, i, all) => key !== all[i - 1]);
    const firmware = { body: keys('object'), latest: keys('latest'), trust: keys('trust'), error: keys('error') };
    expect(firmware.body).toEqual(expect.arrayContaining(['supported', 'state', 'latest', 'trust', 'skipped']));
    expect(firmware.latest.length).toBe(5);
    const stub = await startStub();
    try {
        // A firmware without the update: every part that does not apply is null.
        let body = JSON.parse(await (await fetch(`${stub.url}firmware/available`)).text());
        expect(Object.keys(body)).toEqual(firmware.body);
        expect(Object.keys(body.trust)).toEqual(firmware.trust);
        expect(body.latest).toBeNull();
        expect(body.error).toBeNull();
        expect(body).toEqual(updateNotBuilt(stub.device.firmware));
        // A release on offer, and then a check that failed.
        stub.device.update = updateOffer(stub.device.firmware);
        body = JSON.parse(await (await fetch(`${stub.url}firmware/available`)).text());
        expect(Object.keys(body)).toEqual(firmware.body);
        expect(Object.keys(body.latest)).toEqual(firmware.latest);
        stub.device.update = updateOffer(stub.device.firmware, {
            state: 'idle',
            latest: null,
            error: { code: 'untrusted', text: 'The update list was not signed by a key this firmware trusts.' },
        });
        body = JSON.parse(await (await fetch(`${stub.url}firmware/available`)).text());
        expect(Object.keys(body)).toEqual(firmware.body);
        expect(Object.keys(body.error)).toEqual(firmware.error);
    } finally {
        await stub.close();
    }
});

test("the stand-in writes GET /hardware's keys in the firmware's order", async () => {
    // build_hardware_json, not on_hardware itself: on_hardware only sends
    // what Control::start built once, and control.hpp's own comment says why
    // this is a route of its own rather than part of /status - nothing here
    // depends on a play, unlike every field on_status writes.
    const firmware = [
        ...CONTROL.slice(
            CONTROL.indexOf('std::string build_hardware_json'),
            CONTROL.indexOf('void append_sendspin'),
        ).matchAll(/append_\w+\(out, "([a-z_]+)"/g),
    ].map((m) => m[1]);
    expect(firmware.length).toBeGreaterThan(5);
    const stub = await startStub();
    try {
        const body = JSON.parse(await (await fetch(`${stub.url}hardware`)).text());
        expect(Object.keys(body)).toEqual(firmware);
    } finally {
        await stub.close();
    }
});

test("the stand-in writes GET /layout-options' keys in the firmware's order", async () => {
    // layout_options.hpp writes the board's line (layout_options_head_json) and then each layout's
    // (layout_option_json), and control.cpp sends them; a layout the sink cannot carry has a reason, which a
    // supported one leaves out. The keys in the order the source writes them, a key written twice (a true and a
    // false) once.
    const LAYOUT_OPTIONS = fs.readFileSync(path.join(LIBS_DEVICE, 'layout_options.hpp'), 'utf8');
    const keys = (from, to) => {
        const found = [...LAYOUT_OPTIONS.slice(LAYOUT_OPTIONS.indexOf(from), LAYOUT_OPTIONS.indexOf(to)).matchAll(/\\"([a-z_]+)\\":/g)].map(
            (m) => m[1],
        );
        return found.filter((key, at) => found.indexOf(key) === at);
    };
    const firmware = [
        ...keys('layout_options_head_json(const BoardCaps', 'layout_option_json(const LayoutOption'),
        ...keys('layout_option_json(const LayoutOption', 'layout_options_json(const BoardCaps'),
    ];
    // The head's last key opens the list; the stand-in's options array is that key.
    expect(firmware.length).toBeGreaterThan(8);
    expect(CONTROL).toContain('layout_options_head_json(caps, h.layout())');
    const stub = await startStub();
    try {
        const body = JSON.parse(await (await fetch(`${stub.url}layout-options`)).text());
        const { options, ...head } = body;
        const refused = options.find((o) => o.reason);
        expect(refused, 'the stand-in lists a layout the sink cannot carry').toBeDefined();
        expect([...Object.keys(head), 'options', ...Object.keys(refused)]).toEqual(firmware);
        expect(options.find((o) => !o.reason)).not.toHaveProperty('reason');
    } finally {
        await stub.close();
    }
});

test("the stand-in answers GET /panel-stats with the keys panel_stats.hpp writes, in its order", async () => {
    // panel_stats_json writes the panel's object and then the Meters view's; the timings (timing) are four keys each; the tap's are the
    // last object of meters_json. The keys in the order the source writes them.
    const STATS = fs.readFileSync(path.join(LIBS_DEVICE, 'panel_stats.hpp'), 'utf8');
    const between = (from, to) => STATS.slice(STATS.indexOf(from), STATS.indexOf(to));
    const keysOf = (text) =>
        [...text.matchAll(/(?:\b(?:number|signed_number|fixed1|timing|key)\(out, "([a-z_]+)"|\\"([a-z_]+)\\":)/g)].map((m) => m[1] || m[2]);
    // timing() writes the four after the name its caller gives, which is not a literal and so is not among them.
    const timingKeys = keysOf(between('inline void timing(', 'representation_name'));
    expect(timingKeys).toEqual(['last', 'mean', 'max', 'n']);
    const panelSource = keysOf(between('inline std::string panel_stats_json', '}  // namespace iclforge::panelstats'));
    const meterSource = keysOf(between('inline std::string meters_json', 'inline std::string panel_stats_json'));
    const stub = await startStub();
    try {
        stub.device.screen = screenBoard();
        const body = JSON.parse(await (await fetch(`${stub.url}panel-stats`)).text());
        expect(Object.keys(body)).toEqual(['panel', 'meters']);
        const { panel, meters } = body;
        // The source's "panel" and "meters" are the object names; every key after them up to the next object is that object's.
        expect(panelSource.slice(0, 1)).toEqual(['panel']);
        expect(panelSource.slice(1, panelSource.indexOf('meters'))).toEqual(Object.keys(panel));
        for (const name of ['refresh_us']) {
            expect(Object.keys(panel[name])).toEqual(timingKeys);
        }
        for (const name of ['analysis_us', 'widgets_us', 'render_us']) {
            expect(Object.keys(meters[name])).toEqual(timingKeys);
        }
        expect(meterSource).toEqual([...Object.keys(meters), ...Object.keys(meters.tap)]);
        expect(CONTROL).toContain('h.panel_stats()');
    } finally {
        await stub.close();
    }
});

test('the stand-in answers GET /panel-stats with a refusal on a board with no screen, and lists the route', async () => {
    const stub = await startStub();
    try {
        // A board with no screen has nothing to measure; the route is listed all the same, as every board's.
        const reply = await fetch(`${stub.url}panel-stats`);
        expect(reply.status).toBe(404);
        expect(await reply.text()).toBe(REPLIES.noScreen);
        const api = await (await fetch(`${stub.url}api`)).text();
        expect(api).toContain('GET  /panel-stats   what the panel and the Meters view cost, as JSON, on a board with a screen');
        expect(CONTROL).toContain('GET  /panel-stats   what the panel and the Meters view cost, as JSON, on a board with a screen');
    } finally {
        await stub.close();
    }
});

test("the stand-in writes a cable board's network interfaces in the firmware's order", () => {
    // append_interface writes both interfaces' keys: the shared ones, then the station's own
    // (configured, ssid, rssi_dbm) or else the cable's (speed_mbps, full_duplex). on_status
    // writes the cable's object before the station's.
    const helper = CONTROL.slice(CONTROL.indexOf('void append_interface'), CONTROL.indexOf("// esp_chip_info()'s model"));
    const keys = [...helper.matchAll(/append_(?:key|number|bool|signed|string|optional|levels)\(out, "([a-z_]+)"/g)].map(
        (m) => m[1],
    );
    const shared = ['enabled', 'link', 'address', 'mac'];
    expect(keys).toEqual([...shared, 'configured', 'ssid', 'rssi_dbm', 'speed_mbps', 'full_duplex']);
    const network = cableNetwork();
    expect(Object.keys(network)).toEqual(['kind', 'ssid', 'rssi_dbm', 'address', 'preference', 'interfaces']);
    expect(Object.keys(network.interfaces)).toEqual(['ethernet', 'wifi']);
    expect(Object.keys(network.interfaces.wifi)).toEqual([...shared, 'configured', 'ssid', 'rssi_dbm']);
    expect(Object.keys(network.interfaces.ethernet)).toEqual([...shared, 'speed_mbps', 'full_duplex']);
    const status = CONTROL.slice(CONTROL.indexOf('static esp_err_t on_status'), CONTROL.indexOf('static esp_err_t on_play'));
    expect(status.indexOf('append_interface(out, "ethernet"')).toBeGreaterThan(0);
    expect(status.indexOf('append_interface(out, "ethernet"')).toBeLessThan(status.indexOf('append_interface(out, "wifi"'));
});

test('the stand-in answers /network-preference as the firmware does', async () => {
    const stub = await startStub();
    try {
        // A board with one network has no preference between networks.
        let reply = await fetch(`${stub.url}network-preference`);
        expect(reply.status).toBe(404);
        expect(await reply.text()).toBe(REPLIES.networkPreferenceNone);
        reply = await fetch(`${stub.url}network-preference`, { method: 'PUT', body: 'ethernet' });
        expect(reply.status).toBe(409);
        expect(await reply.text()).toBe(REPLIES.networkPreferenceNone);
        // One with a cable as well has, and takes three words.
        stub.device.network = cableNetwork();
        reply = await fetch(`${stub.url}network-preference`);
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe('auto\n');
        reply = await fetch(`${stub.url}network-preference`, { method: 'PUT', body: 'cable' });
        expect(reply.status).toBe(400);
        expect(await reply.text()).toBe(REPLIES.networkPreferenceBad);
        reply = await fetch(`${stub.url}network-preference`, { method: 'PUT', body: 'wifi' });
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe(REPLIES.networkPreferenceOk);
        const status = JSON.parse(await (await fetch(`${stub.url}status`)).text());
        expect(status.network.preference).toBe('wifi');
        expect(status.network.interfaces.ethernet.enabled).toBe(false);
        // The station alone needs a network to join.
        stub.device.ssid = '';
        reply = await fetch(`${stub.url}network-preference`, { method: 'PUT', body: 'wifi' });
        expect(reply.status).toBe(409);
        expect(await reply.text()).toBe(REPLIES.networkPreferenceRefused);
    } finally {
        await stub.close();
    }
});

// Zones the firmware takes and refuses: the same lists as libs/device/tests/test_wall_clock.cpp, which holds
// the grammar itself, so that the stand-in's copy of it cannot drift from the board's.
const ZONES_TAKEN = [
    'AEST-10AEDT,M10.1.0,M4.1.0/3',
    'EST5EDT,M3.2.0,M11.1.0',
    'JST-9',
    'UTC0',
    '<+0530>-5:30',
    'ACST-9:30ACDT,M10.1.0,M4.1.0/3',
    'LHST-10:30LHDT-11,M10.1.0,M4.1.0',
    'IST-1GMT0,M10.5.0,M3.5.0/1',
    '<-03>3',
    'EST5EDT,J60,J300/2:30:00',
    'EST5EDT,M3.2.0/-1,M11.1.0/25',
];
const ZONES_REFUSED = [
    '',
    'AEST',
    '-10',
    'None',
    'australia/sydney',
    'AE-10',
    '<>-10',
    'AEST-25',
    'AEST-10:60',
    'AEST-10AEDT',
    'AEST-10AEDT,M10.1.0',
    'AEST-10AEDT,M13.1.0,M4.1.0',
    'AEST-10AEDT,M10.6.0,M4.1.0',
    'AEST-10AEDT,M10.1.7,M4.1.0',
    'AEST-10AEDT,J0,J300',
    'AEST-10AEDT,366,300',
    'AEST-10AEDT,M10.1.0,M4.1.0,M1.1.1',
    'UTC0x',
    'AEST -10',
    'ABCDEFGHIJKLMNOPQ-10',
    `AEST-10AEDT,M10.1.0/${'1'.repeat(63)},M4.1.0`,
];

const SERVERS_TAKEN = ['pool.ntp.org', '192.168.1.5', 'ntp', 'my-router.lan', 'NTP1.Example.COM', '0.au.pool.ntp.org', '255.255.255.255'];
const SERVERS_REFUSED = [
    '',
    'None',
    'NONE',
    '256.1.1.1',
    '1.2.3',
    '192.168.01.5',
    '-pool.ntp.org',
    'pool-.ntp.org',
    'pool..ntp.org',
    'pool.ntp.org.',
    'pool.ntp.org:123',
    'http://pool.ntp.org',
    'pool_ntp.org',
    '[::1]',
    'ntp.123',
    `${'a'.repeat(64)}`,
];

// The zones the board offers by name are libs/device's time_zone_data.hpp, written by tools/hearth/gen_time_zones.py;
// the stand-in serves that file's rows, and the page offers what GET /time-zones sends.
const ZONE_DATA = fs.readFileSync(path.join(LIBS_DEVICE, 'time_zone_data.hpp'), 'utf8');
const ZONE_ROWS = [...ZONE_DATA.matchAll(/\{"([^"]+)", "([^"]+)"\},/g)].map((m) => [m[1], m[2]]);

test('every zone the board offers by name is a string the stand-in takes, and the list is sorted', () => {
    expect(ZONE_ROWS.length).toBeGreaterThan(400);
    expect(ZONE_ROWS.length).toBe(Number(ZONE_DATA.match(/(\d+) zones/)[1]));
    // The stand-in's own check, not a request to it: PUT /timezone answers 200 for exactly the zones validTimeZone
    // takes (the next tests drive the route); libs/device/tests/test_time_zones.cpp holds the grammar itself to the same
    // rows, so that the stand-in's copy of it cannot drift from the board's.
    for (const [name, zone] of ZONE_ROWS) {
        expect(validTimeZone(zone), `${name}: ${zone}`).toBe(true);
        expect(validTimeZone(name), `${name} must not read as a POSIX string`).toBe(false);
    }
    const names = ZONE_ROWS.map((z) => z[0]);
    expect(new Set(names).size, 'a zone listed twice').toBe(names.length);
    // In code-unit order, which is the order the board's binary search needs (time_zones.hpp); localeCompare would
    // place '_' and '-' differently.
    expect([...names].sort((a, b) => (a < b ? -1 : Number(a > b)))).toEqual(names);
    for (const name of ['Australia/Melbourne', 'Australia/Sydney', 'Australia/Perth', 'America/New_York', 'Europe/London', 'UTC']) {
        expect(names).toContain(name);
    }
});

test('the stand-in answers /timezone, /ntp-server and /time-sync as the firmware does', async () => {
    const stub = await startStub();
    try {
        // A board that keeps no time (no network, or QEMU's) has nothing to set: what parses is told so, and
        // what does not is refused first, as the routes read the body before they ask whether there is a clock.
        stub.device.time = undefined;
        for (const [route, good, bad] of [
            ['timezone', 'Australia/Melbourne', 'Sydney'],
            ['ntp-server', 'pool.ntp.org', 'pool.ntp.org:123'],
            ['time-sync', 'off', 'maybe'],
        ]) {
            let reply = await fetch(`${stub.url}${route}`);
            expect(reply.status).toBe(404);
            expect(await reply.text()).toBe(REPLIES.noClock);
            if (route === 'timezone') {
                reply = await fetch(`${stub.url}time-zones`);
                expect(reply.status).toBe(404);
                expect(await reply.text()).toBe(REPLIES.noClock);
            }
            reply = await fetch(`${stub.url}${route}`, { method: 'PUT', body: good });
            expect(reply.status).toBe(409);
            expect(await reply.text()).toBe(REPLIES.noClock);
            reply = await fetch(`${stub.url}${route}`, { method: 'PUT', body: bad });
            expect(reply.status).toBe(400);
        }

        // One that keeps it starts as a board should: asking, on, no zone, no server of its own.
        stub.device.time = timeBoard();
        expect(await (await fetch(`${stub.url}timezone`)).text()).toBe('none\n');
        expect(await (await fetch(`${stub.url}ntp-server`)).text()).toBe('none\n');
        expect(await (await fetch(`${stub.url}time-sync`)).text()).toBe('on\n');

        // The zone: a POSIX string, a name from GET /time-zones, or `none`.
        let reply;
        for (const zone of ZONES_TAKEN) {
            reply = await fetch(`${stub.url}timezone`, { method: 'PUT', body: `${zone}\n` });
            expect(reply.status, zone).toBe(200);
            expect(await reply.text()).toBe(REPLIES.timeZoneOk);
            expect(await (await fetch(`${stub.url}timezone`)).text()).toBe(`${zone}\n`);
        }
        for (const zone of ZONES_REFUSED) {
            reply = await fetch(`${stub.url}timezone`, { method: 'PUT', body: zone });
            expect(reply.status, JSON.stringify(zone)).toBe(400);
            expect(await reply.text()).toBe(REPLIES.timeZoneBad);
        }
        // A refusal changes nothing: the last zone taken is still the one.
        expect(await (await fetch(`${stub.url}timezone`)).text()).toBe('EST5EDT,M3.2.0/-1,M11.1.0/25\n');
        reply = await fetch(`${stub.url}timezone`, { method: 'PUT', body: 'none\n' });
        expect(reply.status).toBe(200);
        expect(await (await fetch(`${stub.url}timezone`)).text()).toBe('none\n');
        // A zone the grammar takes and the board's C library reads differently is refused with a 409 and kept
        // out.
        stub.device.time.zoneRefused = true;
        reply = await fetch(`${stub.url}timezone`, { method: 'PUT', body: 'JST-9' });
        expect(reply.status).toBe(409);
        expect(await reply.text()).toBe(REPLIES.timeZoneRefused);
        expect(await (await fetch(`${stub.url}timezone`)).text()).toBe('none\n');
        stub.device.time.zoneRefused = false;

        // A name is stored with its string, and says so in GET /status's time (zone_name); a string has none.
        reply = await fetch(`${stub.url}timezone`, { method: 'PUT', body: 'Australia/Melbourne\n' });
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe(REPLIES.timeZoneOk);
        expect(await (await fetch(`${stub.url}timezone`)).text()).toBe('AEST-10AEDT,M10.1.0,M4.1.0/3\n');
        let time = JSON.parse(await (await fetch(`${stub.url}status`)).text()).time;
        expect(time.zone).toBe('AEST-10AEDT,M10.1.0,M4.1.0/3');
        expect(time.zone_name).toBe('Australia/Melbourne');
        // What a person reads is the place (zone_label), and the place the table holds for the zone (zone_place): never the
        // string, which only tools read.
        expect(time.zone_place).toBe('Australia/Melbourne');
        expect(time.zone_label).toBe('Melbourne (Australia)');
        reply = await fetch(`${stub.url}timezone`, { method: 'PUT', body: 'AEST-10AEDT,M10.1.0,M4.1.0/3' });
        expect(reply.status).toBe(200);
        time = JSON.parse(await (await fetch(`${stub.url}status`)).text()).time;
        expect(time.zone_name, 'a string somebody typed has no name').toBe('none');
        // ... but the table has a place for it (the most populous of those that share it), which is what is read.
        expect(time.zone_place).toBe('Australia/Melbourne');
        expect(time.zone_label).toBe('Melbourne (Australia)');
        // A name that is chosen is the one that is read, whatever the string alone would say.
        reply = await fetch(`${stub.url}timezone`, { method: 'PUT', body: 'Australia/Sydney' });
        expect(reply.status).toBe(200);
        time = JSON.parse(await (await fetch(`${stub.url}status`)).text()).time;
        expect(time.zone_label).toBe('Sydney (Australia)');
        // A string in no table is read as its offset, and as the person's own.
        for (const [custom, label] of [
            ['ZZZ-9:45', 'UTC+09:45 (custom)'],
            ['<-0330>3:30', 'UTC-03:30 (custom)'],
            ['CUT-9:30CUD,M10.1.0,M4.1.0/3', 'UTC+09:30 (custom, with daylight saving)'],
        ]) {
            reply = await fetch(`${stub.url}timezone`, { method: 'PUT', body: custom });
            expect(reply.status, custom).toBe(200);
            time = JSON.parse(await (await fetch(`${stub.url}status`)).text()).time;
            expect(time.zone, custom).toBe(custom);
            expect(time.zone_place, custom).toBe('none');
            expect(time.zone_label, custom).toBe(label);
        }
        // The string the board keeps is never in what a person reads, for any string the table holds. What a PUT of the
        // string and a GET /status read back, asked of the stand-in's own functions: the table's rows are not sent
        // over HTTP (the stand-in answers 200 to each, which the grammar check of the first test holds it to).
        for (const posix of new Set(ZONE_ROWS.map((z) => z[1]))) {
            expect(zoneReadable(posix, ''), posix).not.toContain(posix);
            expect(ZONE_ROWS.map((z) => z[0]), posix).toContain(zonePlace(posix, ''));
        }
        for (const bad of ['australia/melbourne', 'Australia/Melburne', 'Melbourne', 'Australia/Melbourne extra']) {
            reply = await fetch(`${stub.url}timezone`, { method: 'PUT', body: bad });
            expect(reply.status, bad).toBe(400);
            expect(await reply.text()).toBe(REPLIES.timeZoneBad);
        }
        reply = await fetch(`${stub.url}timezone`, { method: 'PUT', body: 'Pacific/Auckland' });
        expect(reply.status).toBe(200);
        reply = await fetch(`${stub.url}timezone`, { method: 'PUT', body: 'none' });
        expect(reply.status).toBe(200);
        time = JSON.parse(await (await fetch(`${stub.url}status`)).text()).time;
        expect(time.zone).toBe('none');
        expect(time.zone_name).toBe('none');
        expect(time.zone_place).toBe('none');
        expect(time.zone_label).toBe('UTC');

        // The names a board offers, one a line, in the table's order.
        reply = await fetch(`${stub.url}time-zones`);
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe(ZONE_ROWS.map((z) => z[0] + '\n').join(''));

        // The server: a host name or an IPv4 address, or `none` for the router's and then the pool.
        for (const server of SERVERS_TAKEN) {
            reply = await fetch(`${stub.url}ntp-server`, { method: 'PUT', body: `${server}\r\n` });
            expect(reply.status, server).toBe(200);
            expect(await reply.text()).toBe(REPLIES.ntpServerOk);
            expect(await (await fetch(`${stub.url}ntp-server`)).text()).toBe(`${server}\n`);
        }
        for (const server of SERVERS_REFUSED) {
            reply = await fetch(`${stub.url}ntp-server`, { method: 'PUT', body: server });
            expect(reply.status, JSON.stringify(server)).toBe(400);
            expect(await reply.text()).toBe(REPLIES.ntpServerBad);
        }
        expect(await (await fetch(`${stub.url}ntp-server`)).text()).toBe('255.255.255.255\n');
        reply = await fetch(`${stub.url}ntp-server`, { method: 'PUT', body: 'none' });
        expect(reply.status).toBe(200);
        expect(await (await fetch(`${stub.url}ntp-server`)).text()).toBe('none\n');

        // Whether to ask at all: on or off.
        for (const bad of ['', 'On', '1', 'true', 'off now']) {
            reply = await fetch(`${stub.url}time-sync`, { method: 'PUT', body: bad });
            expect(reply.status, JSON.stringify(bad)).toBe(400);
            expect(await reply.text()).toBe(REPLIES.timeSyncBad);
        }
        reply = await fetch(`${stub.url}time-sync`, { method: 'PUT', body: 'off\n' });
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe(REPLIES.timeSyncOk);
        expect(await (await fetch(`${stub.url}time-sync`)).text()).toBe('off\n');
        const off = JSON.parse(await (await fetch(`${stub.url}status`)).text()).time;
        expect(off.sync).toBe(false);
        expect(off.asking).toBe(false);
        expect(off.local).toBe(null);
        reply = await fetch(`${stub.url}time-sync`, { method: 'PUT', body: 'on' });
        expect(reply.status).toBe(200);
    } finally {
        await stub.close();
    }
});

test('the stand-in answers /speakers-style as the firmware does', async () => {
    const stub = await startStub();
    try {
        // A board with no screen has nothing to set.
        let reply = await fetch(`${stub.url}speakers-style`);
        expect(reply.status).toBe(404);
        expect(await reply.text()).toBe(REPLIES.noScreen);
        reply = await fetch(`${stub.url}speakers-style`, { method: 'PUT', body: 'pulse' });
        expect(reply.status).toBe(409);
        expect(await reply.text()).toBe(REPLIES.noScreen);
        // One with a screen takes glow or pulse, in lower case, and glows out of the box.
        stub.device.screen = screenBoard();
        reply = await fetch(`${stub.url}speakers-style`);
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe('glow\n');
        for (const bad of ['Pulse', 'pulse now', 'speakers', '1', '']) {
            reply = await fetch(`${stub.url}speakers-style`, { method: 'PUT', body: bad });
            expect(reply.status).toBe(400);
            expect(await reply.text()).toBe(REPLIES.speakersStyleBad);
        }
        reply = await fetch(`${stub.url}speakers-style`, { method: 'PUT', body: 'pulse\n' });
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe(REPLIES.nowPlayingOk);
        expect(await (await fetch(`${stub.url}speakers-style`)).text()).toBe('pulse\n');
        // The route is in the list GET /api says, in the words the firmware says it in.
        const api = await (await fetch(`${stub.url}api`)).text();
        expect(api).toContain('GET  /speakers-style  glow or pulse on a board with a screen: how its speakers show a level; PUT one');
    } finally {
        await stub.close();
    }
});

test('the stand-in answers /now-playing and /dim-after as the firmware does', async () => {
    const stub = await startStub();
    try {
        // A board with no screen has nothing to set.
        for (const route of ['now-playing', 'dim-after']) {
            let reply = await fetch(`${stub.url}${route}`);
            expect(reply.status).toBe(404);
            expect(await reply.text()).toBe(REPLIES.noScreen);
            reply = await fetch(`${stub.url}${route}`, { method: 'PUT', body: route === 'dim-after' ? '5' : 'off' });
            expect(reply.status).toBe(409);
            expect(await reply.text()).toBe(REPLIES.noScreen);
        }
        // One with a screen takes the four words, of which a build may have fewer.
        stub.device.screen = screenBoard();
        let reply = await fetch(`${stub.url}now-playing`);
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe('speakers\n');
        reply = await fetch(`${stub.url}now-playing`, { method: 'PUT', body: 'Speakers' });
        expect(reply.status).toBe(400);
        expect(await reply.text()).toBe(REPLIES.nowPlayingBad);
        // The title card is not built yet, so a build refuses it; the clock is.
        reply = await fetch(`${stub.url}now-playing`, { method: 'PUT', body: 'title' });
        expect(reply.status).toBe(409);
        expect(await reply.text()).toBe(REPLIES.nowPlayingRefused);
        reply = await fetch(`${stub.url}now-playing`, { method: 'PUT', body: 'clock' });
        expect(reply.status).toBe(200);
        expect(await (await fetch(`${stub.url}now-playing`)).text()).toBe('clock\n');
        reply = await fetch(`${stub.url}now-playing`, { method: 'PUT', body: 'off\n' });
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe(REPLIES.nowPlayingOk);
        expect(await (await fetch(`${stub.url}now-playing`)).text()).toBe('off\n');
        // The Meters view is a word the route knows (the 400's sentence names it), and a build that has not got the screen for it refuses it
        // as it does the title card; one that has takes it.
        expect(REPLIES.nowPlayingBad).toContain('meters');
        reply = await fetch(`${stub.url}now-playing`, { method: 'PUT', body: 'meters' });
        expect(reply.status).toBe(409);
        expect(await reply.text()).toBe(REPLIES.nowPlayingRefused);
        stub.device.screen.views.push('meters');
        reply = await fetch(`${stub.url}now-playing`, { method: 'PUT', body: 'meters\n' });
        expect(reply.status).toBe(200);
        expect(await (await fetch(`${stub.url}now-playing`)).text()).toBe('meters\n');
        // And a time to dim after: never, or minutes from 1 to 240.
        expect(await (await fetch(`${stub.url}dim-after`)).text()).toBe('5\n');
        for (const bad of ['soon', '0', '241', '-5', '5 min', '']) {
            reply = await fetch(`${stub.url}dim-after`, { method: 'PUT', body: bad });
            expect(reply.status).toBe(400);
            expect(await reply.text()).toBe(REPLIES.dimAfterBad);
        }
        reply = await fetch(`${stub.url}dim-after`, { method: 'PUT', body: 'never' });
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe(REPLIES.dimAfterOk);
        expect(await (await fetch(`${stub.url}dim-after`)).text()).toBe('never\n');
        reply = await fetch(`${stub.url}dim-after`, { method: 'PUT', body: '30\n' });
        expect(reply.status).toBe(200);
        expect(await (await fetch(`${stub.url}dim-after`)).text()).toBe('30\n');
    } finally {
        await stub.close();
    }
});

test('the stand-in answers /brightness and /clock-format as the firmware does', async () => {
    const stub = await startStub();
    try {
        // A board with no screen has nothing to set.
        for (const [route, good] of [
            ['brightness', '50'],
            ['clock-format', '12'],
        ]) {
            let reply = await fetch(`${stub.url}${route}`);
            expect(reply.status).toBe(404);
            expect(await reply.text()).toBe(REPLIES.noScreen);
            reply = await fetch(`${stub.url}${route}`, { method: 'PUT', body: good });
            expect(reply.status).toBe(409);
            expect(await reply.text()).toBe(REPLIES.noScreen);
        }
        stub.device.screen = screenBoard();

        // The brightness: a whole number of percent from 10 to 100, 80 to begin with.
        expect(await (await fetch(`${stub.url}brightness`)).text()).toBe('80\n');
        for (const bad of ['', '9', '101', '0', '-50', '80%', 'bright', '1000', '50 50']) {
            const reply = await fetch(`${stub.url}brightness`, { method: 'PUT', body: bad });
            expect(reply.status, JSON.stringify(bad)).toBe(400);
            expect(await reply.text()).toBe(REPLIES.brightnessBad);
        }
        for (const good of ['10', '55', '100', ' 70\r\n']) {
            const reply = await fetch(`${stub.url}brightness`, { method: 'PUT', body: good });
            expect(reply.status, good).toBe(200);
            expect(await reply.text()).toBe(REPLIES.brightnessOk);
            expect(await (await fetch(`${stub.url}brightness`)).text()).toBe(`${good.trim()}\n`);
        }
        // A glass with no backlight to dim (a frame buffer with no panel behind it) refuses it, and keeps the last.
        stub.device.screen.dimmable = false;
        let reply = await fetch(`${stub.url}brightness`, { method: 'PUT', body: '30' });
        expect(reply.status).toBe(409);
        expect(await reply.text()).toBe(REPLIES.brightnessRefused);
        expect(await (await fetch(`${stub.url}brightness`)).text()).toBe('70\n');

        // The clock format: 24 or 12 hours, then " seconds" when they are shown.
        expect(await (await fetch(`${stub.url}clock-format`)).text()).toBe('24\n');
        for (const bad of ['', '13', '24h', '12 second', '12  seconds', 'seconds', '12 Seconds', 'twelve']) {
            reply = await fetch(`${stub.url}clock-format`, { method: 'PUT', body: bad });
            expect(reply.status, JSON.stringify(bad)).toBe(400);
            expect(await reply.text()).toBe(REPLIES.clockFormatBad);
        }
        for (const good of ['12', '12 seconds', '24 seconds', '24']) {
            reply = await fetch(`${stub.url}clock-format`, { method: 'PUT', body: `${good}\n` });
            expect(reply.status, good).toBe(200);
            expect(await reply.text()).toBe(REPLIES.clockFormatOk);
            expect(await (await fetch(`${stub.url}clock-format`)).text()).toBe(`${good}\n`);
        }
    } finally {
        await stub.close();
    }
});

test('the stand-in answers /night-schedule and /night-idle as the firmware does', async () => {
    const stub = await startStub();
    try {
        // A board with no screen has nothing to set.
        for (const [route, good] of [
            ['night-schedule', '22:00-07:00 on'],
            ['night-idle', 'on'],
        ]) {
            let reply = await fetch(`${stub.url}${route}`);
            expect(reply.status).toBe(404);
            expect(await reply.text()).toBe(REPLIES.noScreen);
            reply = await fetch(`${stub.url}${route}`, { method: 'PUT', body: good });
            expect(reply.status).toBe(409);
            expect(await reply.text()).toBe(REPLIES.noScreen);
        }
        stub.device.screen = screenBoard();

        // Off, from 22:00 to 07:00, to begin with.
        expect(await (await fetch(`${stub.url}night-schedule`)).text()).toBe('22:00-07:00 off\n');
        expect(await (await fetch(`${stub.url}night-idle`)).text()).toBe('off\n');
        for (const bad of ['', '22:00', '22:00-', '24:00-07:00', '22:60-07:00', '2:00-07:00', '22:00-07:00 maybe', '22:00-07:00 on off', 'dark', '22.00-07.00', 'on off']) {
            const reply = await fetch(`${stub.url}night-schedule`, { method: 'PUT', body: bad });
            expect(reply.status, JSON.stringify(bad)).toBe(400);
            expect(await reply.text()).toBe(REPLIES.nightScheduleBad);
        }
        // The two times alone switch it on; `on` and `off` keep the times; both can be given.
        let reply = await fetch(`${stub.url}night-schedule`, { method: 'PUT', body: '23:30-06:15\n' });
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe(REPLIES.nightOk);
        expect(await (await fetch(`${stub.url}night-schedule`)).text()).toBe('23:30-06:15 on\n');
        await fetch(`${stub.url}night-schedule`, { method: 'PUT', body: 'off' });
        expect(await (await fetch(`${stub.url}night-schedule`)).text()).toBe('23:30-06:15 off\n');
        await fetch(`${stub.url}night-schedule`, { method: 'PUT', body: 'on' });
        expect(await (await fetch(`${stub.url}night-schedule`)).text()).toBe('23:30-06:15 on\n');
        await fetch(`${stub.url}night-schedule`, { method: 'PUT', body: '00:00-23:59 off' });
        expect(await (await fetch(`${stub.url}night-schedule`)).text()).toBe('00:00-23:59 off\n');

        // The idle screen's own setting is separate, and the schedule's words do not touch it.
        for (const bad of ['', 'yes', '1', 'On', 'on off']) {
            reply = await fetch(`${stub.url}night-idle`, { method: 'PUT', body: bad });
            expect(reply.status, JSON.stringify(bad)).toBe(400);
            expect(await reply.text()).toBe(REPLIES.nightIdleBad);
        }
        reply = await fetch(`${stub.url}night-idle`, { method: 'PUT', body: 'on\n' });
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe(REPLIES.nightOk);
        expect(await (await fetch(`${stub.url}night-idle`)).text()).toBe('on\n');
        await fetch(`${stub.url}night-schedule`, { method: 'PUT', body: '21:00-05:00 on' });
        expect(await (await fetch(`${stub.url}night-idle`)).text()).toBe('on\n');
    } finally {
        await stub.close();
    }
});

test('the stand-in answers /dynamic-range as the firmware does', async () => {
    const stub = await startStub();
    try {
        // A board that decodes no Dolby stream has nothing to choose.
        stub.device.dynamicRange = null;
        let reply = await fetch(`${stub.url}dynamic-range`);
        expect(reply.status).toBe(404);
        expect(await reply.text()).toBe(REPLIES.noDynamicRange);
        reply = await fetch(`${stub.url}dynamic-range`, { method: 'PUT', body: 'late-night' });
        expect(reply.status).toBe(409);
        expect(await reply.text()).toBe(REPLIES.noDynamicRange);
        // One that does starts on standard and takes the two words, with or without a line ending.
        stub.device.dynamicRange = 'standard';
        reply = await fetch(`${stub.url}dynamic-range`);
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe('standard\n');
        for (const bad of ['late night', 'Late-night', 'wide', '']) {
            reply = await fetch(`${stub.url}dynamic-range`, { method: 'PUT', body: bad });
            expect(reply.status).toBe(400);
            expect(await reply.text()).toBe(REPLIES.dynamicRangeBad);
        }
        reply = await fetch(`${stub.url}dynamic-range`, { method: 'PUT', body: 'late-night\n' });
        expect(reply.status).toBe(200);
        expect(await reply.text()).toBe(REPLIES.dynamicRangeOk);
        expect(await (await fetch(`${stub.url}dynamic-range`)).text()).toBe('late-night\n');
        reply = await fetch(`${stub.url}dynamic-range`, { method: 'PUT', body: 'standard' });
        expect(reply.status).toBe(200);
        expect(await (await fetch(`${stub.url}dynamic-range`)).text()).toBe('standard\n');
    } finally {
        await stub.close();
    }
});

// The preferences registry (libs/device/include/iclforge/preferences.hpp), which GET /preferences lists.
const PREFERENCES_HPP = fs.readFileSync(path.join(LIBS_DEVICE, 'preferences.hpp'), 'utf8');

test("the stand-in lists preferences with the registry's words", () => {
    const groups = {
        Display: 'kDisplay',
        Sound: 'kSound',
        Network: 'kNetwork',
        Time: 'kTime',
        Updates: 'kUpdates',
        System: 'kSystem',
    };
    for (const p of PREFERENCES) {
        // The entry, not a choice that shares its id (late night's choice of late-night): an entry names its group next.
        const found = new RegExp(String.raw`\{\.id = "${p.id}",\s*\.group`).exec(PREFERENCES_HPP);
        expect(found, `${p.id} is not a preference in preferences.hpp`).not.toBeNull();
        const start = found.index;
        const entry = PREFERENCES_HPP.slice(start, PREFERENCES_HPP.indexOf('.set = ', start));
        expect(entry).toContain(`.group = Group::${groups[p.group]}`);
        expect(entry).toContain(`.label = "${p.label}"`);
        expect(entry).toContain(`.kind = Kind::k${p.kind[0].toUpperCase()}${p.kind.slice(1)}`);
        expect(entry).toContain(`.route = "${p.route}"`);
        for (const [id, label] of p.choices) {
            expect(PREFERENCES_HPP, `${p.id}'s choice ${id}`).toContain(`{.id = "${id}", .label = "${label}"`);
        }
    }
    expect(PREFERENCES_HPP).toContain('This board has no screen.');
});

test('the stand-in lists the sound preferences with the player, and greyed with the reason without one', async () => {
    const stub = await startStub();
    try {
        let listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        let by = Object.fromEntries(listing.preferences.map((p) => [p.id, p]));
        // The board has a player (the stand-in's default): the tone is a button at rest, the trim a sheet that says what is set.
        expect(by['identify-speakers']).toMatchObject({
            group: 'Sound',
            kind: 'action',
            route: '/identify',
            value: 'run',
            available: true,
            reason: '',
        });
        expect(by['identify-speakers'].choices.map((c) => c.id)).toEqual(['run', 'running']);
        expect(by['speaker-trim']).toMatchObject({ group: 'Sound', kind: 'sheet', route: '/trim', value: 'none set', available: true });
        expect(by['speaker-trim'].choices).toBeUndefined();
        // A trim made through its own route is counted; a tone running says so.
        await fetch(`${stub.url}trim`, { method: 'PUT', body: 'L level=-1.5\nC distance=2.4\n' });
        await fetch(`${stub.url}identify`, { method: 'POST', body: 'L loop\n' });
        listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        by = Object.fromEntries(listing.preferences.map((p) => [p.id, p]));
        expect(by['speaker-trim'].value).toBe('2 speakers set');
        expect(by['identify-speakers'].value).toBe('running');
        // No player: both greyed, with the reason, and the sheet still has no choices.
        stub.device.sound = null;
        listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        for (const p of listing.preferences.filter((x) => x.group === 'Sound' && x.id !== 'late-night')) {
            expect(p.available, p.id).toBe(false);
            expect(p.reason, p.id).toBe('This build has no player, so it has no speakers to play a tone through.');
        }
        by = Object.fromEntries(listing.preferences.map((p) => [p.id, p]));
        expect(by['speaker-trim'].choices).toBeUndefined();
        for (const choice of by['identify-speakers'].choices) {
            expect(choice.available).toBe(false);
        }
    } finally {
        await stub.close();
    }
});

test('the stand-in lists late night with the value its route holds, and greyed without a Dolby decoder', async () => {
    const stub = await startStub();
    try {
        let listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        let entry = listing.preferences.find((p) => p.id === 'late-night');
        expect(entry).toMatchObject({
            group: 'Sound',
            label: 'Late night',
            kind: 'choice',
            apply: 'next-play',
            route: '/dynamic-range',
            value: 'standard',
            available: true,
            reason: '',
        });
        expect(entry.choices.map((c) => c.id)).toEqual(['standard', 'late-night']);
        // A change made through its own route shows.
        await fetch(`${stub.url}dynamic-range`, { method: 'PUT', body: 'late-night\n' });
        listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        expect(listing.preferences.find((p) => p.id === 'late-night').value).toBe('late-night');
        // A board that decodes no Dolby stream has the entry greyed, with the registry's reason, and no value.
        stub.device.dynamicRange = null;
        listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        entry = listing.preferences.find((p) => p.id === 'late-night');
        expect(entry).toMatchObject({ available: false, reason: 'This board decodes no Dolby stream: it has no Sendspin player.' });
        for (const choice of entry.choices) {
            expect(choice.available).toBe(false);
        }
        expect(PREFERENCES_HPP).toContain(entry.reason);
    } finally {
        await stub.close();
    }
});

test('the stand-in lists the preferences, greyed with the reason where the board has no screen', async () => {
    const stub = await startStub();
    try {
        let listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        expect(listing.display).toBe('none');
        expect(listing.preferences.map((p) => p.id)).toEqual([
            'now-view',
            'dim-after',
            'speakers-style',
            'brightness',
            'clock-hours',
            'clock-seconds',
            'night-schedule',
            'night-times',
            'night-idle',
            'identify-speakers',
            'speaker-trim',
            'late-night',
            'time-zone',
        ]);
        // The screen's entries are greyed, each with the reason; the time zone is the clock's, which this board keeps, and
        // the tone and the trim are the player's, which this one has.
        for (const p of listing.preferences.filter((q) => q.group === 'Display')) {
            expect(p.available, p.id).toBe(false);
            expect(p.reason, p.id).toBe('This board has no screen.');
            for (const choice of p.choices ?? []) {
                expect(choice.available).toBe(false);
                expect(choice.reason).toBe('This board has no screen.');
            }
        }
        expect(listing.preferences.at(-1)).toMatchObject({ id: 'time-zone', group: 'Time', available: true, value: 'none' });
        // One with a screen has them on, with their values; a change made through the entry's own route shows.
        stub.device.screen = screenBoard();
        listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        expect(listing.display).toBe('dsi');
        expect(listing.preferences[0]).toMatchObject({ id: 'now-view', value: 'speakers', available: true, reason: '' });
        expect(listing.preferences[1]).toMatchObject({ id: 'dim-after', value: '5', available: true, reason: '' });
        expect(listing.preferences.slice(2, 8).map((p) => [p.id, p.value, p.available])).toEqual([
            ['speakers-style', 'glow', true],
            ['brightness', '80', true],
            ['clock-hours', '24', true],
            ['clock-seconds', 'off', true],
            ['night-schedule', 'off', true],
            ['night-times', '22:00-07:00', true],
        ]);
        await fetch(`${stub.url}dim-after`, { method: 'PUT', body: '30\n' });
        await fetch(`${stub.url}speakers-style`, { method: 'PUT', body: 'pulse\n' });
        await fetch(`${stub.url}clock-format`, { method: 'PUT', body: '12 seconds' });
        await fetch(`${stub.url}night-schedule`, { method: 'PUT', body: '23:30-06:15' });
        await fetch(`${stub.url}timezone`, { method: 'PUT', body: 'Australia/Melbourne' });
        listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        const by = Object.fromEntries(listing.preferences.map((p) => [p.id, p]));
        expect(by['dim-after'].value).toBe('30');
        expect(by['speakers-style'].value).toBe('pulse');
        expect(by['clock-hours'].value).toBe('12');
        expect(by['clock-seconds'].value).toBe('on');
        expect(by['night-schedule'].value).toBe('on');
        expect(by['night-times'].value).toBe('23:30-06:15');
        expect(by['time-zone'].value).toBe('Australia/Melbourne');
        // A glass whose backlight cannot be dimmed greys the brightness with its reason, and only that.
        stub.device.screen.dimmable = false;
        listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        expect(listing.preferences[3]).toMatchObject({
            id: 'brightness',
            available: false,
            reason: "This screen's backlight is not under the board's control.",
        });
        expect(listing.preferences[4].available).toBe(true);
        // The night schedule says why it is moot where the screen without lyrics is dark already, or off.
        stub.device.screen.view = 'dark';
        listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        expect(listing.preferences.find((p) => p.id === 'night-schedule').note).toBe(
            'The Dark view is dark already: a touch lights it for 30 seconds, as at night.',
        );
        stub.device.screen.view = 'off';
        listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        expect(listing.preferences.find((p) => p.id === 'night-times').note).toBe('Not used while the screen without lyrics is Off.');
        // A board that keeps no time greys the zone, and the screen's clock entries, with the clock's reason.
        stub.device.time = null;
        listing = JSON.parse(await (await fetch(`${stub.url}preferences`)).text());
        const noClock = 'This board keeps no time: it has no network to ask a time server on.';
        expect(listing.preferences.find((p) => p.id === 'time-zone')).toMatchObject({ available: false, reason: noClock });
        expect(listing.preferences.find((p) => p.id === 'clock-hours')).toMatchObject({ available: false, reason: noClock });
        expect(listing.preferences.find((p) => p.id === 'dim-after').available).toBe(true);
    } finally {
        await stub.close();
    }
});

// Names the firmware takes and refuses: the same lists as libs/device/tests/test_device_name.cpp, which holds
// the rule itself (check_name), so that the stand-in's copy of it cannot drift from the board's.
const NAMES_TAKEN = ['hearth-a1b2c3', 'Sitting room', "Dad's study (TV) #2", 'K\u00fcche', 'a', 'x'.repeat(32), '\u00e9'.repeat(16)];
const NAMES_REFUSED = [
    'x'.repeat(33),
    '\u00e9'.repeat(17),
    ' Attic',
    'At\ttic',
    'At\ntic',
    'At\u0085tic',
    'At\u2028tic',
    'At\u202etic',
    'At\u200btic',
    'At\ufefftic',
];

test('the stand-in answers /name as the firmware does', async () => {
    const stub = await startStub();
    try {
        let reply = await fetch(`${stub.url}name`, { method: 'PUT', body: '' });
        expect(reply.status).toBe(400);
        expect(await reply.text()).toBe(REPLIES.nameEmpty);
        for (const name of NAMES_TAKEN) {
            reply = await fetch(`${stub.url}name`, { method: 'PUT', body: name });
            expect(reply.status, JSON.stringify(name)).toBe(200);
            expect(await reply.text()).toBe(REPLIES.ok);
            expect(await (await fetch(`${stub.url}name`)).text()).toBe(name + '\n');
        }
        // The line ending a shell puts after the name is not part of it.
        reply = await fetch(`${stub.url}name`, { method: 'PUT', body: 'Attic\n' });
        expect(reply.status).toBe(200);
        expect(await (await fetch(`${stub.url}name`)).text()).toBe('Attic\n');
        for (const name of NAMES_REFUSED) {
            reply = await fetch(`${stub.url}name`, { method: 'PUT', body: name });
            expect(reply.status, JSON.stringify(name)).toBe(409);
            expect(await reply.text()).toBe(REPLIES.nameRefused);
        }
        // A refused name leaves the name as it was.
        expect(await (await fetch(`${stub.url}name`)).text()).toBe('Attic\n');
    } finally {
        await stub.close();
    }
});

// A POST with a Host header of the test's choosing, which fetch does not let it set.
function post(stub, route, body, host) {
    const url = new URL(stub.url);
    return new Promise((resolve, reject) => {
        const request = require('http').request(
            { host: url.hostname, port: url.port, path: `/${route}`, method: 'POST', headers: host ? { Host: host } : {} },
            (res) => {
                const chunks = [];
                res.on('data', (chunk) => chunks.push(chunk));
                res.on('end', () => resolve({ status: res.statusCode, text: Buffer.concat(chunks).toString('utf8') }));
            },
        );
        request.on('error', reject);
        request.end(body);
    });
}

test('the stand-in answers /reset-preferences as the firmware does', async () => {
    const stub = await startStub();
    try {
        // A board with a screen, set to what a person chose, and named, and on a network.
        stub.device.screen = screenBoard();
        Object.assign(stub.device.screen, {
            view: 'clock',
            dimAfter: '30',
            speakersStyle: 'pulse',
            brightness: 40,
            clockFormat: '12 seconds',
            night: { enabled: true, from: '23:00', to: '06:00', idle: true },
        });
        stub.device.time.zone = 'JST-9';
        stub.device.name = 'Attic';
        stub.device.pairings = [pairedServer()];
        const reply = await post(stub, 'reset-preferences', '');
        expect(reply.status).toBe(200);
        expect(reply.text).toBe(REPLIES.resetPreferencesOk);
        // What a person chose is back; what the board is stays.
        expect(stub.device.screen.view).toBe('speakers');
        expect(stub.device.screen.dimAfter).toBe('5');
        expect(stub.device.screen.speakersStyle).toBe('glow');
        expect(stub.device.screen.brightness).toBe(80);
        expect(stub.device.screen.clockFormat).toBe('24');
        expect(stub.device.screen.night).toEqual({ enabled: false, from: '22:00', to: '07:00', idle: false });
        // Where the room is stays (the owner, 2026-10-25).
        expect(stub.device.time.zone).toBe('JST-9');
        expect(stub.device.name).toBe('Attic');
        expect(stub.device.ssid).toBe('kitchen');
        expect(stub.device.pairings).toHaveLength(1);
    } finally {
        await stub.close();
    }
});

test('the stand-in answers /factory-reset as the firmware does', async () => {
    const stub = await startStub();
    try {
        stub.device.screen = screenBoard();
        Object.assign(stub.device.screen, { view: 'clock' });
        stub.device.name = 'Attic';
        stub.device.pairings = [pairedServer()];
        // Only the words erase anything, and only said to the board by its own address or name.
        for (const wrong of ['', 'factory', 'erase everything please', 'ERASE EVERYTHING']) {
            const reply = await post(stub, 'factory-reset', wrong);
            expect(reply.status, JSON.stringify(wrong)).toBe(400);
            expect(reply.text).toBe(REPLIES.factoryResetBad);
        }
        let reply = await post(stub, 'factory-reset', 'erase everything', 'attacker.example');
        expect(reply.status).toBe(403);
        expect(reply.text).toBe(REPLIES.factoryResetHost);
        expect(stub.device.name).toBe('Attic');
        expect(stub.device.pairings).toHaveLength(1);
        // By the board's name, or its address, it erases: the name, the network, the pairings, the choices.
        reply = await post(stub, 'factory-reset', 'erase everything', 'attic.local');
        expect(reply.status).toBe(200);
        expect(reply.text).toBe(REPLIES.factoryResetOk);
        expect(stub.device.name).toBe('hearth-a1b2c3');
        expect(stub.device.ssid).toBe('');
        expect(stub.device.pairings).toHaveLength(0);
        expect(stub.device.screen.view).toBe('speakers');
        expect(stub.device.sendspin.client_id).not.toBe(idleSendspin().client_id);
    } finally {
        await stub.close();
    }
});
