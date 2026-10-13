// iclforge WASM decode demo - a consumer of the bundled iclforge-wasm-decoder
// bindings from bindings/js/. This file owns the page
// (Web Audio playback of already-decoded PCM, the Canvas visualizations ported
// from apps/forge/gui/assets/qml/SoundfieldView.qml and Main.qml's Objects tab, scrub/solo
// controls) - decoding, the §7.8 fold and the realtime AudioWorklet pipeline
// all come from those bindings.
//
// `./package/` is `bindings/js/dist/` copied in alongside the
// Emscripten build output (see apps/demos/wasm/CMakeLists.txt's build docs in
// docs/platforms/wasm.md) - a self-contained servable directory needs both.
// `iclforge_decode.js` (loaded as a plain classic <script> in index.html,
// exactly as before) supplies the `createIclForgeModule` factory the bindings
// itself takes as a parameter rather than embedding a compiled binary of its
// own - see bindings/js/README.md's "Loading the WASM module" section.

import {
    decodeFile, DownmixTarget, IclForgeDecoderNode, KaraokeReproduction, KaraokeVocals, scanStream,
} from './package/index.js';
import {
    MicrophoneSession, drawRibbon, guideChannelIndex, guidePartName, noteName, readReference,
} from './mic.js';

// Ear-level ring: iclforge::spatial's kSpeakerAzimuthDeg
// (libs/render/include/iclforge/render/spatial.hpp), ITU-R BS.775, degrees CCW from
// front, left positive. Ceiling ring: the same azimuth convention extended to
// Table E2.5's height locations, matching apps/forge/gui/assets/qml/SoundfieldView.qml's own
// extension (its location_azimuth_deg()) - a second, smaller, dashed ring for
// elevated channels, not a fabricated height axis. A plain 5.1/7.1
// stream (like the bundled demo) never populates it; a real 7.1.4 stream does.
const EAR_LEVEL_AZIMUTH_DEG = { L: 30, C: 0, R: -30, Ls: 110, Rs: -110 };
const CEILING_AZIMUTH_DEG = { Vhl: 45, Vhr: -45, Vhc: 0, Lts: 110, Rts: -110 };

// Distinct per-object colors - the desktop GUI's own room view uses one flat
// color (only the currently-SELECTED object gets an accent), but this page
// has no per-object selection concept, so each object gets its own hue to
// stay visually distinguishable with several moving at once.
const OBJECT_COLORS = ['248,113,113', '52,211,153', '96,165,250', '250,204,21', '192,132,252'];

let soloObject = -1; // -1 = play the bed downmix; >=0 = that object's own isolated audio

let audioCtx = null;
let sourceNode = null;
let playStartCtxTime = 0;
let playStartOffset = 0;
let playing = false;

let decoded = null; // DecodedProgram, see bindings/js/src/decode-file.ts
// What the current decode was made from, so that a change of the karaoke reproduction can decode it
// again (a push decoder is made with its settings).
let decodedBytes = null;
let decodedLabel = '';

const el = (id) => document.getElementById(id);

// --- Lyrics ------------------------------------------------------------------------------
// The words of the stream, read by the library's own reader (libs/lyrics, compiled into the
// module as parseLyrics / LyricsDocument - apps/demos/wasm/lyrics_bindings.cpp) and followed
// against the position of what is playing. The reader is asked where the position is on every
// frame, so a seek, a pause and a stop are only another position.
let lyricsModule = null;   // its own module instance, kept: a document lives in the module that made it
let lyricsDocument = null; // the Embind LyricsDocument, deleted when another file is loaded
let lyricsData = null;     // document.describe(): the lines and their words, as plain objects
let lyricsDelayMs = 0;
let lyricsShownKey = '';

function setStatus(text, isError) {
    const status = el('status');
    status.textContent = text;
    status.classList.toggle('error', Boolean(isError));
}

async function loadModule() {
    // createIclForgeModule is the global factory MODULARIZE+EXPORT_NAME
    // produces (see wasm_decode_demo/CMakeLists.txt's link options).
    return await createIclForgeModule();
}

// The §7.8 Lo/Ro fold the decoder produced, ready to play. The matrix, the
// levels and the normalisation are all the library's (iclforge::ac3::OutputStage) -
// this page has no downmix of its own, which is the point: what a visitor
// hears here is what 'forge decode channels=2' writes.
//
// No soft clip: §7.8.1 normalises the coefficients so that the sum feeding
// any one output never exceeds 1, which means the fold cannot be louder than
// the loudest coded sample.
function downmixToStereo(result) {
    const n = result.fold[0].length;
    return { left: result.fold[0], right: result.fold[1] && result.fold[1].length === n ? result.fold[1] : result.fold[0] };
}

// Builds the buffer actually played: either the bed downmix (default) or,
// when soloObject >= 0, that object's own real isolated JOC-reconstructed
// audio (mono, copied to both channels - deliberately no position-based pan
// here, so what you hear is unambiguously that object's own waveform and
// nothing else, not a re-panned approximation).
function buildPlaybackBuffer(result) {
    if (soloObject >= 0 && soloObject < result.objectCount) {
        const mono = result.objectAudio[soloObject];
        const left = new Float32Array(mono.length);
        const right = new Float32Array(mono.length);
        for (let i = 0; i < mono.length; i++) {
            const s = Math.tanh(mono[i]);
            left[i] = s;
            right[i] = s;
        }
        return { left, right };
    }
    return downmixToStereo(result);
}

function stop() {
    if (sourceNode) {
        try { sourceNode.stop(); } catch (e) { /* already stopped */ }
        sourceNode.disconnect();
        sourceNode = null;
    }
    playing = false;
    el('playPause').textContent = 'Play';
}

function play() {
    if (!decoded) return;
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    stop();

    const { left, right } = buildPlaybackBuffer(decoded);
    const buffer = audioCtx.createBuffer(2, left.length, decoded.sampleRate);
    buffer.copyToChannel(left, 0);
    buffer.copyToChannel(right, 1);

    sourceNode = audioCtx.createBufferSource();
    sourceNode.buffer = buffer;
    sourceNode.connect(audioCtx.destination);
    sourceNode.onended = () => { if (playing) stop(); };

    const offset = playStartOffset % decoded.durationSeconds;
    sourceNode.start(0, offset);
    playStartCtxTime = audioCtx.currentTime - offset;
    playing = true;
    el('playPause').textContent = 'Pause';
    requestAnimationFrame(tick);
}

function pause() {
    if (!playing) return;
    playStartOffset = currentPlaybackSeconds();
    stop();
}

function currentPlaybackSeconds() {
    if (!playing || !audioCtx) return playStartOffset;
    return audioCtx.currentTime - playStartCtxTime;
}

// --- Visualization: ported from apps/forge/gui/assets/qml/SoundfieldView.qml -----------
// (screenX/screenY azimuth->pixel mapping, ring radius, opacity-by-RMS,
// energy-vector arrow - see that file for the original QML this was ported
// from, and the PR description for why it's channel energy, not objects.)

function drawRing(ctx, cx, cy, radius, dashed) {
    ctx.save();
    if (dashed) ctx.setLineDash([4, 4]);
    ctx.strokeStyle = 'rgba(148,163,184,0.35)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.arc(cx, cy, radius, 0, 2 * Math.PI);
    ctx.stroke();
    ctx.restore();
}

function drawSpeaker(ctx, cx, cy, radius, az, label, level, color) {
    const azRad = (az * Math.PI) / 180;
    const sx = cx - Math.sin(azRad) * radius;
    const sy = cy - Math.cos(azRad) * radius;
    const dotRadius = 6 + level * 10;

    ctx.beginPath();
    ctx.arc(sx, sy, dotRadius, 0, 2 * Math.PI);
    ctx.fillStyle = `rgba(${color},${0.35 + 0.65 * level})`;
    ctx.fill();
    ctx.strokeStyle = 'rgba(226,232,240,0.8)';
    ctx.lineWidth = 1.5;
    ctx.stroke();

    ctx.fillStyle = 'rgba(226,232,240,0.9)';
    ctx.font = '12px system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText(label, sx, sy - dotRadius - 8);

    return { x: Math.sin(azRad) * level, y: Math.cos(azRad) * level };
}

// Ported from apps/forge/gui/assets/qml/SoundfieldView.qml: an ear-level ring plus a
// smaller, dashed ceiling ring for elevated channels (its own
// visual cue for "a conceptually different, flattened-height plane"), driven
// throughout by real per-channel RMS from the decoder - not a fabricated
// height axis; channels this stream doesn't have simply don't light up.
function draw() {
    const canvas = el('ring');
    const ctx = canvas.getContext('2d');
    const w = canvas.width, h = canvas.height;
    const cx = w / 2, cy = h / 2 + 20;
    const earRadius = Math.min(w, h) / 2 - 44;
    const ceilingRadius = earRadius * 0.6;

    ctx.clearRect(0, 0, w, h);
    drawRing(ctx, cx, cy, earRadius, false);
    drawRing(ctx, cx, cy, ceilingRadius, true);

    // Listener.
    ctx.beginPath();
    ctx.arc(cx, cy, 8, 0, 2 * Math.PI);
    ctx.strokeStyle = 'rgba(148,163,184,0.6)';
    ctx.stroke();

    if (!decoded) {
        requestAnimationFrame(draw);
        return;
    }

    const t = currentPlaybackSeconds();
    const blockDuration = decoded.energyBlockSize / decoded.sampleRate;
    let vecX = 0, vecY = 0, lfeLevel = 0;

    decoded.channelLabels.forEach((label, ch) => {
        const energyTrace = decoded.energy[ch];
        const blockIndex = Math.max(0, Math.min(energyTrace.length - 1, Math.floor(t / blockDuration)));
        const rms = energyTrace.length > 0 ? energyTrace[blockIndex] : 0;
        // 0..~0.4 RMS covers this demo's material comfortably; clamp to keep
        // a silent channel visibly at rest rather than pinned mid-scale.
        const level = Math.max(0, Math.min(1, rms / 0.35));

        if (label === 'LFE') { lfeLevel = level; return; } // no direction (SoundfieldView.qml's own rule).

        if (label in EAR_LEVEL_AZIMUTH_DEG) {
            const v = drawSpeaker(ctx, cx, cy, earRadius, EAR_LEVEL_AZIMUTH_DEG[label], label, level, '96,165,250');
            vecX += v.x; vecY += v.y;
        } else if (label in CEILING_AZIMUTH_DEG) {
            drawSpeaker(ctx, cx, cy, ceilingRadius, CEILING_AZIMUTH_DEG[label], label, level, '167,139,250');
        } // any other channel (Cs, Lrs/Rrs, Lw/Rw, ...): no ring position in this simple demo.
    });

    // Energy-vector arrow: the ear-level soundfield's overall instantaneous
    // direction (not a height indicator - see the ceiling ring for that).
    const vecLen = Math.hypot(vecX, vecY);
    if (vecLen > 0.02) {
        const scale = earRadius * Math.min(1, vecLen / 1.5);
        const ex = cx - (vecX / vecLen) * scale;
        const ey = cy - (vecY / vecLen) * scale;
        ctx.beginPath();
        ctx.moveTo(cx, cy);
        ctx.lineTo(ex, ey);
        ctx.strokeStyle = 'rgba(251,191,36,0.9)';
        ctx.lineWidth = 2.5;
        ctx.stroke();
    }

    // LFE: text-only badge, no direction (SoundfieldView.qml's own rule).
    ctx.fillStyle = `rgba(248,113,113,${0.4 + 0.6 * lfeLevel})`;
    ctx.font = 'bold 12px system-ui, sans-serif';
    ctx.textAlign = 'left';
    ctx.fillText('LFE', 12, h - 14);

    requestAnimationFrame(draw);
}

// Real per-object level at playback time t: a short RMS window over that
// object's own real reconstructed audio (JOC) - the same "pulse with real
// energy" language the speaker-ring panel already uses, just windowed in JS
// instead of precomputed in C++ since only a handful of objects ever need
// it, each over a small window, once per animation frame.
function objectAudioLevelAt(result, obj, t) {
    const audio = result.objectAudio[obj];
    if (!audio || audio.length === 0) return 0;
    const center = Math.floor(t * result.sampleRate);
    const half = 512;
    const start = Math.max(0, center - half);
    const end = Math.min(audio.length, center + half);
    if (end <= start) return 0;
    let sumSq = 0;
    for (let i = start; i < end; i++) sumSq += audio[i] * audio[i];
    return Math.max(0, Math.min(1, Math.sqrt(sumSq / (end - start)) / 0.35));
}

// Looks up object `obj`'s state at playback time `t`, clamped to the decoded
// range - real decoded OAMD data, one entry per real decoded frame, not
// interpolated/fabricated between frames. width/depth/height are TS 103 420
// 5.6.1.2's extent, 0/0/0 for a point source.
function objectStateAt(result, obj, t) {
    const positions = result.objectPositions[obj];
    const frameDuration = result.objectFrameSize / result.sampleRate;
    const relative = t - result.objectStartSeconds;
    const frame = Math.max(0, Math.min(result.objectFrameCount - 1, Math.floor(relative / frameDuration)));
    const stride = 7;
    const base = frame * stride;
    return {
        x: positions[base], y: positions[base + 1], z: positions[base + 2],
        gainDb: positions[base + 3],
        width: positions[base + 4] ?? 0,
        depth: positions[base + 5] ?? 0,
        height: positions[base + 6] ?? 0,
    };
}

function drawRoomBox(ctx, x, y, w, h, label) {
    ctx.strokeStyle = 'rgba(148,163,184,0.3)';
    ctx.strokeRect(x + 1, y + 1, w - 2, h - 2);
    ctx.fillStyle = 'rgba(148,163,184,0.6)';
    ctx.font = '10px system-ui, sans-serif';
    ctx.textAlign = 'left';
    ctx.fillText(label, x + 6, 12);
}

function drawObjectDot(ctx, x, y, radius, color, label, highlighted) {
    ctx.beginPath();
    ctx.arc(x, y, radius, 0, 2 * Math.PI);
    ctx.fillStyle = `rgba(${color},${highlighted ? 1 : 0.85})`;
    ctx.fill();
    if (highlighted) {
        ctx.strokeStyle = '#fff';
        ctx.lineWidth = 2;
        ctx.stroke();
    }
    ctx.fillStyle = 'rgba(226,232,240,0.85)';
    ctx.font = '10px system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText(label, x, y - radius - 6);
}

// Ported from apps/forge/gui/assets/qml/Main.qml's Objects tab (top-down + elevation room
// panels, side by side - see docs/platforms/android.md's own description of
// the same layout in the Shield app). Unlike that GUI's own version, which
// previews positions about to be ENCODED, this one is driven entirely by
// real decoded OAMD positions read back out of the bitstream - what a real
// object actually did, not what it was told to do.
function drawObjects() {
    const canvas = el('objects');
    if (canvas) {
        const ctx = canvas.getContext('2d');
        const w = canvas.width, h = canvas.height;
        ctx.clearRect(0, 0, w, h);

        if (decoded && decoded.objectCount > 0) {
            const gap = 16;
            const padW = (w - gap) / 2;
            const elevX = padW + gap;
            drawRoomBox(ctx, 0, 0, padW, h, 'PLAN (top-down)');
            drawRoomBox(ctx, elevX, 0, padW, h, 'ELEVATION (side-on)');

            // Main.qml's own piecewise z->y mapping: ear level sits at 66% of
            // the pad's height, not the middle, and the floor/ceiling margins
            // differ (14px top, 10px bottom) - ported verbatim.
            const earY = h * 0.66;
            const zToY = (z) => (z >= 0 ? earY - z * (earY - 14) : earY + (-z) * ((h - 10) - earY));
            ctx.strokeStyle = 'rgba(148,163,184,0.25)';
            ctx.beginPath();
            ctx.moveTo(elevX + 4, earY);
            ctx.lineTo(elevX + padW - 4, earY);
            ctx.stroke();

            const t = currentPlaybackSeconds();
            for (let obj = 0; obj < decoded.objectCount; obj++) {
                const s = objectStateAt(decoded, obj, t);
                const level = objectAudioLevelAt(decoded, obj, t);
                const color = OBJECT_COLORS[obj % OBJECT_COLORS.length];
                // An object with extent draws bigger: the dot still tracks
                // level, and the widest axis adds up to another 14 px on top,
                // so a wall of rain reads as a wall rather than a raindrop.
                const extent = Math.max(s.width, s.depth, s.height);
                const radius = 5 + level * 7 + extent * 14;
                const label = decoded.objectLabels[obj] || `obj ${obj + 1}`;
                const highlighted = soloObject === obj;

                // Plan: x -> right wall, y -> rear wall (top-down).
                drawObjectDot(ctx, s.x * padW, s.y * h, radius, color, label, highlighted);
                // Elevation: x-axis stays depth (y); y-axis is height via zToY.
                drawObjectDot(ctx, elevX + s.y * padW, zToY(s.z), radius, color, label, highlighted);
            }
        }
    }
    requestAnimationFrame(drawObjects);
}

let seekDragging = false;

function tick() {
    if (!playing) return;
    const t = currentPlaybackSeconds();
    el('scrubTime').textContent = `${t.toFixed(1)}s / ${decoded.durationSeconds.toFixed(1)}s`;
    if (!seekDragging) el('seek').value = String((t / decoded.durationSeconds) * 1000);
    if (t < decoded.durationSeconds) requestAnimationFrame(tick);
}

// Lets a viewer scrub to any point in the real decoded audio/energy - moving
// the slider while playing restarts playback from there; while paused it just
// repositions the static preview frame draw() already reads from playStartOffset.
function seekTo(fraction) {
    if (!decoded) return;
    playStartOffset = Math.max(0, Math.min(decoded.durationSeconds, fraction * decoded.durationSeconds));
    el('scrubTime').textContent = `${playStartOffset.toFixed(1)}s / ${decoded.durationSeconds.toFixed(1)}s`;
    if (playing) play();
}

function setSoloObject(index) {
    soloObject = index;
    document.querySelectorAll('#soloControls button').forEach((btn) => {
        btn.classList.toggle('active', Number(btn.dataset.object) === index);
    });
    if (playing) { pause(); play(); } // restart on the newly-selected source, from the same position
}

function buildSoloControls(objectCount, labels) {
    const container = el('soloControls');
    container.innerHTML = '';
    if (objectCount === 0) {
        el('objectsPanel').style.display = 'none';
        return;
    }
    el('objectsPanel').style.display = '';

    const bedBtn = document.createElement('button');
    bedBtn.textContent = 'Bed (all channels)';
    bedBtn.dataset.object = '-1';
    bedBtn.className = 'active';
    bedBtn.addEventListener('click', () => setSoloObject(-1));
    container.appendChild(bedBtn);

    for (let obj = 0; obj < objectCount; obj++) {
        const btn = document.createElement('button');
        btn.textContent = labels && labels[obj] ? `Solo ${labels[obj]}` : `Solo object ${obj + 1}`;
        btn.dataset.object = String(obj);
        btn.style.setProperty('--dot-color', `rgb(${OBJECT_COLORS[obj % OBJECT_COLORS.length]})`);
        btn.classList.add('object-btn');
        btn.addEventListener('click', () => setSoloObject(obj));
        container.appendChild(btn);
    }
}

function setLyricsStatus(text, isError) {
    const status = el('lyricsStatus');
    status.textContent = text;
    status.classList.toggle('error', Boolean(isError));
}

function singerLabel(line) {
    if (!lyricsData || line.singer === '') return '';
    const first = lyricsData.firstSingerName || 'V1';
    const second = lyricsData.secondSingerName || 'V2';
    if (line.singer === 'v1') return first;
    if (line.singer === 'v2') return second;
    return `${first} + ${second}`;
}

// One line as an element. `now`, for the line being sung, makes each word a span carrying its
// state; the others are plain text.
function lineElement(line, className, state) {
    const row = document.createElement('div');
    row.className = `lyric ${className}` + (line.singer === 'v2' ? ' side-second' : '');
    if (line.singer !== '') {
        const who = document.createElement('span');
        who.className = 'who';
        who.textContent = singerLabel(line);
        row.appendChild(who);
    }
    if (state && line.words.length > 0) {
        line.words.forEach((word, index) => {
            const span = document.createElement('span');
            const wordState = state.wordStates[index];
            span.className = 'word' + (wordState === 0 ? ' sung' : wordState === 1 ? ' now' : '');
            if (wordState === 1) {
                span.style.setProperty('--fill', `${Math.round(state.fraction * 100)}%`);
                span.classList.add('filling');
            }
            span.textContent = word.text;
            row.appendChild(span);
        });
    } else {
        row.appendChild(document.createTextNode(line.text));
    }
    return row;
}

// How the line just sung went, as a small mark after it: three dots and a percentage, or "not sung"
// with no number. Nothing for a line the tune has no notes in, with the microphone off, or without
// the headphones answer. The line's own times are the lyrics', and the nudge puts them on the
// item's timeline as it puts the words later.
function lineMark(line) {
    if (!micSession || !micSession.on) {
        return null;
    }
    const result = micSession.lineResult(line.startMs + lyricsDelayMs, line.endMs + lyricsDelayMs);
    if (!result.scored) {
        return null;
    }
    const mark = document.createElement('span');
    mark.className = 'mark' + (result.notSung ? ' not-sung' : ` level-${result.level}`);
    mark.textContent = result.notSung ? 'not sung'
        : `${'●'.repeat(result.level)}${'○'.repeat(3 - result.level)} ${result.percent} %`;
    mark.setAttribute('aria-label', result.notSung ? 'Pitch: this line was not sung'
        : `Pitch: ${result.percent} percent`);
    return mark;
}

// Draws the previous, current and next lines for the playing position. Only changed when what it
// would draw has: the key is the line and the word and the fill to the percent.
function drawLyrics() {
    const view = el('lyricsView');
    if (!lyricsDocument || !lyricsData) {
        requestAnimationFrame(drawLyrics);
        return;
    }
    const lines = lyricsData.lines;
    if (!lyricsData.timed) {
        // Plain text is shown whole and is not followed.
        if (lyricsShownKey !== 'plain') {
            view.replaceChildren(...lines.map((line) => lineElement(line, 'timeless', null)));
            lyricsShownKey = 'plain';
        }
        requestAnimationFrame(drawLyrics);
        return;
    }
    const at = lyricsDocument.position(currentPlaybackSeconds() * 1000, lyricsDelayMs);
    const current = at.line;
    const states = [];
    if (current !== null && at.inLine) {
        const words = lines[current].words;
        for (let w = 0; w < words.length; w++) {
            states.push(lyricsDocument.wordState(current, w, at.positionMs));
        }
    }
    // The mark of the line just sung is part of what is drawn: it moves as frames are scored.
    const sungLine = current === null ? -1 : (at.inLine ? current - 1 : current);
    const sungMark = sungLine >= 0 && lines[sungLine].text !== '' ? lineMark(lines[sungLine]) : null;
    const key = `${current}|${at.inLine}|${states.join('')}|${Math.round(at.fraction * 100)}|` +
        `${sungMark ? sungMark.textContent : ''}`;
    if (key !== lyricsShownKey) {
        lyricsShownKey = key;
        const rows = [];
        const before = current === null ? -1 : current - 1;
        const after = current === null ? 0 : current + 1;
        if (before >= 0) {
            const previous = lineElement(lines[before], 'previous', null);
            if (at.inLine && sungMark) previous.appendChild(sungMark);
            rows.push(previous);
        }
        if (current !== null && lines[current].text !== '') {
            // After a line has ended (the gap a SubRip cue leaves) it is not being sung.
            rows.push(at.inLine
                ? lineElement(lines[current], 'current', { wordStates: states, fraction: at.fraction })
                : (() => {
                    const ended = lineElement(lines[current], 'previous', null);
                    if (sungMark) ended.appendChild(sungMark);
                    return ended;
                })());
        }
        // The next line that has words in it: a line that only marks a time is no line to read.
        for (let n = after; n < lines.length && n < after + 3; n++) {
            if (lines[n].text !== '') {
                rows.push(lineElement(lines[n], 'next', null));
                break;
            }
        }
        view.replaceChildren(...rows);
    }
    requestAnimationFrame(drawLyrics);
}

async function loadLyrics(file) {
    try {
        lyricsModule = lyricsModule ?? await loadModule();
        const bytes = new Uint8Array(await file.arrayBuffer());
        const duration = decoded ? Math.round(decoded.durationSeconds * 1000) : -1;
        const parsed = lyricsModule.parseLyrics(bytes, duration);
        if (!parsed.ok) {
            setLyricsStatus(`${file.name}: ${parsed.error}`, true);
            return;
        }
        if (lyricsDocument) lyricsDocument.delete();
        lyricsDocument = parsed.document;
        lyricsData = lyricsDocument.describe();
        lyricsShownKey = '';
        const sung = lyricsData.lines.filter((line) => line.text !== '').length;
        const kind = lyricsData.timed ? (lyricsData.lines.some((l) => l.words.length > 0)
            ? 'timed by word' : 'timed by line') : 'plain text, not followed';
        const title = lyricsData.title ? `"${lyricsData.title}" - ` : '';
        setLyricsStatus(`${title}${sung} lines, ${kind}${lyricsData.hasSingers ? ', a duet' : ''}.`, false);
    } catch (err) {
        setLyricsStatus(`Could not read ${file.name}: ${err.message}`, true);
    }
}

// The fold's own options, with the karaoke reproduction the page asks for. It only changes a
// stream that is karaoke (an AC-3 or E-AC-3 stream flagged bsmod 7): any other decodes as it did. At the
// stereo fold the stream's own mix is already Annex C's 2/0 reproduction, so the choices are the
// capable decoder's: which vocals are heard.
const KARAOKE_CHOICES = {
    stream: { karaoke: KaraokeReproduction.AsCoded, karaokeVocals: KaraokeVocals.Both },
    both: { karaoke: KaraokeReproduction.Capable, karaokeVocals: KaraokeVocals.Both },
    v1: { karaoke: KaraokeReproduction.Capable, karaokeVocals: KaraokeVocals.V1 },
    v2: { karaoke: KaraokeReproduction.Capable, karaokeVocals: KaraokeVocals.V2 },
    none: { karaoke: KaraokeReproduction.Capable, karaokeVocals: KaraokeVocals.None },
};

function foldOptions() {
    return {
        target: DownmixTarget.LoRo,
        applyDialnorm: true,
        ...KARAOKE_CHOICES[el('karaokeChoice').value],
    };
}

// What a change of the karaoke choice does: the stream is decoded again, from where it was.
function onKaraokeChange() {
    if (decodedBytes === null) return;
    const wasPlaying = playing;
    const at = decoded ? currentPlaybackSeconds() : 0;
    void handleDecoded(decodedBytes, decodedLabel, { resumeAt: at, play: wasPlaying });
}

async function handleDecoded(bytes, label, options = {}) {
    setStatus(`Decoding ${label}...`, false);
    try {
        const moduleInstance = await loadModule();
        // The real §7.8 Lo/Ro fold (iclforge::ac3::OutputStage/DC1) - never a hand-rolled
        // one - plus dialnorm normalisation, exactly what the old bespoke
        // whole-file decode did, now produced by the package's decodeFile()
        // built on the same push-frame primitive the realtime section below
        // uses.
        const result = decodeFile(moduleInstance, bytes, { fold: foldOptions() });
        decoded = result;
        decodedBytes = bytes;
        decodedLabel = label;
        el('karaokePanel').style.display = result.karaoke ? '' : 'none';
        playStartOffset = options.resumeAt ?? 0;
        soloObject = -1;
        stop();
        const objectNote = result.objectCount > 0 ? `, ${result.objectCount} Atmos object(s)` : '';
        el('streamInfo').textContent =
            `${result.streamKind}, ${result.sampleRate} Hz, ${result.channelCount} ch ` +
            `(${result.channelLabels.join(', ')})${objectNote}, ${result.durationSeconds.toFixed(1)}s`;
        el('playPause').disabled = false;
        el('seek').disabled = false;
        el('seek').value = String(Math.round((playStartOffset / result.durationSeconds) * 1000) || 0);
        el('scrubTime').textContent = `${playStartOffset.toFixed(1)}s / ${result.durationSeconds.toFixed(1)}s`;
        buildSoloControls(result.objectCount, result.objectLabels);
        setStatus(`Decoded ${label}.`, false);
        // The tune of a karaoke stream is read in the background; the microphone card waits for it.
        prepareMicrophone(result, bytes).catch((err) => micStatus(`Could not read the melody: ${err.message}`, true));
        if (options.play) play();
    } catch (err) {
        decoded = null;
        el('playPause').disabled = true;
        setStatus(`Decode failed: ${err.message}`, true);
    }
}

let bundledDemoBytes = null;

async function loadBundledDemo() {
    const response = await fetch('assets/demo.ec3');
    const buffer = await response.arrayBuffer();
    bundledDemoBytes = new Uint8Array(buffer);
    await handleDecoded(bundledDemoBytes, 'the bundled demo stream (assets/demo.ec3)');
}

// The demo materialises the decoded programme in the WASM heap and again in
// JS copies, so input size is the closest proxy for peak memory: past this
// cap the decode would only run into the module's memory ceiling and fail
// there anyway. 24 MiB of E-AC-3 at 448 kbps is about seven minutes.
const MAX_UPLOAD_BYTES = 24 * 1024 * 1024;

function loadFile(file) {
    if (file.size > MAX_UPLOAD_BYTES) {
        setStatus(
            `${file.name} is ${(file.size / (1024 * 1024)).toFixed(1)} MiB - this demo accepts ` +
            'up to 24 MiB (about seven minutes at 448 kbps). Try a shorter clip.',
            true);
        return;
    }
    const reader = new FileReader();
    reader.onload = () => handleDecoded(new Uint8Array(reader.result), file.name);
    reader.readAsArrayBuffer(file);
}

// --- Microphone ----------------------------------------------------------------------------
// A singer's pitch against the tune (planning/karaoke-microphone.md, M7). The tune is the guide
// melody's channel as coded, read by the library's tracker compiled into this module; the
// microphone is the browser's, opened only by the button, with its own echo cancellation; the
// score is the library's. mic.js has the pieces and the promises.
let micModule = null;          // its own module instance: a contour lives in the module that made it
let micReference = null;       // a ReferenceContour, or null
let micReferenceBytes = null;  // the decode it was read from, so that a re-decode of it is not re-read
let micReading = null;         // an AbortController for a read in flight
let micSession = null;
let micNotes = [];
let micRange = { low: 55, high: 79 };
let micPartText = '';
let micDrawScheduled = false;
let micStreams = 0;            // how many times a decode has taken the microphone and the tune away

function micStatus(text, isError) {
    const status = el('micStatus');
    status.textContent = text;
    status.classList.toggle('error', Boolean(isError));
}

function micClearReference() {
    micReading?.abort();
    micReading = null;
    micReference?.delete();
    micReference = null;
    micReferenceBytes = null;
    micNotes = [];
    micPartText = '';
}

// What a decode of another stream does to the card: the microphone goes off and the tune goes.
// A press may still be on its way to opening the microphone, because the browser asks the person
// and waits for as long as they like: stop() ends an opening that has got as far as the session,
// and the count tells the press, wherever it has got to, that the stream it was for has gone
// (toggleMicrophone). Without that, the microphone would open behind a card that is no longer
// there, or with a tune that has been deleted.
function micLeaveStream() {
    micStreams += 1;
    micSession?.stop();
    micClearReference();
}

// Called with every decode. A stream that is not karaoke has no tune and no card; a karaoke stream
// of the same bytes (a change of the vocals decodes it again) keeps the tune it has.
async function prepareMicrophone(result, bytes) {
    const panel = el('micPanel');
    if (!result.karaoke) {
        micLeaveStream();
        panel.style.display = 'none';
        return;
    }
    panel.style.display = '';
    if (micReferenceBytes === bytes && micReference) {
        return;
    }
    micLeaveStream();
    el('micToggle').disabled = true;
    micModule = micModule ?? await loadModule();

    const index = guideChannelIndex(result.channelLabels);
    if (index < 0) {
        micPartText = '';
        micStatus('This stream has no melody or vocal channel to score against; the microphone shows your pitch only.', false);
        el('micToggle').disabled = false;
        return;
    }
    const part = guidePartName(result.channelLabels, index);
    const reading = new AbortController();
    micReading = reading;
    micStatus(`Reading the melody (${part})… 0%`, false);
    const reference = await readReference(micModule, result.channels[index], result.sampleRate, {
        signal: reading.signal,
        onProgress: (share) => micStatus(`Reading the melody (${part})… ${Math.round(share * 100)}%`, false),
    });
    if (reading.signal.aborted) {
        reference?.delete();
        return;
    }
    micReading = null;
    if (!reference || reference.voicedFrames() === 0) {
        reference?.delete();
        micStatus(`The ${part} has no clear tune to score against; the microphone shows your pitch only.`, false);
        el('micToggle').disabled = false;
        return;
    }
    micReference = reference;
    micReferenceBytes = bytes;
    micPartText = part;
    micNotes = reference.notes();
    let low = Infinity;
    let high = -Infinity;
    for (let i = 2; i < micNotes.length; i += 3) {
        low = Math.min(low, micNotes[i]);
        high = Math.max(high, micNotes[i]);
    }
    low -= 2;
    high += 2;
    if (high - low < 18) {
        const grow = (18 - (high - low)) / 2;
        low -= grow;
        high += grow;
    }
    micRange = { low, high };
    micStatus(`Scored against ${part}.`, false);
    el('micToggle').disabled = false;
}

async function toggleMicrophone() {
    if (micSession?.on) {
        micSession.stop();
        return;
    }
    el('micToggle').disabled = true;
    // A decode of another stream that comes before the microphone is open overtakes this press
    // (micLeaveStream). The card is then that stream's, or gone: the press opens nothing, and
    // leaves the status and the button as the decode set them.
    const forStream = micStreams;
    const overtaken = () => forStream !== micStreams;
    try {
        // The context is the one playback uses: the voice and the sound it sings along to share a clock.
        if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
        await audioCtx.resume();
        micModule = micModule ?? await loadModule();
        if (overtaken()) {
            return;
        }
        const session = new MicrophoneSession({
            module: micModule,
            reference: micReference,
            audioContext: audioCtx,
            playback: () => ({ playing, startCtxTime: playStartCtxTime }),
            onUpdate: scheduleMicDraw,
        });
        micSession = session;
        session.headphones = el('micHeadphones').checked;
        session.offsetMs = Number(el('micOffset').value);
        await session.start();
        // Stopped while it was opening: it is off, and nothing went wrong.
        if (!session.on) {
            return;
        }
        // The track's own figure, when the browser gives one, is where the delay slider starts.
        if (session.inputLatencySec > 0) {
            el('micDelay').value = String(Math.round(session.inputLatencySec * 1000));
        } else {
            session.inputLatencySec = Number(el('micDelay').value) / 1000;
        }
        el('micDelayText').textContent = `${Math.round(session.inputLatencySec * 1000)} ms`;
        micStatus(micPartText ? `Scored against ${micPartText}.` : 'Showing your pitch.', false);
    } catch (err) {
        if (!overtaken()) {
            micSession = null;
            micStatus(micMessage(err), true);
        }
    } finally {
        if (!overtaken()) {
            el('micToggle').disabled = false;
        }
        scheduleMicDraw();
    }
}

// A sentence for a person: what the browser's error means for a microphone.
function micMessage(err) {
    switch (err && err.name) {
        case 'NotAllowedError': return 'The microphone was blocked. Allow it for this page in the browser and try again.';
        case 'NotFoundError': return 'No microphone was found.';
        case 'NotReadableError': return 'The microphone is in use by another application.';
        default: return String((err && err.message) || err);
    }
}

function scheduleMicDraw() {
    if (micDrawScheduled) return;
    micDrawScheduled = true;
    requestAnimationFrame(drawMic);
}

function drawMic() {
    micDrawScheduled = false;
    const session = micSession;
    const on = Boolean(session?.on);
    el('micToggle').textContent = on ? 'Turn the microphone off' : 'Turn the microphone on';
    el('micIndicator').style.display = on ? '' : 'none';
    const fill = el('micMeterFill');
    fill.style.width = `${Math.round((on ? session.level : 0) * 100)}%`;

    // The ribbon follows the sound being heard: the playing position less the output's latency.
    const canvas = el('micRibbon');
    const outputLatency = audioCtx ? (audioCtx.outputLatency || audioCtx.baseLatency || 0) : 0;
    const positionMs = (currentPlaybackSeconds() - outputLatency) * 1000;
    drawRibbon(canvas.getContext('2d'), canvas.width, canvas.height, {
        notes: micNotes,
        trace: on ? session.trace : [],
        positionMs: Math.max(0, positionMs),
        lowMidi: micRange.low,
        highMidi: micRange.high,
    });

    const pitch = el('micPitch');
    const score = el('micScore');
    if (!on) {
        pitch.textContent = 'Listening…';
        score.textContent = '';
    } else {
        const live = session.live;
        if (!live.voiced) {
            pitch.textContent = 'Listening…';
        } else {
            const base = `${noteName(live.hz)} · ${Math.round(live.hz)} Hz`;
            pitch.textContent = live.cents === null ? base
                : `${base} · ${Math.abs(live.cents) < 10 ? 'in tune' : `${Math.round(Math.abs(live.cents))} cents ${live.cents > 0 ? 'sharp' : 'flat'}`}`;
        }
        const s = session.score;
        if (!micReference || !s) {
            score.textContent = '';
        } else if (s.withheld) {
            score.textContent = 'Say you are wearing headphones to see a score.';
        } else if (s.percent < 0) {
            score.textContent = 'Sing a few notes to get a score.';
        } else {
            let text = `Pitch score so far: ${s.percent} %`;
            if (s.lagMs !== null && Math.abs(s.lagMs) >= 40) {
                const ms = Math.round(Math.abs(s.lagMs) / 10) * 10;
                text += s.lagMs > 0 ? ` You are about ${ms} ms behind the tune.` : ` You are about ${ms} ms ahead of the tune.`;
            }
            score.textContent = text;
        }
        scheduleMicDraw();   // the ribbon moves with the sound
    }

    const summary = el('micSummary');
    const last = session?.summary;
    if (!on && last && micReference) {
        const parts = [`Last performance: pitch score ${last.percent} %.`,
                       `You sang for ${last.coveragePercent} % of the tune's voiced time.`];
        if (Math.abs(last.meanCents) >= 10) {
            parts.push(`On average you were ${Math.round(Math.abs(last.meanCents))} cents ${last.meanCents > 0 ? 'sharp' : 'flat'}.`);
        }
        parts.push(`Compared with ${micPartText}, within ${last.toleranceCents} cents, in any octave. Shown here, kept nowhere.`);
        summary.textContent = parts.join(' ');
    } else {
        summary.textContent = '';
    }
}

// --- Realtime push-decode demo (AudioWorklet) ------------------------------
// Proves the OTHER real capability this package adds: the same bundled
// stream, fed one access unit at a time (as a live source would) through
// PushDecoder's own Embind class, decoded in a Worker, and played through a
// real AudioWorkletNode - not the whole-file decodeFile() path above. Needs
// cross-origin isolation for SharedArrayBuffer (see bindings/js/README.md); the
// button stays disabled and says why when that's not available (a plain
// `python3 -m http.server` does not send the required headers - see
// docs/platforms/wasm.md).

let realtimeNode = null;
let realtimeAudioCtx = null;
let realtimePushTimer = null;

function setRealtimeStatus(text) {
    el('realtimeStatus').textContent = text;
}

async function startRealtime() {
    el('realtimeStart').disabled = true;
    setRealtimeStatus('Loading module and fetching the bundled stream...');
    try {
        const moduleInstance = await loadModule();
        const bytes = bundledDemoBytes ?? new Uint8Array(await (await fetch('assets/demo.ec3')).arrayBuffer());
        const scanned = scanStream(moduleInstance, bytes);
        if (!scanned.ok) throw new Error(scanned.error);

        realtimeAudioCtx = new (window.AudioContext || window.webkitAudioContext)();
        realtimeNode = await IclForgeDecoderNode.create(realtimeAudioCtx, {
            workletProcessorUrl: new URL('./package/worklet-processor.js', location.href),
            workerUrl: new URL('./package/decoder-worker.js', location.href),
            wasmGlueUrl: new URL('./iclforge_decode.js', location.href),
            fold: { target: DownmixTarget.LoRo, applyDialnorm: true },
        });
        realtimeNode.addEventListener('streaminfo', (event) => {
            setRealtimeStatus(
                `Streaming: ${event.detail.sampleRate} Hz, ${event.detail.channelCount} coded channel(s), ` +
                `folded to ${event.detail.foldChannelCount}.`);
        });
        realtimeNode.addEventListener('underrun', (event) => {
            setRealtimeStatus(`Buffer underrun: ${event.detail} frames short.`);
        });
        realtimeNode.addEventListener('error', (event) => setRealtimeStatus(`Error: ${event.detail}`));

        // A real AudioContext starts consuming the instant the worklet node
        // is connected - unlike OfflineAudioContext, there is no "wait until
        // primed" concept in Web Audio. So this pre-buffers a couple of
        // seconds (a real streaming client does the same before hitting
        // play) by pushing that much up front, in a tight loop rather than
        // paced, and only connects the node once decode has caught up with
        // it - avoiding the cold-start underrun a from-zero connect would
        // hit while the first few units are still decoding.
        let index = 0;
        const unitsPerSecond = Math.ceil(scanned.sampleRate / 1536);
        const prebufferCount = Math.min(scanned.accessUnits.length, unitsPerSecond * 2);
        for (; index < prebufferCount; index++) {
            const unit = scanned.accessUnits[index];
            realtimeNode.pushAccessUnit(bytes.slice(unit.offset, unit.offset + unit.length));
        }
        await realtimeNode.whenIdle();
        realtimeNode.node.connect(realtimeAudioCtx.destination);

        // Paces the rest to roughly real time by each access unit's own
        // coded duration, so this behaves like a live feed rather than
        // dumping the whole file into the ring buffer at once. This is a
        // demo simplification, not something a real integration should
        // copy: a browser throttles setTimeout heavily in a backgrounded
        // tab, which can starve this loop while the (unthrottled) audio
        // graph keeps consuming - a real client instead pushes units as its
        // own transport delivers them (see the hls.js bridge, bindings/js/src/
        // hls-bridge.ts), which isn't driven by a synthetic per-frame timer
        // at all.
        const pushNext = () => {
            if (index >= scanned.accessUnits.length) {
                realtimeNode.flush();
                setRealtimeStatus('Bundled stream finished - stop and start again to replay.');
                return;
            }
            const unit = scanned.accessUnits[index++];
            realtimeNode.pushAccessUnit(bytes.slice(unit.offset, unit.offset + unit.length));
            // 1536/48000s per access unit is this fixture's own frame size;
            // a real feed would instead push as units arrive from its own
            // transport.
            realtimePushTimer = setTimeout(pushNext, (1536 / scanned.sampleRate) * 1000);
        };
        pushNext();

        el('realtimeStop').disabled = false;
    } catch (err) {
        setRealtimeStatus(`Failed to start: ${err.message}`);
        el('realtimeStart').disabled = false;
    }
}

function stopRealtime() {
    if (realtimePushTimer) clearTimeout(realtimePushTimer);
    realtimePushTimer = null;
    realtimeNode?.close();
    realtimeNode = null;
    realtimeAudioCtx?.close();
    realtimeAudioCtx = null;
    el('realtimeStart').disabled = false;
    el('realtimeStop').disabled = true;
    setRealtimeStatus('Stopped.');
}

window.addEventListener('DOMContentLoaded', () => {
    el('loadDemo').addEventListener('click', loadBundledDemo);
    el('fileInput').addEventListener('change', (e) => {
        if (e.target.files.length > 0) loadFile(e.target.files[0]);
    });
    el('playPause').addEventListener('click', () => {
        if (playing) pause(); else play();
    });
    el('karaokeChoice').addEventListener('change', onKaraokeChange);
    el('micToggle').addEventListener('click', toggleMicrophone);
    el('micHeadphones').addEventListener('change', (e) => {
        if (micSession) micSession.headphones = e.target.checked;
        scheduleMicDraw();
    });
    el('micDelay').addEventListener('input', (e) => {
        el('micDelayText').textContent = `${Number(e.target.value)} ms`;
        if (micSession) micSession.inputLatencySec = Number(e.target.value) / 1000;
    });
    el('micOffset').addEventListener('input', (e) => {
        const ms = Number(e.target.value);
        el('micOffsetText').textContent = `${ms > 0 ? '+' : ''}${ms} ms`;
        if (micSession) micSession.offsetMs = ms;
    });
    el('lyricsInput').addEventListener('change', (e) => {
        if (e.target.files.length > 0) void loadLyrics(e.target.files[0]);
    });
    el('lyricsDelay').addEventListener('input', (e) => {
        lyricsDelayMs = Number(e.target.value);
        el('lyricsDelayText').textContent = `${lyricsDelayMs > 0 ? '+' : ''}${lyricsDelayMs} ms`;
        lyricsShownKey = '';
    });
    const seek = el('seek');
    seek.addEventListener('pointerdown', () => { seekDragging = true; });
    seek.addEventListener('input', () => {
        // Live-update the static frame while dragging, without restarting
        // audio on every intermediate tick - only on release (see 'change').
        if (!decoded) return;
        playStartOffset = (Number(seek.value) / 1000) * decoded.durationSeconds;
        el('scrubTime').textContent = `${playStartOffset.toFixed(1)}s / ${decoded.durationSeconds.toFixed(1)}s`;
    });
    seek.addEventListener('change', () => {
        seekDragging = false;
        seekTo(Number(seek.value) / 1000);
    });

    if (window.crossOriginIsolated) {
        el('realtimeStart').disabled = false;
    } else {
        setRealtimeStatus(
            'Disabled: this page is not cross-origin isolated (needs COOP: same-origin / ' +
            'COEP: require-corp response headers for SharedArrayBuffer).');
    }
    el('realtimeStart').addEventListener('click', startRealtime);
    el('realtimeStop').addEventListener('click', stopRealtime);

    requestAnimationFrame(draw);
    requestAnimationFrame(drawObjects);
    requestAnimationFrame(drawLyrics);
});
