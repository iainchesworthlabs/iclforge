// @ts-check
'use strict';

// The flash budget planning/esp32-device-ui.md sets for the page (decision 18):
// the page and its script together, as the firmware embeds them. LF
// line endings too (.gitattributes pins them), so a Windows checkout embeds,
// budgets and maps coverage onto the same bytes CI does.
//
// 24,576 since Hearth B2, re-derived rather than raised to fit: the page lost
// the playback controls, which a server now owns, and gained the settings that
// are the board's own - a name, a network, a slot width, the wiring.
//
// 28,672 since Hearth B3: the Sendspin player's section - the server, the
// link, the clock, how late the stream plays, underruns, a level per output,
// a pairing code and the three pairing actions. planning/esp32-device-ui.md,
// decision 18, has the image sizes behind it: the budget is a statement about
// keeping the page small enough to read and serve from flash rather than
// about running out of room.
//
// 45,056 since the redesign: Hearth's own look - its palette in both schemes,
// section rules, segmented controls for the slot width and the named
// layouts, level meters, a toast for the live region and a dialog before
// forgetting every server - and the board's network and firmware. Decision
// 22 has the images it was measured against.
//
// 49,152 since the list of paired servers: GET /pairing's servers, each by
// its name with a Forget, and the one dialog asking before forgetting one
// server or every server. Decision 22's re-derivation has the images.
//
// 81,920 (80 KiB) since the layout chooser and the Preferences work: the owner, 2026-10-25, "we can increase the device web page size if
// required". Raised with room for the settings that follow (a per-speaker trim, a zone picker) rather than to fit one change;
// decision 22's third re-derivation has the images, slot by slot.
//
// 65,536 since the zone list of the Time section (61,440 with the section alone) (a board's time zone, time server and whether it asks one: planning/p4x-now-playing.md,
// stage 3), which every board that has a network has; 57,344 since the firmware section (decision 29): both slots, a trial, an
// upload and the last update; an upload from a file with the bytes sent
// shown; Restart and Roll back behind the dialog, which now asks before
// anything that cannot be taken back. Decision 22's second re-derivation has
// the images.
//
// 69,632 since the Speakers section (a level and a distance for each speaker, a test tone out of each, and what the
// build can do about a delay: planning/p4x-preferences.md), which every board with a Sendspin player has: a table
// of the layout's speakers, two fields and a button a row, and three buttons. About 4.7 KB; the images have room.

const fs = require('fs');
const path = require('path');
const { test, expect } = require('@playwright/test');
const { UI_DIR } = require('./stub');

const BUDGET = 81920;

test('the page and its script fit the flash budget', () => {
    const files = ['iclforge_ui.html', 'iclforge_ui.js'].map((name) => fs.readFileSync(path.join(UI_DIR, name)));
    const total = files.reduce((sum, file) => sum + file.length, 0);
    test.info().annotations.push({
        type: 'flash',
        description: `${files.map((f) => f.length).join(' + ')} = ${total} of ${BUDGET} bytes`,
    });
    for (const file of files) {
        expect(file.includes(13), 'a carriage return in a file the firmware embeds').toBe(false);
    }
    expect(total).toBeLessThanOrEqual(BUDGET);
});
