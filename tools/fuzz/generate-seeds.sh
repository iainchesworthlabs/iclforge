#!/usr/bin/env bash
# tools/fuzz/generate-seeds.sh - grow a fuzzing corpus from iclforge's own valid
# output, by running forge across the layout/codec/tool matrix this project
# already supports. Cheaper and more representative than hand-written corpus
# files: every seed here is a real, self-consistent stream this project can
# actually produce, so a fuzzer's mutations start from "almost valid" rather
# than from nothing.
#
# Needs a built forge. Any working configuration will do: this generates
# plain valid streams, not instrumented ones, so it needs neither Clang nor
# sanitizers. The Windows MSVC leg is what gets used in practice, for no
# better reason than that it is the one already built on the development
# host - every other leg builds clean too.
#
#   ICLFORGE_CLI_BIN=build/config-windows-msvc-debug/bin/forge.exe tools/fuzz/generate-seeds.sh
#
# Usage: tools/fuzz/generate-seeds.sh [output-dir]
#
# With no output-dir each harness's seeds go beside it, to libs/<lib>/fuzz/seeds/<harness>/
# (tools/fuzz/harnesses.sh); with one, every harness's go under it, a directory each.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=tools/fuzz/harnesses.sh
. "$SCRIPT_DIR/harnesses.sh"
ICLFORGE_CLI="${ICLFORGE_CLI_BIN:-}"
OUT_ROOT="${1:-}"

if [[ -z "$ICLFORGE_CLI" ]] || [[ ! -f "$ICLFORGE_CLI" ]]; then
    echo "error: set ICLFORGE_CLI_BIN to a built forge (see this script's header)" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
# The corpus is assembled here, a directory per harness, and copied out at the end.
OUT="$WORK/seeds"

mkdir -p "$OUT/fuzz_truehd_parse" "$OUT/fuzz_truehd_encode" "$OUT/fuzz_mat_unwrap" \
         "$OUT/fuzz_mp4_demux" "$OUT/fuzz_matroska_demux" "$OUT/fuzz_mpegts_demux" \
         "$OUT/fuzz_scan" "$OUT/fuzz_ac3_decode" "$OUT/fuzz_eac3_decode" "$OUT/fuzz_wav_read" \
         "$OUT/fuzz_signing_verify" "$OUT/fuzz_iec61937_unwrap" \
         "$OUT/fuzz_ac4_parse" "$OUT/fuzz_iab_parse" "$OUT/fuzz_content_signature" \
         "$OUT/fuzz_ac4_content_signature"

run() { "$ICLFORGE_CLI" "$@" >/dev/null; }

# add_seed <comma-separated dest names under $OUT> <file>
add_seed() {
    local dests="$1" file="$2" base names
    base="$(basename "$file")"
    IFS=',' read -ra names <<< "$dests"
    for name in "${names[@]}"; do
        cp "$file" "$OUT/$name/$base"
    done
}

# AC-4 has no encoder here, so its seed is the checked-in Dolby Encoding
# Engine baseline libs/ac4/tests already parses - a real stream from a real
# encoder, and a better starting corpus than anything this repo could
# synthesise for a format it only reads.
echo "==> AC-4: the checked-in DEE baseline, as a parser seed"
add_seed "fuzz_ac4_parse" "$REPO_ROOT/testdata/external-baseline/ac4-stereo-64/dee.ac4"

# Content signing: a short stream signed with a throwaway root and signer that openssl makes here
# and nothing keeps, and the parts of that signature as the two harnesses read them. fuzz_content_
# signature takes a mode byte first (see the harness): 0 Thrift, 1 DER, 2 text, 3 blob, 4 blob
# verified, 5 certificates, 6 base64. No private key is written to a seed: the one text seed that
# a key loader reads is a bare scalar of no value to anyone.
echo "==> AC-4 content signature: a short stream signed with a throwaway key, and its parts"
if command -v openssl >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    cs="$WORK/content-signature"
    mkdir -p "$cs"
    python3 - "$cs/tone.wav" <<'PY'
import math, struct, sys, wave
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(48000)
    w.writeframes(b"".join(
        struct.pack("<hh", int(6000 * math.sin(2 * math.pi * 440 * n / 48000)),
                    int(6000 * math.sin(2 * math.pi * 660 * n / 48000)))
        for n in range(6144)))
PY
    openssl ecparam -name prime256v1 -genkey -noout -out "$cs/root.key"
    openssl req -new -x509 -key "$cs/root.key" -sha256 -days 30 -subj "/CN=Seed Root CA" \
        -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign" \
        -out "$cs/root.pem"
    openssl ecparam -name prime256v1 -genkey -noout -out "$cs/signer.key"
    openssl req -new -key "$cs/signer.key" -sha256 \
        -subj "/CN=Seed Content Signer/title=Code\/Content Signer" -out "$cs/signer.csr"
    printf 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n' > "$cs/signer.ext"
    openssl x509 -req -in "$cs/signer.csr" -CA "$cs/root.pem" -CAkey "$cs/root.key" \
        -CAcreateserial -sha256 -days 30 -extfile "$cs/signer.ext" -out "$cs/signer.pem"
    cat "$cs/signer.pem" "$cs/root.pem" > "$cs/chain.pem"
    run ac4-encode "$cs/tone.wav" "$cs/plain.ac4" 64
    run ac4-sign "$cs/plain.ac4" "$cs/signed.ac4" content-key="$cs/signer.key" \
        content-chain="$cs/chain.pem"
    plain_size="$(wc -c < "$cs/plain.ac4")"
    signed_size="$(wc -c < "$cs/signed.ac4")"
    tail -c +"$((plain_size + 1))" "$cs/signed.ac4" > "$cs/blob.bin"
    cp "$cs/signed.ac4" "$OUT/fuzz_ac4_content_signature/ac4-signed-stereo-64.ac4"
    cp "$cs/plain.ac4" "$OUT/fuzz_ac4_content_signature/ac4-unsigned-stereo-64.ac4"
    head -c "$((signed_size - 700))" "$cs/signed.ac4" \
        > "$OUT/fuzz_ac4_content_signature/ac4-signed-cut-in-trailer.ac4"
    seed="$OUT/fuzz_content_signature"
    { printf '\000'; cat "$cs/blob.bin"; } > "$seed/thrift-blob.bin"
    { printf '\001'; openssl x509 -in "$cs/signer.pem" -outform DER; } > "$seed/der-signer.bin"
    { printf '\002'; cat "$cs/chain.pem"; } > "$seed/text-pem-chain.bin"
    { printf '\002'; head -c 32 /dev/zero | tr '\000' '\021'; } > "$seed/text-bare-scalar.bin"
    { printf '\003'; cat "$cs/blob.bin"; } > "$seed/blob-parse.bin"
    { printf '\004'; cat "$cs/blob.bin"; } > "$seed/blob-verify.bin"
    { printf '\005'; openssl x509 -in "$cs/root.pem" -outform DER
      openssl x509 -in "$cs/signer.pem" -outform DER; } > "$seed/certs-der-pair.bin"
    { printf '\006'; base64 -w0 "$cs/blob.bin"; } > "$seed/blob-base64.bin"
else
    echo "    openssl or python3 not found: the content signature seeds are left as committed"
fi

# The IAB seed comes from examples/encode_iab.cpp's own --write-fixture mode,
# the one tools/ci/run_codec_matrix.sh drives, so it needs that example built -
# which this script's ICLFORGE_CLI_BIN contract does not cover. It is committed
# instead; regenerate with:
#
#     encode_iab --write-fixture libs/iab/fuzz/seeds/fuzz_iab_parse/iab-bed-object.iab

echo "==> AC-3: silence, sine and orbit across every layout AC-3 can carry"
for layout in mono stereo 51 51c; do
    f="$WORK/ac3-sine-$layout.ac3"
    run sine "$f" 1 192 1000 50 "$layout"
    add_seed "fuzz_scan,fuzz_ac3_decode" "$f"
done
run silence "$WORK/ac3-silence.ac3" 1 192
add_seed "fuzz_scan,fuzz_ac3_decode" "$WORK/ac3-silence.ac3"
run orbit "$WORK/ac3-orbit.ac3" 1 448 2
add_seed "fuzz_scan,fuzz_ac3_decode" "$WORK/ac3-orbit.ac3"

echo "==> AC-3 -> WAV roundtrip, for real-audio encode input and WAV-reader seeds"
run decode "$WORK/ac3-sine-51.ac3" "$WORK/roundtrip-51.wav"
add_seed "fuzz_wav_read" "$WORK/roundtrip-51.wav"
run decode "$WORK/ac3-sine-stereo.ac3" "$WORK/roundtrip-stereo.wav"
add_seed "fuzz_wav_read" "$WORK/roundtrip-stereo.wav"

echo "==> AC-3: real-audio encode (silence/tones alone give false passes - see"
echo "    codec-validation-needs-real-audio)"
f="$WORK/ac3-encode-51.ac3"
run encode "$WORK/roundtrip-51.wav" "$f" 448 51
add_seed "fuzz_scan,fuzz_ac3_decode" "$f"

# 512 kbit/s stereo: 2048-byte syncframes, the top of AC-3's per-frame size
# range for 48 kHz, with most of each frame spent on §5.3.3 skip-field
# padding and 16-bit (bap 15) mantissas - a size-and-layout corner none of
# the 192/448 kbit/s seeds above reach. This shape surfaced the FFmpeg
# probe-window misdetection recorded in tools/ci/fuzz_encoder_space.py (the
# note above MIN_STREAM_BYTES, case seed 1124127684685913171): the stream is
# fully valid, so the decoder-side fuzzers should mutate from it too.
f="$WORK/ac3-encode-stereo-512.ac3"
run encode "$WORK/roundtrip-stereo.wav" "$f" 512 stereo
add_seed "fuzz_scan,fuzz_ac3_decode" "$f"

echo "==> E-AC-3: silence and sine across every layout"
for layout in mono stereo 51 71 512 514 714; do
    f="$WORK/eac3-sine-$layout.ec3"
    run eac3-sine "$f" 1 192 1000 50 "$layout"
    add_seed "fuzz_scan,fuzz_eac3_decode" "$f"
    f="$WORK/eac3-silence-$layout.ec3"
    run eac3-silence "$f" 1 192 "$layout"
    add_seed "fuzz_scan,fuzz_eac3_decode" "$f"
done

echo "==> E-AC-3: real-audio encode across every Annex E tool combination"
for tools in none cpl spx aht all "cpl+spx" "cpl:4+spx:5" "aht:0" "spx+noatten" "spx+atten:12"; do
    safe="${tools//[:+]/_}"
    for layout in 51 714; do
        f="$WORK/eac3-encode-${safe}-${layout}.ec3"
        run eac3-encode "$WORK/roundtrip-51.wav" "$f" 448 "$tools" "$layout"
        add_seed "fuzz_scan,fuzz_eac3_decode" "$f"
    done
done

echo "==> Atmos: JOC + OAMD object container, and the bed51 fallback (both are"
echo "    plain E-AC-3 syntax as far as the decoder entry points are concerned -"
echo "    see graceful-51-fallback-either-or)"
run atmos "$WORK/atmos-objects.ec3" 1 448 4 3 objects
add_seed "fuzz_scan,fuzz_eac3_decode" "$WORK/atmos-objects.ec3"
run atmos "$WORK/atmos-bed51.ec3" 1 448 4 3 bed51
add_seed "fuzz_scan,fuzz_eac3_decode" "$WORK/atmos-bed51.ec3"
run atmos-encode "$WORK/roundtrip-51.wav" "$WORK/atmos-encode.ec3" 448 0
add_seed "fuzz_scan,fuzz_eac3_decode" "$WORK/atmos-encode.ec3"

echo "==> Third-party structure: the committed external-baseline bitstreams"
echo "    (Dolby Encoding Engine 6.5.4 and FFmpeg 8.0.1 - third-party decode interop)"
# Everything above this point is iclforge's own output, so every seed shares
# iclforge's own encoder choices: frame-hoisted exponent strategies, coupling
# either on for all six blocks or off, snroffststr 0, frmfgaincode 0, geometry
# sent once in block 0 and never resent. A mutation starting from one of those
# explores the neighbourhood of THIS encoder's syntax, and no amount of it
# reaches syntax the encoder never emits in the first place. The external
# baseline is real third-party structure - a Dolby encoder resending coupling
# geometry mid-frame, bringing channels into coupling part-way through, and
# transmitting the per-channel fast-gain and fine-SNR elements this one leaves
# out - which is exactly the shape that carried five real decoder defects
# found while wiring up tools/checks/verify_gold_reference.sh's interop
# checks. Seeding from these puts the fuzzers' mutations in that neighbourhood
# too.
EXTERNAL_BASELINE="$REPO_ROOT/testdata/external-baseline"
for leg in ac3-51-448 eac3-51-256 eac3-stereo-192; do
    case "$leg" in
        ac3-*) ext=ac3; targets="fuzz_scan,fuzz_ac3_decode" ;;
        *)     ext=ec3; targets="fuzz_scan,fuzz_eac3_decode" ;;
    esac
    for tool in dee ffmpeg; do
        src="$EXTERNAL_BASELINE/$leg/$tool.$ext"
        if [[ ! -f "$src" ]]; then
            echo "error: external-baseline fixture missing: $src" >&2
            exit 1
        fi
        # Renamed on the way in: every fixture is called dee.ac3/ffmpeg.ec3
        # inside its own leg directory, and the corpus directories are flat.
        cp "$src" "$WORK/external-${leg}-${tool}.${ext}"
        add_seed "$targets" "$WORK/external-${leg}-${tool}.${ext}"
    done
done

echo "==> WAV: an IEC 61937 burst-wrapped PCM16 WAV too - a different write path"
run spdif "$WORK/ac3-silence.ac3" "$WORK/spdif.wav"
add_seed "fuzz_wav_read" "$WORK/spdif.wav"

echo "==> Object signing: the same Atmos streams, signed and unsigned, each"
echo "    prefixed with the key fuzz_signing_verify's input format expects"
# fuzz_signing_verify reads a length byte, that many key bytes, then the
# stream (see the harness's own header). A seed therefore has to be built,
# not just copied - and it needs to cover both verification outcomes, so one
# copy is signed with the key the seed carries (kValid) and one is not
# (kMismatch on every frame that has a container to check). The key itself is
# arbitrary throwaway bytes generated here: nothing in this project ships,
# needs or derives a real Dolby key, and verification works against whatever
# key it is handed.
KEY_FILE="$WORK/seed-signing.key"
head -c 16 /dev/urandom > "$KEY_FILE"
# Truncated to the first few syncframes rather than carrying the whole
# second of audio: libFuzzer takes its -max_len from the largest seed, and
# verify_atmos_frame's message-A reconstruction is linear in frame size (it
# walks every bit of the frame once per call), so a 33 KB seed would set the
# mutation length for the whole run and cost exec/s for no extra syntax. Four
# kilobytes is three complete 448 kbit/s frames plus a partial fourth - which
# is itself worth having, since a stream ending mid-frame is exactly what the
# framing walk's `off + size > stream.size()` break exists for.
signing_seed() {
    local name="$1" stream="$2"
    # `out` on its own line, not folded into the `local` above: every word of
    # a `local` is expanded before the builtin assigns any of them, so a
    # "$name" there would still read the caller's (here add_seed's leftover
    # loop variable), not this function's parameter.
    local out="$OUT/fuzz_signing_verify/$name"
    printf '\020' > "$out"       # key length: 16
    cat "$KEY_FILE" >> "$out"
    head -c 4096 "$stream" >> "$out"
}
run atmos "$WORK/atmos-signed.ec3" 1 448 4 3 objects sign-objects "signing-key=$KEY_FILE"
run atmos-encode "$WORK/roundtrip-51.wav" "$WORK/atmos-encode-signed.ec3" 448 0 \
    sign-objects "signing-key=$KEY_FILE"
signing_seed "atmos-objects-signed.bin" "$WORK/atmos-signed.ec3"
signing_seed "atmos-encode-signed.bin" "$WORK/atmos-encode-signed.ec3"
signing_seed "atmos-objects-unsigned.bin" "$WORK/atmos-objects.ec3"
signing_seed "atmos-encode-unsigned.bin" "$WORK/atmos-encode.ec3"
# The bed51 fallback carries no object container at all, so every frame
# reports kNoContainer - the third of verify_atmos_frame's three outcomes,
# and the one a plain non-Atmos stream takes.
signing_seed "atmos-bed51.bin" "$WORK/atmos-bed51.ec3"

echo "==> Metadata payloads: the EMDF containers, and the OAMD and JOC payloads"
echo "    inside them, extracted from the Atmos streams above (signing-verify fuzz walk)"
python3 "$SCRIPT_DIR/metadata-seeds.py" extract "$OUT" \
    "$WORK/atmos-objects.ec3" "$WORK/atmos-encode.ec3" "$WORK/atmos-bed51.ec3"

echo "==> ADM: BW64/RF64 fixtures for fuzz_adm_parse - synthesised rather than"
echo "    encoded, since nothing forge produces is an ADM file"
python3 "$SCRIPT_DIR/metadata-seeds.py" adm "$OUT"

echo "==> IEC 61937 carriers, for the burst de-framer (IEC 61937 de-framing)"
# Both data types and both burst periods: AC-3's 6144 bytes and E-AC-3's
# 24576. The WAV header stays on deliberately - unspdif walks the RIFF chunk
# list itself, and a fuzzer that only ever saw bare carrier bytes would never
# mutate the chunk sizes that walk trusts.
#
# Cut to the first eight bursts, unlike every other seed here, which is a
# whole file: a burst carrier is 6144 or 24576 bytes PER FRAME, so a second
# of E-AC-3 already comes to 1.5 MB, and libFuzzer deprioritizes large corpus
# entries anyway. Eight bursts exercises every part of the framing a hundred
# would. The cut lands on a burst boundary, so the last one is whole; the
# RIFF header left behind still declares the untruncated length, which is
# itself worth starting from - unspdif clamps a data chunk to what the file
# actually holds, and that clamp is exactly the kind of thing to mutate.
# 44 is the header write_wav_pcm16_raw emits.
run spdif "$WORK/ac3-sine-51.ac3" "$WORK/spdif-ac3-full.wav"
head -c $((44 + 8 * 6144)) "$WORK/spdif-ac3-full.wav" > "$WORK/spdif-ac3-51.wav"
add_seed "fuzz_iec61937_unwrap,fuzz_wav_read" "$WORK/spdif-ac3-51.wav"
run spdif "$WORK/eac3-sine-51.ec3" "$WORK/spdif-eac3-full.wav"
head -c $((44 + 8 * 24576)) "$WORK/spdif-eac3-full.wav" > "$WORK/spdif-eac3-51.wav"
add_seed "fuzz_iec61937_unwrap,fuzz_wav_read" "$WORK/spdif-eac3-51.wav"
# A plain PCM WAV too: "this is not a carrier" is a verdict the de-framer has
# to reach as reliably as it reaches the other one.
head -c 131072 "$WORK/roundtrip-stereo.wav" > "$WORK/carrier-not.wav"
add_seed "fuzz_iec61937_unwrap" "$WORK/carrier-not.wav"
# AC-4 (IEC 61937-14): the Dolby Encoding Engine's stereo stream, whose bursts
# are as long as its frames. 'forge spdif' does not pack AC-4 yet
# (planning/ac4.md, I1), so the seed script packs its first four frames.
python3 "$SCRIPT_DIR/metadata-seeds.py" ac4-carrier "$OUT" \
    "$REPO_ROOT/testdata/external-baseline/ac4-stereo-64/dee.ac4"

# TrueHD and MLP, from the committed fixtures (testdata/truehd) and nothing else: cuts of every
# stream for the reader, the decoder and the differential harness (which share fuzz_truehd_parse's
# seeds), the encoder's configuration space, MAT bursts of three of them made by 'forge spdif', and
# three streams wrapped by 'forge mp4', 'mkv' and 'ts'. All of it is more than the engine needs:
# the committed seeds are the subset that reaches different branches, which
#
#     tools/fuzz/run.sh merge fuzz_truehd_decode <the fuzz_truehd_parse directory written here>
#
# finds (and the same with each other harness), so that every file is there for a reason.
echo "==> TrueHD and MLP: the fixtures cut, the encoder's space, MAT bursts and containers"
python3 "$SCRIPT_DIR/truehd-seeds.py" streams "$OUT/fuzz_truehd_parse"
python3 "$SCRIPT_DIR/truehd-seeds.py" encoder "$OUT/fuzz_truehd_encode"
python3 "$SCRIPT_DIR/truehd-seeds.py" mat "$OUT/fuzz_mat_unwrap" --forge "$ICLFORGE_CLI"
python3 "$SCRIPT_DIR/truehd-seeds.py" containers "$OUT" --forge "$ICLFORGE_CLI"

echo "==> done:"
for d in "$OUT"/fuzz_*; do
    harness="$(basename "$d")"
    if [[ -n "$OUT_ROOT" ]]; then
        dest="$OUT_ROOT/$harness"
    else
        dest="$(fuzz_dir_of "$harness")/seeds/$harness"
    fi
    mkdir -p "$dest"
    cp "$d"/* "$dest"/
    printf '    %-20s %s files -> %s\n' "$harness" "$(find "$d" -type f | wc -l)" "${dest#"$REPO_ROOT"/}"
done
