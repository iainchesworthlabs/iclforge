'use strict';

// A stand-in for iclforge::Control (firmware/esp-idf/iclforge/src/control.cpp), for the
// web page's tests - planning/esp32-device-ui.md. It serves the page's two
// files with the headers the firmware sends and answers the REST routes with
// the firmware's status codes and reply texts, over a model of the streaming
// example's player (firmware/hearth-sink/main/
// hearth_sink.cpp): POST /play hands a location to the http source, which
// takes it only if it starts with http://; a play clears the last one's
// figures, runs for a few /status polls and finishes; a location that does not
// open leaves the state "failed" with no figures. contract.spec.js holds the
// replies, the headers and the routes to control.cpp's own.
//
// One per test, in the test's own process, on a port of its own: tests reach
// into it directly to script a reply, hold one back, or read what was sent.
//
// The firmware routes are iclforge::Firmware's (firmware/esp-idf/iclforge/src/
// firmware.cpp), modelled as far as the page uses them: an upload whose head
// is an application image is written, and the board restarts into it on
// trial; a restart, and a rollback. A restart is the next request dropped,
// as a board that is starting again does not answer.

const crypto = require('crypto');
const http = require('http');
const fs = require('fs');
const path = require('path');

const UI_DIR = path.resolve(__dirname, '../../../../../firmware/esp-idf/iclforge/ui');

const POLICY =
    "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src data:; frame-ancestors 'none'";

// control.cpp's reply texts, character for character.
const REPLIES = {
    api: [
        'iclforge player',
        'GET  /              a web page that shows and drives the player',
        'GET  /api           this list',
        'GET  /status        what is playing, as JSON',
        "GET  /hardware      what this board is - chip, revision, FPU, PSRAM, and this sink's own ceiling - as JSON, once at startup",
        'POST /play          body: a URL or path to play',
        'POST /stop',
        'POST /volume        body: 0.0 to 1.0',
        'GET  /layout        the output layout',
        'PUT  /layout        body: a name (5.1.4) or a speaker list; next play',
        "GET  /name          this board's name; PUT one to change it",
        'GET  /slot-width    16 or 32; PUT one to change it at the next play',
        'GET  /wiring        1 when a second I2S line is wired; PUT 1 or 0',
        'PUT  /network       body: an SSID, a newline, a passphrase; next boot',
        'GET  /network-preference  auto, ethernet or wifi on a board with a cable and WiFi; PUT one to change it',
        'GET  /now-playing   speakers, title, clock, off or dark on a board with a screen; PUT one to change it',
        'GET  /dim-after     never, or minutes until that screen dims, on a board with a screen; PUT one',
        "GET  /dynamic-range standard or late-night: how much of an AC-3, E-AC-3 or AC-4 stream's dynamic range is kept; PUT one to change it (from the next stream)",
        'GET  /timezone      the POSIX time zone the board\'s time is shown in, or none (UTC); PUT one',
        'GET  /ntp-server    the time server a person named, or none (the router\'s, then the pool); PUT one',
        'GET  /time-sync     on or off: whether the board keeps its time with a time server; PUT one',
        'GET  /pairing       the servers this board is paired with, as JSON',
        'POST /pairing       body: reset, cancel, forget, or forget and a server_id (Sendspin pairing)',
        'GET  /firmware      both app slots, a trial, an upload and the last update, as JSON',
        'PUT  /firmware      body: an app image; flash mode, then a restart into it',
        'PUT  /firmware/mode body: flash, or normal (a restart)',
        'PUT  /firmware/rollback  the image before this one boots next',
        "GET  /firmware/coredump  the last crash's core dump, as it lies in flash",
        'DELETE /firmware/coredump  erase it',
        'POST /restart       restart into the image that runs now',
        "POST /reset-preferences  the screen's choices back to the defaults; the name, network, wiring and pairings stay",
        'POST /factory-reset body: erase everything; erases the name, network, pairings and every setting, then restarts',
        'GET  /log           recent console output; ?from=N for what came after byte N',
        '',
    ].join('\n'),
    playEmpty: 'POST /play wants the location as the body\n',
    playRefused: 'this source cannot play that\n',
    accepted: 'accepted\n',
    stopped: 'stopped\n',
    volumeBad: 'POST /volume wants a number 0.0 to 1.0\n',
    volumeRefused: 'this sink has no volume to set\n',
    ok: 'ok\n',
    layoutNone: 'this player has no layout to report\n',
    layoutEmpty: 'PUT /layout wants a name (5.1.4) or a speaker list (L,R,C,LFE,Ls,Rs)\n',
    layoutFixed: "this player's layout is fixed\n",
    layoutRefused:
        'not a layout this player can play: check the name or the list, and that it has no more slots than the sink\n',
    layoutOk: 'ok; takes effect at the next play\n',
    nameEmpty: 'PUT /name wants a name\n',
    nameRefused:
        'not a name this player can hold: 1 to 32 bytes of text, with no control character in it and no space at either end, or it could not be stored\n',
    slotWidthBad: 'PUT /slot-width wants a number of bits\n',
    slotWidthRefused:
        "not a slot width this player's sink has: 16 or 32 on an I2S bus, and a sink with no hardware behind it keeps the one it was built for\n",
    wiringBad: 'PUT /wiring wants 1 (a second I2S line is wired) or 0\n',
    wiringRefused: 'the wiring could not be stored, or a play is running\n',
    wiringFixed: "this player's wiring is fixed\n",
    wiringNone: 'this player reports no wiring\n',
    networkEmpty: 'PUT /network wants an SSID, a newline, and a passphrase\n',
    networkPreferenceBad: 'PUT /network-preference wants auto, ethernet or wifi\n',
    networkPreferenceNone: 'this player has one network, so no preference between networks\n',
    networkPreferenceRefused:
        'that preference could not be set: wifi needs a WiFi network stored, and the board may be busy joining one\n',
    networkPreferenceOk: 'ok; takes effect in a moment, and is not kept if the interface it names does not come up\n',
    noScreen: 'this board has no screen\n',
    nowPlayingBad: 'PUT /now-playing wants speakers, title, clock, off or dark\n',
    nowPlayingRefused: 'that view is not in this build, or the setting could not be stored\n',
    nowPlayingOk: 'ok; the screen follows at once\n',
    dimAfterBad: 'PUT /dim-after wants never, or a number of minutes from 1 to 240\n',
    dimAfterRefused: 'that time could not be stored\n',
    dimAfterOk: 'ok; takes effect at once\n',
    noDynamicRange: 'this board decodes no Dolby stream, so it has no dynamic range to choose\n',
    dynamicRangeBad: 'PUT /dynamic-range wants standard or late-night\n',
    dynamicRangeRefused: 'that setting could not be stored\n',
    dynamicRangeOk: 'ok; takes effect from the next stream\n',
    noClock: 'this board keeps no time\n',
    timeZoneBad:
        'PUT /timezone wants a POSIX time zone with its daylight saving rules, such as AEST-10AEDT,M10.1.0,M4.1.0/3 (the offset is hours WEST of UTC), or none\n',
    timeZoneRefused:
        "that zone could not be taken: this board's C library reads it differently, or it could not be stored\n",
    timeZoneOk: 'ok; the clock follows at once\n',
    ntpServerBad:
        'PUT /ntp-server wants a host name or an IPv4 address, such as pool.ntp.org or 192.168.1.5, or none\n',
    ntpServerRefused: 'that server could not be stored\n',
    ntpServerOk: 'ok; the board asks it at once\n',
    timeSyncBad: 'PUT /time-sync wants on or off\n',
    timeSyncRefused: 'that setting could not be stored\n',
    timeSyncOk: 'ok; takes effect at once\n',
    nextPlay: 'ok; takes effect at the next play\n',
    nextBoot: 'ok; takes effect at the next boot\n',
    pairingBad: 'POST /pairing wants reset, cancel, forget, or forget and a server_id\n',
    pairingUnknown: 'this board has no pairing with that server\n',
    pairingRefused: 'this board is not a Sendspin player\n',
    resetPreferencesNone: 'this board keeps no preferences to reset\n',
    resetPreferencesOk:
        'ok; the preferences are back to the defaults, and the name, the network, the wiring and the pairings are kept\n',
    factoryResetHost: "a factory reset is taken only on the board's own address or name\n",
    factoryResetBad: 'POST /factory-reset wants the body: erase everything\n',
    factoryResetFixed: 'this board cannot be reset to its factory state\n',
    factoryResetOk: 'erased; the board restarts with no name, network or pairings of its own\n',
    noFirmware: 'this board takes no firmware updates\n',
    noLog: 'this board keeps no log\n',
};

// firmware.cpp's reply texts, and firmware_image.hpp's for an image it
// refuses: the literals, each with the newline reply_text adds. Replies built
// around a number or a version are made in the model from the literals in
// FIRMWARE_PARTS.
const FIRMWARE = {
    oneSlot: "this board's partition table has one app slot: move it to the two-slot table over USB once\n",
    noLength: 'PUT /firmware wants the image as the body, with its length\n',
    short: 'that body is too short to be an application image\n',
    type: 'PUT /firmware wants the image as application/octet-stream, or no Content-Type at all\n',
    onTrial: 'the running image is still on trial; wait until it is accepted (GET /firmware)\n',
    busy: 'an update is already under way\n',
    underWay: 'an update is under way\n',
    notImage:
        'this is not an ESP-IDF application image (its first byte is not 0xE9); send iclforge_hearth_sink.bin, not the merged factory image or the ELF\n',
    modeBad: 'PUT /firmware/mode wants flash or normal\n',
    flashMode: 'flash mode: nothing plays until the board restarts\n',
    notFlash: 'not in flash mode\n',
    leaving: 'restarting into the image that runs now\n',
    givingUp: "giving up this image's trial; going back\n",
    nothingOneSlot: 'this board has one app slot, so there is nothing to go back to\n',
    restartOnTrial:
        'the running image is still on trial, and a restart now goes back to the image before it; to do that, PUT /firmware/rollback\n',
    restarting: 'restarting\n',
    noCoredump: 'there is no core dump: nothing has crashed since the last was erased\n',
    erased: 'erased\n',
};
const FIRMWARE_PARTS = {
    tooBig: ['that image is ', ' bytes, and the slot holds '],
    goingBack: ['going back to ', '; the board restarts into it, on trial'],
    noImage: ['there is no image to go back to in ', ': it failed its trial or does not check out', ': it is empty'],
};

const ROUTES = [
    'GET /',
    'GET /ui.js',
    'GET /api',
    'GET /status',
    'GET /hardware',
    'POST /play',
    'POST /stop',
    'POST /volume',
    'GET /layout',
    'PUT /layout',
    'GET /name',
    'PUT /name',
    'GET /wiring',
    'PUT /wiring',
    'GET /slot-width',
    'PUT /slot-width',
    'PUT /network',
    'GET /network-preference',
    'PUT /network-preference',
    'GET /now-playing',
    'PUT /now-playing',
    'GET /dim-after',
    'PUT /dim-after',
    'GET /dynamic-range',
    'PUT /dynamic-range',
    'GET /timezone',
    'PUT /timezone',
    'GET /ntp-server',
    'PUT /ntp-server',
    'GET /time-sync',
    'PUT /time-sync',
    'GET /pairing',
    'POST /pairing',
    'GET /firmware',
    'PUT /firmware',
    'PUT /firmware/mode',
    'PUT /firmware/rollback',
    'GET /firmware/coredump',
    'DELETE /firmware/coredump',
    'POST /restart',
    'POST /reset-preferences',
    'POST /factory-reset',
    'GET /log',
];

// The streams the model plays, with the channels each codes. The E-AC-3 one is
// the WASM page's demo, as CI's HTTP step plays it; the AC-3 one is the
// example's own sample; the 7.1.4 one is the stream set's walk
// (firmware/hearth-sink/www/).
const STREAMS = {
    eac3: { codec: 'E-AC-3', acmod: 7, channels: 6, substreams: 1, dialnorm: -31, objects: true, coded: 'L,C,R,Ls,Rs,LFE' },
    ac3: { codec: 'AC-3', acmod: 7, channels: 6, substreams: 1, dialnorm: -31, objects: false, coded: 'L,C,R,Ls,Rs,LFE' },
    eac3_714: {
        codec: 'E-AC-3', acmod: 7, channels: 12, substreams: 3, dialnorm: -31, objects: false,
        coded: 'L,C,R,Ls,Rs,Lrs,Rrs,Vhl,Vhr,Lts,Rts,LFE',
    },
};

// Table E2.5's location names as the firmware writes them, and the other
// tokens OutputLayout's list takes (libs/render/include/iclforge/render/layout.hpp).
const LOCATIONS = 'L C R Ls Rs Lc Rc Lrs Rrs Cs Ts Lsd Rsd Lw Rw Vhl Vhr Vhc Lts Rts LFE LFE2'.split(' ');
const TOKENS = new Set([...LOCATIONS.map((n) => n.toLowerCase()), '-']);

// OutputLayout's names: F.L.H, the slots ring, heights, LFE.
const RING = { 1: 'C', 2: 'L R', 3: 'L C R', 4: 'L R Ls Rs', 5: 'L C R Ls Rs', 7: 'L C R Ls Rs Lrs Rrs', 9: 'L C R Ls Rs Lrs Rrs Lw Rw' };
const HEIGHTS = { 0: '', 2: 'Vhl Vhr', 4: 'Vhl Vhr Lts Rts', 6: 'Vhl Vhr Vhc Lts Rts Ts' };
const FEEDS = { 0: '', 1: 'LFE', 2: 'LFE LFE2' };

// A layout's slots by the names the firmware gives them ("-" for an empty
// one), or null if OutputLayout would not parse it.
function speakersOf(text) {
    const name = /^(\d)\.(\d)(?:\.(\d))?$/.exec(text);
    if (name) {
        const parts = [RING[name[1]], HEIGHTS[name[3] || 0], FEEDS[name[2]]];
        return parts.includes(undefined) ? null : parts.join(' ').split(' ').filter(Boolean);
    }
    const tokens = text.split(',').map((t) => t.trim());
    const known = (t) => TOKENS.has(t.toLowerCase()) || /^-?\d+(\.\d+)?\/-?\d+(\.\d+)?$/.test(t);
    if (tokens.length > 16 || !tokens.every(known)) {
        return null;
    }
    return tokens.map((t) => LOCATIONS.find((n) => n.toLowerCase() === t.toLowerCase()) || t);
}

// How many slots a layout needs, or 0 if OutputLayout would not parse it.
function slotsOf(text) {
    const speakers = speakersOf(text);
    return speakers ? speakers.length : 0;
}

// How the player serves a layout (planning/esp32-device-ui.md, "The output
// layout"): the decoder's fold for two full-range speakers or one and nothing
// else; objects placed when the layout has heights and the stream objects;
// the coded channels placed otherwise, with the speakers they do not reach
// left silent. A channel with no slot of its own is spread, which the model
// does not work out: it then reports nothing silent.
function served(kind, layout) {
    const speakers = (speakersOf(layout) || []).filter((n) => n !== '-');
    const lfe = speakers.filter((n) => n.startsWith('LFE'));
    const height = speakers.some((n) => /^(Vh|Lts|Rts|Ts)/.test(n) || /\/[1-9]/.test(n));
    if (lfe.length === 0 && !height && speakers.length <= 2) {
        return { render: speakers.length === 1 ? 'mono' : 'loro', silent: '' };
    }
    if (kind.objects && height) {
        return { render: 'objects', silent: '' };
    }
    const coded = kind.coded.split(',');
    const spread = coded.some((n) => !speakers.includes(n) && !n.startsWith('LFE'));
    return { render: 'channels', silent: spread ? '' : speakers.filter((n) => !coded.includes(n)).join(',') };
}

function idleStats() {
    return {
        frames: 0,
        held: 0,
        us_per_frame: 0,
        worst_frame_us: 0,
        render_us_per_frame: 0,
        sink_us_per_frame: 0,
        realtime_permille: 0,
        resync_bytes: 0,
        fetched_bytes: 0,
        ring_low: null,
        passes: 0,
        layout_mismatches: 0,
        finished: false,
        failed: false,
        why: '',
        error: 0,
    };
}

// The Sendspin player's part of /status with no server connected, as
// append_sendspin writes it (firmware/esp-idf/iclforge/src/control.cpp).
function idleSendspin() {
    return {
        server: '',
        server_id: '',
        dialect: '',
        psk: '',
        activity: '',
        role: '',
        clock_converged: false,
        clock_error_us: 0,
        clock_updates: 0,
        clock_rejected: 0,
        connections: 0,
        client_id: 'gS3cmMlDUaQGhxYd0PF0x0jWR2OGdhxwUwBBwyD3O1c',
        paired: 0,
        pairing_code: '',
        pairing_held: false,
        pairing_rounds: 0,
        pairing_outcome: '',
        lost_pairing: false,
        playing: 'idle',
        bursts: 0,
        underruns: 0,
        late: 0,
        dropped: 0,
        invalid: 0,
        resyncs: 0,
        error_us: 0,
        worst_error_us: 0,
        play_frame: null,
        play_server_us: null,
        origin_server_us: null,
        peak_db: [],
        rms_db: [],
        stream_rms: [],
        burst_us: 0,
        worst_burst_us: 0,
        decode_stack_free: 0,
        server_stack_free: 0,
        settings_revision: 0,
        identifying: false,
    };
}

// GET /hardware's board, as append_hardware/build_hardware_json would write
// it for an ESP32-S3 with no PSRAM fitted (the shipped Sendspin sink's own
// default shape) - static for the run, unlike everything above.
function defaultHardware() {
    return {
        target: 'esp32s3',
        chip: 'ESP32-S3',
        revision: '0.2',
        cores: 2,
        fpu: true,
        cpu_freq_mhz: 240,
        psram_bytes: 0,
        sink_max_slots: 8,
        sink_max_slots_bits: 16,
        project: 'iclforge_hearth_sink',
        version: 'v0.10.0-beta.1-42-gee9cf4f',
        idf_version: 'v6.1',
        // No PSRAM and eight 16-bit slots is the shape of none of the locked profiles
        // (firmware/hearth-sink/profiles.json), and an image not built as one reports both empty.
        profile: '',
        profile_hash: '',
        capabilities: [
            '2 cores, a hardware floating-point unit',
            'Running at 240 MHz',
            "This sink's bus reaches up to 8 slots at 16-bit",
        ],
        notices: ['No PSRAM in this build: a wide buffer ring, deep DMA queues or an object reconstruction buffer fall back to internal RAM, or may not fit at all.'],
    };
}

// A paired server playing a 5.1 stream to the board in bursts.
function playingSendspin() {
    return {
        ...idleSendspin(),
        server: 'Hearth on the desk',
        server_id: 'Yx3kP0aZ',
        dialect: 'specification',
        psk: 'long-term',
        activity: 'playback',
        role: '_iclforge_player@v1',
        clock_converged: true,
        clock_error_us: 310,
        connections: 1,
        paired: 1,
        playing: 'bursts',
        bursts: 1875,
        error_us: -42,
        worst_error_us: 180,
        play_frame: 2880000,
        play_server_us: 1726500060000000,
        origin_server_us: 1726500000000000,
        peak_db: [-3.1, -3.4, -8.9, -16.2, -12.5, -120],
        rms_db: [-18.2, -18.6, -21, -30.4, -26.1, -120],
        stream_rms: [123027, 117490, 89125, 30200, 49545, 0],
        burst_us: 11850,
        worst_burst_us: 19420,
        decode_stack_free: 5120,
        server_stack_free: 2210,
        settings_revision: 3,
    };
}

// The board on a WiFi network, as the example's network_link() and
// network_address() report it (firmware/hearth-sink/main/
// network.hpp).
function wifiNetwork() {
    // preference and interfaces are null on a board with one network.
    return { kind: 'wifi', ssid: 'kitchen', rssi_dbm: -58, address: '192.168.1.23', preference: null, interfaces: null };
}

// The same board with a cable as well: network_link() on net/dual, which says which of the two it
// uses and how each stands. The cable is in and holds an address, so it is the one in use.
function cableNetwork(preference = 'auto') {
    return {
        kind: 'ethernet',
        ssid: '',
        rssi_dbm: null,
        address: '192.168.1.24',
        preference,
        interfaces: {
            ethernet: {
                enabled: preference !== 'wifi',
                link: true,
                address: '192.168.1.24',
                mac: 'e8:f6:0a:e7:53:fb',
                speed_mbps: 100,
                full_duplex: true,
            },
            wifi: {
                enabled: preference !== 'ethernet',
                link: true,
                address: '192.168.1.23',
                mac: 'e8:f6:0a:e7:53:fa',
                configured: true,
                ssid: 'kitchen',
                rssi_dbm: -58,
            },
        },
    };
}

// The panel of a board with a screen (firmware/hearth-sink/main/panel/): what it shows while a song has
// no lyrics, which of the four views this build has, and when it dims, in the words GET /now-playing and GET
// /dim-after use. The title card is not built yet, so the build has three of the four views.
function screenBoard() {
    return { view: 'speakers', views: ['speakers', 'clock', 'off'], dimAfter: '5' };
}

// The time of a board that keeps it (firmware/hearth-sink/main/clock/sntp, every board with a network): in the
// words GET /timezone, /ntp-server and /time-sync use, and the numbers GET /status's "time" has. `zoneRefused`
// is a board whose C library reads a zone differently from the grammar, which the route says with a 409.
function timeBoard() {
    return {
        sync: 'on',
        zone: 'none',
        server: 'none',
        asking: true,
        synced: true,
        local: '2026-10-21 08:09:44',
        servers: '192.168.1.5 (from DHCP), pool.ntp.org',
        answers: 1,
        zoneRefused: false,
    };
}

// Whether a string is a time server the firmware takes: libs/device's wall_clock.hpp valid_ntp_server, rule for
// rule: an IPv4 address (four parts, no leading zero) or a host name (RFC 1123 labels, the last not all digits),
// at most 63 bytes.
function validNtpServer(text) {
    if (text.length === 0 || text.length > 63) {
        return false;
    }
    if (/^[0-9.]+$/.test(text)) {
        const parts = text.split('.');
        return parts.length === 4 && parts.every((p) => /^(0|[1-9][0-9]{0,2})$/.test(p) && Number(p) <= 255);
    }
    const labels = text.split('.');
    if (!labels.every((l) => /^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/.test(l))) {
        return false;
    }
    return !/^[0-9]+$/.test(labels[labels.length - 1]);
}

// Whether a string is a POSIX time zone the firmware takes: libs/device's wall_clock.hpp, rule for rule.
// `std offset [dst [offset] ,start[/time],end[/time]]`; a zone with daylight saving MUST give its rules, and
// nothing but printable ASCII is read.
function validTimeZone(text) {
    if (text.length === 0 || text.length > 63 || /[^!-~]/.test(text)) {
        return false;
    }
    let at = 0;
    const peek = () => (at < text.length ? text[at] : '');
    const take = (c) => {
        if (peek() === c) {
            at += 1;
            return true;
        }
        return false;
    };
    const digit = (c) => c >= '0' && c <= '9';
    const alpha = (c) => /^[A-Za-z]$/.test(c);
    const number = (least, most) => {
        let value = 0;
        let count = 0;
        while (digit(peek()) && count < most) {
            value = value * 10 + (peek().charCodeAt(0) - 48);
            at += 1;
            count += 1;
        }
        return count < least ? null : value;
    };
    const name = () => {
        const start = at;
        if (take('<')) {
            while (at < text.length && peek() !== '>') {
                if (!(alpha(peek()) || digit(peek()) || peek() === '+' || peek() === '-')) {
                    return false;
                }
                at += 1;
            }
            if (!take('>')) {
                return false;
            }
            const length = at - start - 2;
            return length >= 3 && length <= 16;
        }
        while (alpha(peek())) {
            at += 1;
        }
        const length = at - start;
        return length >= 3 && length <= 16;
    };
    const hms = (maxHours) => {
        if (!take('-')) {
            take('+');
        }
        const hours = number(1, maxHours >= 100 ? 3 : 2);
        if (hours === null || hours > maxHours) {
            return false;
        }
        if (take(':')) {
            const minutes = number(2, 2);
            if (minutes === null || minutes > 59) {
                return false;
            }
            if (take(':')) {
                const seconds = number(2, 2);
                if (seconds === null || seconds > 59) {
                    return false;
                }
            }
        }
        return true;
    };
    const rule = () => {
        if (take('J')) {
            const day = number(1, 3);
            if (day === null || day < 1 || day > 365) {
                return false;
            }
        } else if (take('M')) {
            const month = number(1, 2);
            if (month === null || month < 1 || month > 12 || !take('.')) {
                return false;
            }
            const week = number(1, 1);
            if (week === null || week < 1 || week > 5 || !take('.')) {
                return false;
            }
            const weekday = number(1, 1);
            if (weekday === null || weekday > 6) {
                return false;
            }
        } else {
            const day = number(1, 3);
            if (day === null || day > 365) {
                return false;
            }
        }
        return take('/') ? hms(167) : true;
    };
    if (!name() || !hms(24)) {
        return false;
    }
    if (at === text.length) {
        return true;
    }
    if (!name()) {
        return false;
    }
    if (peek() !== ',' && !hms(24)) {
        return false;
    }
    return take(',') && rule() && take(',') && rule() && at === text.length;
}

// One of GET /pairing's servers, as on_pairing_get writes it
// (firmware/esp-idf/iclforge/src/control.cpp): the same keys in the same order.
function pairedServer(fields) {
    return { server_id: '', name: '', connected: false, last_playback: false, seen: true, ...fields };
}

// One of GET /firmware's slots, as append_slot writes it
// (libs/device/include/iclforge/firmware_status.hpp).
function firmwareSlot(fields) {
    return {
        label: 'ota_0',
        state: 'valid',
        version: 'v0.10.0-beta.1-42-gee9cf4f',
        project: 'iclforge_hearth_sink',
        idf_version: 'v6.1',
        elf_sha256: '2366bde995250290d1f5a8c3b7e4f09a61c2d8e3b5f7a9c1d3e5f7a9b1c3d5e7',
        image_sha256: '592201f1a71d62fe0b8c6d4e2f1a3b5c7d9e1f3a5b7c9d1e3f5a7b9c1d3e5f70',
        intact: true,
        ...fields,
    };
}

// A slot with no image: its label and state, and nothing else (report_slot).
function emptySlot(label) {
    return { label, state: 'empty', version: '', project: '', idf_version: '', elf_sha256: '', image_sha256: '', intact: null };
}

// A trial as status() reports it, just begun: a Hearth sink's conditions,
// the Kconfig defaults' hold and deadline.
function firmwareTrial(fields) {
    return {
        healthy_for_ms: 0,
        hold_ms: 30000,
        remaining_ms: 300000,
        waiting_for: ['a network address', 'the Sendspin player'],
        ...fields,
    };
}

// GET /firmware as render_firmware_status writes it, every key always there:
// a board on the two-slot table (partitions.csv) running the image a USB
// flash put in ota_0, with the other slot empty.
function defaultFirmware() {
    const table = [
        ['nvs', 1, 2, 0x9000, 0x6000],
        ['phy_init', 1, 1, 0xf000, 0x1000],
        ['otadata', 1, 0, 0x10000, 0x2000],
        ['ota_0', 0, 16, 0x20000, 0x400000],
        ['ota_1', 0, 17, 0x420000, 0x400000],
        ['coredump', 1, 3, 0x820000, 0x10000],
        ['audio', 1, 0x40, 0x830000, 0x40000],
        ['storage', 1, 0x81, 0x870000, 0x40000],
        ['reserve', 1, 0x41, 0x8b0000, 0x400000],
    ];
    return {
        mode: 'normal',
        running: firmwareSlot({}),
        other: emptySlot('ota_1'),
        trial: null,
        upload: null,
        last_update: null,
        coredump: null,
        network: 'stored',
        slot_bytes: 0x400000,
        flash_bytes: 16 * 1024 * 1024,
        partitions: table.map(([label, type, subtype, offset, size]) => ({ label, type, subtype, offset, size })),
        bootloader_version: 'v6.1',
    };
}

// The head of an application image, as parse_image_head reads it
// (firmware_image.hpp): its first byte, its chip ID, and its app
// description's magic word, version and project. `size` bytes in all.
function appImage({ version = 'v0.11.0', project = 'iclforge_hearth_sink', chip = 9, size = 4096, magic = 0xe9 } = {}) {
    const image = Buffer.alloc(size);
    image[0] = magic;
    image[1] = 4; // segments
    image.writeUInt16LE(chip, 12);
    image[23] = 1; // a SHA-256 appended
    image.writeUInt32LE(0xabcd5432, 32);
    image.write(version, 48, 32, 'latin1');
    image.write(project, 80, 32, 'latin1');
    image.write('v6.1', 144, 32, 'latin1');
    return image;
}

// GET /status as control.cpp writes it: the same keys in the same order, the
// volume to three places, and a newline at the end. `second_line` is left out
// for a sink with no second line to wire, as the example leaves it.
// GET /status's "time" object, as on_status writes it.
function timeStatus(t) {
    return {
        sync: t.sync === 'on',
        asking: t.asking,
        synced: t.synced,
        local: t.synced ? t.local : null,
        zone: t.zone,
        server: t.server,
        servers: t.servers,
        answers: t.answers,
    };
}

function statusJson(d) {
    const stream = d.player ? d.player.stream : d.lastStream;
    const stats = d.player ? d.player.stats : d.lastStats;
    const fields = [
        ['state', JSON.stringify(d.state)],
        ['location', JSON.stringify(d.location)],
        ['source', JSON.stringify(d.source)],
        ['sink', JSON.stringify(d.sink)],
        ['sink_slots', String(d.sinkSlots)],
        ['slot_bits', String(d.slotBits)],
        ...(d.secondLine === undefined ? [] : [['second_line', d.secondLine ? 'true' : 'false']]),
        ['name', JSON.stringify(d.name)],
        ['layout', JSON.stringify(d.layout)],
        ['volume', d.volume.toFixed(3)],
        ['stream', stream ? JSON.stringify(stream) : 'null'],
    ];
    for (const [key, value] of Object.entries(stats)) {
        fields.push([key, JSON.stringify(value)]);
    }
    if (d.sendspin !== undefined) {
        fields.push(['sendspin', JSON.stringify(d.sendspin)]);
    }
    if (d.network !== undefined) {
        fields.push(['network', JSON.stringify(d.network)]);
    }
    if (d.time !== undefined) {
        fields.push(['time', d.time === null ? 'null' : JSON.stringify(timeStatus(d.time))]);
    }
    return '{' + fields.map(([k, v]) => JSON.stringify(k) + ':' + v).join(',') + '}\n';
}

// Whether a string is a name the firmware takes: libs/device's device_name.hpp check_name, rule for rule. 1 to 32
// bytes of well-formed text (the stand-in sees U+FFFD where the bytes were not), with no control character, line
// separator, bidirectional control, zero-width mark or byte order mark in it, and no space at either end.
function validName(text) {
    if (text.length === 0 || Buffer.byteLength(text, 'utf8') > 32) {
        return false;
    }
    if (text.startsWith(' ') || text.endsWith(' ')) {
        return false;
    }
    return !/[\u0000-\u001f\u007f-\u009f\u2028\u2029\u200b-\u200f\u202a-\u202e\u2066-\u2069\ufeff\ufffd]/.test(text);
}

// What the board answers to by name: its mDNS host name (devname::mdns_label, for the ASCII the stand-in's names
// have), and the two loopback names the stand-in itself is reached at.
function hostNames(name) {
    const label = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'hearth';
    return ['127.0.0.1', 'localhost', label + '.local'];
}

async function startStub() {
    const device = {
        state: 'stopped',
        location: 'http://10.0.2.2:8000/demo.ec3',
        source: 'http',
        sink: 'capture-i2s',
        sinkSlots: 2,
        slotBits: 32,
        // Whether a second I2S line is wired; undefined for a part that can
        // have none (an ESP32-C6), whose /status leaves it out.
        secondLine: false,
        name: 'hearth-a1b2c3',
        // Stored rather than reported: /status does not carry a network, and
        // a passphrase should not travel back out of a device at all.
        ssid: 'kitchen',
        password: '',
        layout: '2.0',
        volume: 1,
        // A Sendspin player's firmware (sdkconfig.sendspin). Undefined for a
        // firmware with no player, whose /status has no "sendspin" key, and
        // null for one whose player did not start.
        sendspin: idleSendspin(),
        // GET /pairing's servers, the most recently used first (pairedServer).
        // A test that sets sendspin.paired sets these to agree when the list
        // matters to it.
        pairings: [],
        // What /status says the board is joined to: undefined for a firmware
        // that does not report it, null for a build with no network.
        network: wifiNetwork(),
        // A board with a screen (a test sets screenBoard()): the view it shows while a song has no
        // lyrics, the views this build has, and when it dims. Null for a board with none.
        screen: null,
        // How much of a Dolby stream's dynamic range the listener wants: 'standard' or 'late-night' on a board
        // with a Sendspin player, which decodes for a listener; null for one that decodes none, whose routes
        // answer 404.
        dynamicRange: 'standard',
        // What the board says about its time (timeBoard); undefined for a firmware that keeps none (no
        // network, or QEMU's), whose /status has no "time" key and whose time routes answer 404.
        time: timeBoard(),
        hardware: defaultHardware(),
        // GET /firmware's body (defaultFirmware), or undefined for a firmware
        // that takes no updates.
        firmware: defaultFirmware(),
        // What GET /firmware/coredump sends while firmware.coredump says
        // there is a core dump.
        coredumpBytes: Buffer.alloc(0),
        // Requests left to drop while the board restarts; the page and its
        // script are not counted, so that a page loaded then still loads.
        down: 0,
        restartDrops: 1,
        player: null, // the play in progress: {stream, stats, total, fails}
        lastStats: idleStats(), // the last play that ended by itself, until another begins
        lastStream: null,
        framesPerPoll: 50,
    };
    const requests = [];
    const scripted = new Map(); // route -> replies to give before the model's
    const holding = new Map(); // route -> answers waiting for release()
    let payload = null; // a fixed GET /status body, instead of the model's
    let statusInFlight = 0;
    let statusInFlightMost = 0;
    // Drops scripted and not yet made. Until they are, and while the board is
    // restarting, every answer closes its connection: a request that meets a
    // drop then meets it on a connection Chromium did not reuse, and is not
    // sent again (see next()).
    let dropsPending = 0;
    const closing = (res) => {
        if (dropsPending > 0 || device.down > 0) {
            res.setHeader('Connection', 'close');
        }
    };

    const send = (res, code, body, type = 'text/plain') => {
        closing(res);
        res.writeHead(code, { 'Content-Type': type });
        res.end(body);
    };

    const file = (res, name, type) => {
        closing(res);
        res.writeHead(200, {
            'Content-Type': type,
            'Cache-Control': 'no-cache',
            'Content-Security-Policy': POLICY,
        });
        res.end(fs.readFileSync(path.join(UI_DIR, name)));
    };

    // A play moves on at each poll, the way a player on its own tasks would
    // between two of them.
    function advance() {
        const p = device.player;
        if (!p || device.state !== 'playing') {
            return;
        }
        const s = p.stats;
        s.frames = Math.min(p.total, s.frames + device.framesPerPoll);
        Object.assign(s, {
            us_per_frame: 4075,
            worst_frame_us: 46299,
            render_us_per_frame: 112,
            sink_us_per_frame: 252,
            realtime_permille: 127,
            fetched_bytes: s.frames * 1792,
            ring_low: 6144,
        });
        const k = p.kind;
        p.stream = {
            codec: k.codec, acmod: k.acmod, channels: k.channels, substreams: k.substreams, dialnorm: k.dialnorm,
            objects: k.objects, objects_rendered: p.served.render === 'objects', slots: p.slots,
            layout: p.layout, render: p.served.render, coded: k.coded, silent: p.served.silent,
        };
        if (p.fails && s.frames >= p.total / 2) {
            Object.assign(s, { finished: true, failed: true, why: 'decode', error: 5 });
        } else if (s.frames >= p.total) {
            Object.assign(s, { finished: true, passes: 1, why: 'end of stream' });
        } else {
            return;
        }
        device.lastStats = s;
        device.lastStream = p.stream;
        device.player = null;
        device.state = s.failed ? 'failed' : 'finished';
    }

    // What app_main does with a location from the queue: end the play, then
    // hand the location to the source, which takes it or refuses it.
    function play(location) {
        device.player = null;
        if (!location.startsWith('http://') || location.length >= 256) {
            device.state = 'stopped';
            return;
        }
        device.location = location;
        // begin_play: the last play's figures go before the source opens.
        // Here it opens at once, so "opening" never shows; the firmware
        // reports it while the open takes, and rendering.spec.js covers it.
        device.lastStats = idleStats();
        device.lastStream = null;
        if (location.includes('unreachable')) {
            device.state = 'failed';
            return;
        }
        const kind = location.endsWith('.ac3') ? STREAMS.ac3 : location.includes('714') ? STREAMS.eac3_714 : STREAMS.eac3;
        device.player = {
            kind,
            layout: device.layout,
            served: served(kind, device.layout),
            slots: slotsOf(device.layout),
            stream: null,
            stats: idleStats(),
            total: 250,
            fails: location.includes('broken'),
        };
        device.state = 'playing';
    }

    // A restart: the board stops answering for `restartDrops` requests and
    // comes back as `then` leaves it, nothing playing. The idle connections
    // close first, as the board's do: Chromium sends a request again when a
    // connection it reused closes unanswered, and a board back by then never
    // seems to have gone, which `restartDrops = 0` models.
    function restart(then) {
        then();
        device.player = null;
        device.state = 'stopped';
        device.hardware.version = device.firmware.running.version;
        device.down = device.restartDrops;
        server.closeIdleConnections();
    }

    // The trial image an update or a rollback restarts into: `slot` running,
    // on trial, and what ran before kept in the other slot.
    function onTrial(slot, result) {
        const fw = device.firmware;
        Object.assign(fw, {
            mode: 'normal',
            running: { ...slot, state: 'trial', intact: null },
            other: { ...fw.running },
            trial: firmwareTrial(),
            upload: null,
            last_update: { version: slot.version, result, reason: '' },
        });
    }

    // Firmware::on_upload, then run_upload: what is refused before flash
    // mode, flash mode, the head, and an image written and checked.
    function upload(raw, headers, res) {
        const fw = device.firmware;
        if (!fw) {
            return send(res, 404, REPLIES.noFirmware);
        }
        if (!fw.other) {
            return send(res, 409, FIRMWARE.oneSlot);
        }
        if (!raw.length) {
            return send(res, 411, FIRMWARE.noLength);
        }
        if (raw.length < 288) {
            return send(res, 400, FIRMWARE.short);
        }
        if (raw.length > fw.slot_bytes) {
            return send(res, 413, FIRMWARE_PARTS.tooBig[0] + raw.length + FIRMWARE_PARTS.tooBig[1] + fw.slot_bytes + '\n');
        }
        const type = headers['content-type'] || '';
        if (type && type !== 'application/octet-stream') {
            return send(res, 415, FIRMWARE.type);
        }
        if (fw.trial) {
            return send(res, 409, FIRMWARE.onTrial);
        }
        // From here the upload's own task answers, and closes the connection.
        res.setHeader('Connection', 'close');
        fw.mode = 'flash';
        device.player = null;
        device.state = 'flash';
        if (raw[0] !== 0xe9) {
            fw.last_update = { version: '', result: 'refused', reason: FIRMWARE.notImage.trim() };
            return send(res, 400, FIRMWARE.notImage);
        }
        const text = (at) => raw.toString('latin1', at, at + 32).split('\0')[0];
        const written = { ...firmwareSlot({ label: fw.other.label }), version: text(48), project: text(80) };
        const sha256 = crypto.createHash('sha256').update(raw).digest('hex');
        send(res, 200, JSON.stringify({ version: written.version, slot: written.label, sha256, restarting: true }) + '\n', 'application/json');
        return restart(() => onTrial(written, 'on trial'));
    }

    // Firmware::on_rollback: on trial, giving the trial up; otherwise the
    // other slot's image, if it could boot, on trial.
    function rollback(res) {
        const fw = device.firmware;
        if (fw.upload) {
            return send(res, 409, FIRMWARE.busy);
        }
        if (fw.trial) {
            send(res, 200, FIRMWARE.givingUp);
            return restart(() =>
                Object.assign(fw, {
                    running: { ...fw.other },
                    other: { ...fw.running, state: 'invalid' },
                    trial: null,
                    last_update: { version: fw.running.version, result: 'rolled back', reason: 'rolled back by request during its trial' },
                }),
            );
        }
        const o = fw.other;
        if (!o) {
            return send(res, 409, FIRMWARE.nothingOneSlot);
        }
        const [none, failed, empty] = FIRMWARE_PARTS.noImage;
        if (o.state === 'empty') {
            return send(res, 409, none + o.label + empty + '\n');
        }
        if (o.state === 'invalid' || o.state === 'aborted' || o.intact === false) {
            return send(res, 409, none + o.label + failed + '\n');
        }
        send(res, 200, FIRMWARE_PARTS.goingBack[0] + o.version + FIRMWARE_PARTS.goingBack[1] + '\n');
        return restart(() => onTrial(o, 'rollback requested'));
    }

    function answer(route, rawBody, res, raw, headers) {
        // read_body: an empty body and one over 2 KB are both "no body", and
        // the whitespace a shell leaves on the end is trimmed.
        const body = rawBody.length > 2048 ? '' : rawBody.replace(/[\r\n ]+$/, '');
        const fw = device.firmware;
        switch (route) {
            case 'GET /':
                return file(res, 'iclforge_ui.html', 'text/html; charset=utf-8');
            case 'GET /ui.js':
                return file(res, 'iclforge_ui.js', 'text/javascript; charset=utf-8');
            case 'GET /api':
                return send(res, 200, REPLIES.api);
            case 'GET /status':
                advance();
                return send(res, 200, payload === null ? statusJson(device) : payload, 'application/json');
            case 'GET /hardware':
                return send(res, 200, JSON.stringify(device.hardware) + '\n', 'application/json');
            case 'POST /play':
                if (!body) {
                    return send(res, 400, REPLIES.playEmpty);
                }
                if (body.length >= 512) {
                    return send(res, 409, REPLIES.playRefused);
                }
                play(body);
                return send(res, 202, REPLIES.accepted);
            case 'POST /stop':
                device.player = null;
                device.state = 'stopped';
                return send(res, 200, REPLIES.stopped);
            case 'POST /volume': {
                const value = Number.parseFloat(body);
                if (!body || Number.isNaN(value) || value < 0 || value > 1) {
                    return send(res, 400, REPLIES.volumeBad);
                }
                device.volume = value;
                return send(res, 200, REPLIES.ok);
            }
            case 'GET /name':
                return send(res, 200, device.name + '\n');
            case 'PUT /name': {
                if (!body) {
                    return send(res, 400, REPLIES.nameEmpty);
                }
                if (!validName(body)) {
                    return send(res, 409, REPLIES.nameRefused);
                }
                device.name = body;
                return send(res, 200, REPLIES.ok);
            }
            case 'GET /slot-width':
                return send(res, 200, device.slotBits + '\n');
            case 'PUT /slot-width': {
                const bits = Number.parseInt(body, 10);
                if (!body || Number.isNaN(bits)) {
                    return send(res, 400, REPLIES.slotWidthBad);
                }
                if ((bits !== 16 && bits !== 32) || device.player) {
                    return send(res, 409, REPLIES.slotWidthRefused);
                }
                device.slotBits = bits;
                // Two lines carry twice one line's slots, and a line carries
                // 128 bits a frame whichever width divides it.
                device.sinkSlots = (bits === 16 ? 8 : 4) * (device.secondLine ? 2 : 1);
                return send(res, 200, REPLIES.nextPlay);
            }
            case 'GET /wiring':
                if (device.secondLine === undefined) {
                    return send(res, 404, REPLIES.wiringNone);
                }
                return send(res, 200, (device.secondLine ? '1' : '0') + '\n');
            case 'PUT /wiring': {
                if (body !== '0' && body !== '1') {
                    return send(res, 400, REPLIES.wiringBad);
                }
                if (device.secondLine === undefined) {
                    return send(res, 409, REPLIES.wiringFixed);
                }
                if (device.player) {
                    return send(res, 409, REPLIES.wiringRefused);
                }
                device.secondLine = body === '1';
                device.sinkSlots = (device.slotBits === 16 ? 8 : 4) * (device.secondLine ? 2 : 1);
                return send(res, 200, REPLIES.nextPlay);
            }
            case 'PUT /network': {
                const [ssid, ...rest] = rawBody.split('\n');
                if (!ssid.trim()) {
                    return send(res, 400, REPLIES.networkEmpty);
                }
                device.ssid = ssid.trim();
                device.password = rest.join('\n');
                return send(res, 200, REPLIES.nextBoot);
            }
            case 'GET /network-preference':
                if (!device.network || !device.network.preference) {
                    return send(res, 404, REPLIES.networkPreferenceNone);
                }
                return send(res, 200, device.network.preference + '\n');
            case 'PUT /network-preference': {
                if (!['auto', 'ethernet', 'wifi'].includes(body)) {
                    return send(res, 400, REPLIES.networkPreferenceBad);
                }
                if (!device.network || !device.network.preference) {
                    return send(res, 409, REPLIES.networkPreferenceNone);
                }
                // The station alone needs a network to join.
                if (body === 'wifi' && !device.ssid) {
                    return send(res, 409, REPLIES.networkPreferenceRefused);
                }
                device.network = cableNetwork(body);
                return send(res, 200, REPLIES.networkPreferenceOk);
            }
            case 'GET /now-playing':
                if (!device.screen) {
                    return send(res, 404, REPLIES.noScreen);
                }
                return send(res, 200, device.screen.view + '\n');
            case 'PUT /now-playing': {
                const word = body.trim();
                if (!['speakers', 'title', 'clock', 'off'].includes(word)) {
                    return send(res, 400, REPLIES.nowPlayingBad);
                }
                if (!device.screen) {
                    return send(res, 409, REPLIES.noScreen);
                }
                // A build shows the views it has; the others are refused.
                if (!device.screen.views.includes(word)) {
                    return send(res, 409, REPLIES.nowPlayingRefused);
                }
                device.screen.view = word;
                return send(res, 200, REPLIES.nowPlayingOk);
            }
            case 'GET /dim-after':
                if (!device.screen) {
                    return send(res, 404, REPLIES.noScreen);
                }
                return send(res, 200, device.screen.dimAfter + '\n');
            case 'PUT /dim-after': {
                const word = body.trim();
                const minutes = /^[0-9]{1,3}$/.test(word) ? Number.parseInt(word, 10) : word === 'never' ? 0 : -1;
                if (minutes < 0 || minutes > 240 || (word !== 'never' && minutes < 1)) {
                    return send(res, 400, REPLIES.dimAfterBad);
                }
                if (!device.screen) {
                    return send(res, 409, REPLIES.noScreen);
                }
                device.screen.dimAfter = minutes === 0 ? 'never' : String(minutes);
                return send(res, 200, REPLIES.dimAfterOk);
            }
            case 'GET /dynamic-range':
                if (!device.dynamicRange) {
                    return send(res, 404, REPLIES.noDynamicRange);
                }
                return send(res, 200, device.dynamicRange + '\n');
            case 'PUT /dynamic-range': {
                const word = body.trim();
                if (!['standard', 'late-night'].includes(word)) {
                    return send(res, 400, REPLIES.dynamicRangeBad);
                }
                if (!device.dynamicRange) {
                    return send(res, 409, REPLIES.noDynamicRange);
                }
                device.dynamicRange = word;
                return send(res, 200, REPLIES.dynamicRangeOk);
            }
            case 'GET /timezone':
                if (!device.time) {
                    return send(res, 404, REPLIES.noClock);
                }
                return send(res, 200, device.time.zone + '\n');
            case 'PUT /timezone': {
                const word = body.trim();
                if (word !== 'none' && !validTimeZone(word)) {
                    return send(res, 400, REPLIES.timeZoneBad);
                }
                if (!device.time) {
                    return send(res, 409, REPLIES.noClock);
                }
                if (device.time.zoneRefused) {
                    return send(res, 409, REPLIES.timeZoneRefused);
                }
                device.time.zone = word;
                return send(res, 200, REPLIES.timeZoneOk);
            }
            case 'GET /ntp-server':
                if (!device.time) {
                    return send(res, 404, REPLIES.noClock);
                }
                return send(res, 200, device.time.server + '\n');
            case 'PUT /ntp-server': {
                const word = body.trim();
                if ((word !== 'none' && !validNtpServer(word)) || (/^none$/i.test(word) && word !== 'none')) {
                    return send(res, 400, REPLIES.ntpServerBad);
                }
                if (!device.time) {
                    return send(res, 409, REPLIES.noClock);
                }
                device.time.server = word;
                return send(res, 200, REPLIES.ntpServerOk);
            }
            case 'GET /time-sync':
                if (!device.time) {
                    return send(res, 404, REPLIES.noClock);
                }
                return send(res, 200, device.time.sync + '\n');
            case 'PUT /time-sync': {
                const word = body.trim();
                if (word !== 'on' && word !== 'off') {
                    return send(res, 400, REPLIES.timeSyncBad);
                }
                if (!device.time) {
                    return send(res, 409, REPLIES.noClock);
                }
                device.time.sync = word;
                device.time.asking = word === 'on';
                device.time.synced = word === 'on' && device.time.synced;
                return send(res, 200, REPLIES.timeSyncOk);
            }
            case 'GET /pairing':
                if (!device.sendspin) {
                    return send(res, 404, REPLIES.pairingRefused);
                }
                return send(res, 200, JSON.stringify({ capacity: 8, servers: device.pairings }) + '\n', 'application/json');
            case 'POST /pairing': {
                // "forget " and a server_id, as on_pairing reads it: that one
                // server. read_body has trimmed the body, so "forget " alone
                // is "forget", every server, on the board as here.
                const one = body.startsWith('forget ') ? body.slice('forget '.length) : '';
                if (!one && body !== 'reset' && body !== 'cancel' && body !== 'forget') {
                    return send(res, 400, REPLIES.pairingBad);
                }
                const p = device.sendspin;
                if (!p) {
                    return send(res, 409, REPLIES.pairingRefused);
                }
                if (one) {
                    const gone = device.pairings.find((s) => s.server_id === one);
                    if (!gone) {
                        return send(res, 404, REPLIES.pairingUnknown);
                    }
                    device.pairings = device.pairings.filter((s) => s !== gone);
                    p.paired = device.pairings.length;
                    // Its connection closes, with client/goodbye user_request.
                    if (gone.connected) {
                        device.sendspin = { ...idleSendspin(), client_id: p.client_id, paired: p.paired };
                    }
                    return send(res, 200, REPLIES.ok);
                }
                if (body === 'reset') {
                    Object.assign(p, { pairing_held: false, pairing_rounds: 0 });
                } else if (body === 'cancel') {
                    if (p.pairing_code) {
                        Object.assign(p, { pairing_code: '', pairing_outcome: 'cancelled' });
                    }
                } else {
                    // A new identity, and every server's record gone: the
                    // connections close and the player starts again.
                    device.sendspin = { ...idleSendspin(), client_id: 'Q1vGr0WkzZ5c2hXU8eYy0fKp3tNnJmAs7LbD4oHqIwE' };
                    device.pairings = [];
                }
                return send(res, 200, REPLIES.ok);
            }
            case 'GET /layout':
                return send(res, 200, device.layout + '\n');
            case 'PUT /layout': {
                if (!body) {
                    return send(res, 400, REPLIES.layoutEmpty);
                }
                const slots = slotsOf(body);
                if (!slots || slots > device.sinkSlots) {
                    return send(res, 409, REPLIES.layoutRefused);
                }
                device.layout = body;
                return send(res, 200, REPLIES.layoutOk);
            }
            // Updates over the network (planning/esp32-ota.md). A firmware
            // without them answers every one of these routes 404.
            case 'GET /firmware':
                if (!fw) {
                    return send(res, 404, REPLIES.noFirmware);
                }
                return send(res, 200, JSON.stringify(fw) + '\n', 'application/json');
            case 'PUT /firmware':
                return upload(raw, headers, res);
            case 'PUT /firmware/mode':
                if (!fw) {
                    return send(res, 404, REPLIES.noFirmware);
                }
                if (body !== 'flash' && body !== 'normal') {
                    return send(res, 400, FIRMWARE.modeBad);
                }
                if (fw.trial || fw.upload) {
                    return send(res, 409, fw.trial ? FIRMWARE.onTrial : FIRMWARE.busy);
                }
                if (body === 'flash') {
                    fw.mode = 'flash';
                    device.player = null;
                    device.state = 'flash';
                    return send(res, 200, FIRMWARE.flashMode);
                }
                if (fw.mode !== 'flash') {
                    return send(res, 200, FIRMWARE.notFlash);
                }
                send(res, 200, FIRMWARE.leaving);
                return restart(() => (fw.mode = 'normal'));
            case 'PUT /firmware/rollback':
                return fw ? rollback(res) : send(res, 404, REPLIES.noFirmware);
            case 'POST /restart':
                if (!fw) {
                    return send(res, 404, REPLIES.noFirmware);
                }
                if (fw.trial || fw.upload) {
                    return send(res, 409, fw.trial ? FIRMWARE.restartOnTrial : FIRMWARE.underWay);
                }
                send(res, 200, FIRMWARE.restarting);
                return restart(() => (fw.mode = 'normal'));
            // Reset preferences: the screen's choices and the time zone go back to the image's; the name, the
            // network, the wiring and the pairings stay (libs/device's settings_keys.hpp). At once.
            case 'POST /reset-preferences':
                if (device.screen) {
                    Object.assign(device.screen, { view: 'speakers', dimAfter: '5' });
                }
                if (device.time) {
                    device.time.zone = 'none';
                }
                return send(res, 200, REPLIES.resetPreferencesOk);
            // The factory reset: only by the board's own address or name, and only for the words; then
            // everything is erased and the board restarts, with a new identity and no network of its own.
            case 'POST /factory-reset': {
                const host = (headers.host || '').split(':')[0];
                if (!hostNames(device.name).includes(host)) {
                    return send(res, 403, REPLIES.factoryResetHost);
                }
                if (body !== 'erase everything') {
                    return send(res, 400, REPLIES.factoryResetBad);
                }
                send(res, 200, REPLIES.factoryResetOk);
                return restart(() => {
                    Object.assign(device, { name: 'hearth-a1b2c3', ssid: '', password: '', layout: '2.0', pairings: [] });
                    if (device.sendspin) {
                        device.sendspin = { ...idleSendspin(), client_id: 'Q1vGr0WkzZ5c2hXU8eYy0fKp3tNnJmAs7LbD4oHqIwE' };
                    }
                    if (device.screen) {
                        Object.assign(device.screen, { view: 'speakers', dimAfter: '5' });
                    }
                    if (device.time) {
                        Object.assign(device.time, { zone: 'none', server: 'none', sync: 'on' });
                    }
                    if (fw) {
                        fw.mode = 'normal';
                    }
                });
            }
            // The last crash's core dump (O4): the bytes are the model's
            // `coredumpBytes`, and GET /firmware's `coredump` says there is one.
            case 'GET /firmware/coredump':
                if (!fw) {
                    return send(res, 404, REPLIES.noFirmware);
                }
                if (!fw.coredump) {
                    return send(res, 404, FIRMWARE.noCoredump);
                }
                return send(res, 200, device.coredumpBytes, 'application/octet-stream');
            case 'DELETE /firmware/coredump':
                if (!fw) {
                    return send(res, 404, REPLIES.noFirmware);
                }
                if (fw.upload) {
                    return send(res, 409, FIRMWARE.busy);
                }
                fw.coredump = null;
                return send(res, 200, FIRMWARE.erased);
            // Recent console output (O4): a firmware that keeps no ring, as
            // the page's own tests have no use for one.
            case 'GET /log':
                return send(res, 404, REPLIES.noLog);
            default: {
                // esp_http_server's own answers, after which it closes the
                // connection.
                const [, url] = route.split(' ');
                const known = ROUTES.some((r) => r.split(' ')[1] === url);
                res.setHeader('Connection', 'close');
                return known
                    ? send(res, 405, 'Specified method is invalid for this resource', 'text/html')
                    : send(res, 404, 'Nothing matches the given URI', 'text/html');
            }
        }
    }

    const server = http.createServer((req, res) => {
        // As bytes: an image's are not text.
        const chunks = [];
        req.on('data', (chunk) => chunks.push(chunk));
        req.on('end', () => {
            const raw = Buffer.concat(chunks);
            const body = raw.toString('utf8');
            const route = req.method + ' ' + (req.url || '/').split('?')[0];
            requests.push({ route, body, raw, at: Date.now(), headers: req.headers });
            if (device.down > 0 && route !== 'GET /' && route !== 'GET /ui.js') {
                device.down -= 1;
                req.socket.destroy();
                return;
            }
            if (route === 'GET /status') {
                statusInFlight += 1;
                statusInFlightMost = Math.max(statusInFlightMost, statusInFlight);
                res.on('close', () => (statusInFlight -= 1));
            }
            const next = (scripted.get(route) || []).shift();
            if (next === 'drop') {
                dropsPending -= 1;
                req.socket.destroy();
            } else if (next === 'hang') {
                // Never answered: the page's own timeout has to notice.
            } else if (next) {
                send(res, next.status, next.body, next.type);
            } else if (holding.has(route)) {
                holding.get(route).push(() => answer(route, body, res, raw, req.headers));
            } else {
                answer(route, body, res, raw, req.headers);
            }
        });
    });
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
    const sockets = new Set();
    server.on('connection', (socket) => {
        sockets.add(socket);
        socket.on('close', () => sockets.delete(socket));
    });

    return {
        url: `http://127.0.0.1:${server.address().port}/`,
        device,
        requests,
        // The requests made to one route, as "METHOD /path".
        sent: (route) => requests.filter((r) => r.route === route),
        // A reply to give the next request to a route instead of the model's:
        // {status, body, type}, 'drop' (close the connection unanswered) or
        // 'hang' (never answer). Before a drop the idle connections close, so
        // that the request meets a connection Chromium did not reuse: on one
        // it did, Chromium sends the request again, and then how often
        // depends on how many connections it has open.
        next(route, reply) {
            if (!scripted.has(route)) {
                scripted.set(route, []);
            }
            scripted.get(route).push(reply);
            if (reply === 'drop') {
                dropsPending += 1;
                server.closeIdleConnections();
            }
        },
        // Hold the answers to a route until the returned function is called.
        hold(route) {
            holding.set(route, []);
            return () => {
                const waiting = holding.get(route) || [];
                holding.delete(route);
                waiting.forEach((reply) => reply());
            };
        },
        // A fixed GET /status body - a recorded payload, or anything else.
        setStatus(body) {
            payload = body === null ? null : typeof body === 'string' ? body : JSON.stringify(body);
        },
        statusInFlightMost: () => statusInFlightMost,
        close: () =>
            new Promise((resolve) => {
                sockets.forEach((socket) => socket.destroy());
                server.close(() => resolve());
            }),
    };
}

module.exports = {
    startStub,
    statusJson,
    idleSendspin,
    playingSendspin,
    pairedServer,
    wifiNetwork,
    cableNetwork,
    screenBoard,
    timeBoard,
    validName,
    hostNames,
    defaultHardware,
    defaultFirmware,
    firmwareSlot,
    emptySlot,
    firmwareTrial,
    appImage,
    REPLIES,
    FIRMWARE,
    FIRMWARE_PARTS,
    ROUTES,
    POLICY,
    UI_DIR,
    slotsOf,
    speakersOf,
    served,
    validTimeZone,
};
