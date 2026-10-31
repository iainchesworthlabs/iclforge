//! `iclforge::truehd` — the TrueHD and MLP decoder through the Rust binding.
//!
//! The committed FFmpeg fixtures (`testdata/truehd`, made by `tools/generators/gen_truehd_ffmpeg.py`)
//! are the oracle: the codec is lossless, so a decode returns the integers the stream was made from
//! and the SHA-256 of what FFmpeg's strict decode of the same file gives is the SHA-256 of ours.
//! The same equality is held to `forge decode` when `ICLFORGE_FORGE` names an executable, so the
//! binding, the command line and FFmpeg all give the same samples.

use iclforge::truehd::{self, Decoder, DecoderConfig, ErrorKind, Presentation, ScannedStream};
use iclforge::Error;
use std::path::PathBuf;

// --- the fixtures ----------------------------------------------------------------------------

struct Fixture {
    name: String,
    file: String,
    codec: String,
    sample_rate: u32,
    channels: usize,
    access_units: usize,
    stream_bytes: usize,
    pcm_samples: usize,
    pcm_sha256: String,
}

fn directory() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../testdata/truehd")
        .canonicalize()
        .expect("testdata/truehd")
}

/// The field `key` of a manifest entry, as the text after `"key": `, up to the next comma or newline.
fn field<'a>(entry: &'a str, key: &str) -> &'a str {
    let at = entry
        .find(&format!("\"{key}\":"))
        .unwrap_or_else(|| panic!("no {key} in {entry}"));
    let rest = entry[at + key.len() + 3..].trim_start();
    let end = rest.find([',', '\n']).unwrap_or(rest.len());
    rest[..end].trim().trim_matches('"')
}

/// The `fixtures` of manifest.json, without a JSON parser: each entry starts at its `"name"`, and
/// `spliced` (streams the generic tests do not decode) follows the list.
fn fixtures() -> Vec<Fixture> {
    let text = std::fs::read_to_string(directory().join("manifest.json")).unwrap();
    let text = &text[..text.find("\"spliced\"").unwrap_or(text.len())];
    text.split("\"name\":")
        .skip(1)
        .map(|entry| {
            let entry = format!("\"name\":{entry}");
            let number = |key: &str| field(&entry, key).parse::<usize>().unwrap();
            Fixture {
                name: field(&entry, "name").to_string(),
                file: field(&entry, "file").to_string(),
                codec: field(&entry, "codec").to_string(),
                sample_rate: number("sample_rate") as u32,
                channels: number("channels"),
                access_units: number("access_units"),
                stream_bytes: number("stream_bytes"),
                pcm_samples: number("pcm_samples"),
                pcm_sha256: field(&entry, "pcm_sha256").to_string(),
            }
        })
        .collect()
}

fn stream_of(fixture: &Fixture) -> Vec<u8> {
    std::fs::read(directory().join(&fixture.file)).unwrap()
}

fn named(name: &str) -> Fixture {
    fixtures().into_iter().find(|f| f.name == name).unwrap()
}

// --- SHA-256 (FIPS 180-4), for the manifest's hashes; the crate has no dependencies ----------

fn sha256_hex(data: &[u8]) -> String {
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    ];
    let mut state: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
        0x5be0cd19,
    ];
    let mut message = data.to_vec();
    message.push(0x80);
    while message.len() % 64 != 56 {
        message.push(0);
    }
    message.extend_from_slice(&((data.len() as u64) * 8).to_be_bytes());
    for block in message.chunks(64) {
        let mut w = [0u32; 64];
        for (i, word) in block.chunks(4).enumerate() {
            w[i] = u32::from_be_bytes([word[0], word[1], word[2], word[3]]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = state;
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ (!e & g);
            let t1 = h
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(K[i])
                .wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        for (slot, value) in state.iter_mut().zip([a, b, c, d, e, f, g, h]) {
            *slot = slot.wrapping_add(value);
        }
    }
    state.iter().map(|word| format!("{word:08x}")).collect()
}

#[test]
fn the_sha256_helper_matches_the_standard_vectors() {
    assert_eq!(
        sha256_hex(b"abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
    assert_eq!(
        sha256_hex(b""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    );
    assert_eq!(
        sha256_hex(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
    );
}

/// What FFmpeg's `-f s32le` writes for 24-bit samples: each shifted left by 8, little-endian.
fn ffmpeg_s32le(pcm: &[i32]) -> Vec<u8> {
    pcm.iter()
        .flat_map(|sample| (sample << 8).to_le_bytes())
        .collect()
}

/// Every access unit of `stream` through a decoder, the samples joined.
fn decode_all(stream: &[u8], config: &DecoderConfig) -> (Vec<i32>, usize) {
    let units = truehd::scan(stream).unwrap();
    let mut decoder = Decoder::new(config).unwrap();
    let mut pcm = Vec::new();
    let mut channels = 0;
    for unit in &units.units {
        if let Some(decoded) = decoder.decode(unit).unwrap() {
            channels = decoded.channels();
            pcm.extend_from_slice(decoded.pcm());
        }
    }
    (pcm, channels)
}

// --- the tests -------------------------------------------------------------------------------

#[test]
fn there_are_fixtures_to_hold_the_binding_to() {
    let all = fixtures();
    assert!(all.len() >= 20, "{} fixtures", all.len());
    assert!(all.iter().any(|f| f.codec == "mlp"));
}

#[test]
fn decode_gives_ffmpegs_samples() {
    for fixture in fixtures() {
        let stream = stream_of(&fixture);
        let (pcm, channels) = decode_all(&stream, &DecoderConfig::default());
        assert_eq!(channels, fixture.channels, "{}", fixture.name);
        assert_eq!(
            pcm.len(),
            fixture.pcm_samples * fixture.channels,
            "{}",
            fixture.name
        );
        assert_eq!(
            sha256_hex(&ffmpeg_s32le(&pcm)),
            fixture.pcm_sha256,
            "{}",
            fixture.name
        );
        // 24 bits, sign-extended.
        assert!(
            pcm.iter().all(|&s| (-(1 << 23)..(1 << 23)).contains(&s)),
            "{}",
            fixture.name
        );
    }
}

#[test]
fn probe_and_scan_state_what_the_manifest_states() {
    for fixture in fixtures() {
        let stream = stream_of(&fixture);
        let info = truehd::probe(&stream).unwrap();
        assert_eq!(info.is_mlp, fixture.codec == "mlp", "{}", fixture.name);
        assert_eq!(info.sample_rate, fixture.sample_rate, "{}", fixture.name);
        assert_eq!(info.access_units, fixture.access_units, "{}", fixture.name);
        assert_eq!(
            info.channels[info.substreams as usize - 1] as usize,
            fixture.channels,
            "{}",
            fixture.name
        );
        assert_eq!(info.first_unit_offset, 0, "{}", fixture.name);

        let units = truehd::scan(&stream).unwrap();
        assert_eq!(units.units.len(), fixture.access_units, "{}", fixture.name);
        assert_eq!(units.skipped_bytes, 0, "{}", fixture.name);
        assert_eq!(
            units.units.iter().map(|u| u.len()).sum::<usize>(),
            fixture.stream_bytes,
            "{}",
            fixture.name
        );
        assert_eq!(units.units.concat(), stream, "{}", fixture.name);

        let scanned = ScannedStream::new(&stream, 0).unwrap();
        assert_eq!(
            scanned.sample_rate(),
            fixture.sample_rate,
            "{}",
            fixture.name
        );
        assert_eq!(
            scanned.units() as usize,
            fixture.access_units,
            "{}",
            fixture.name
        );
        assert_eq!(
            scanned.samples() as usize,
            fixture.pcm_samples,
            "{}",
            fixture.name
        );
        assert_eq!(scanned.skipped_bytes(), 0, "{}", fixture.name);
        assert_eq!(scanned.leading_bytes(), 0, "{}", fixture.name);
        assert_eq!(
            scanned.samples() + u64::from(scanned.padding_samples()),
            scanned.units() * u64::from(scanned.samples_per_unit()),
            "{}",
            fixture.name
        );
        let duration = fixture.pcm_samples as f64 / f64::from(fixture.sample_rate);
        assert!(
            (scanned.duration_seconds() - duration).abs() < 1e-9,
            "{}",
            fixture.name
        );
        assert_eq!(scanned.restarts()[0].unit, 0, "{}", fixture.name);
        assert_eq!(scanned.restarts()[0].offset, 0, "{}", fixture.name);
    }
}

#[test]
fn a_decode_from_the_restart_for_a_sample_gives_that_sample() {
    for fixture in fixtures() {
        let stream = stream_of(&fixture);
        let scanned = ScannedStream::new(&stream, 1).unwrap();
        let (whole, channels) = decode_all(&stream, &DecoderConfig::default());
        let last = scanned.samples();
        for sample in [0, 1, last / 3, last / 2 + 7, last - 1] {
            let restart = scanned.restart_for_sample(sample).expect(&fixture.name);
            let first = restart.unit * u64::from(scanned.samples_per_unit());
            assert!(first <= sample, "{}", fixture.name);
            let (joined, _) = decode_all(scanned.stream_from(restart), &DecoderConfig::default());
            let at = (sample - first) as usize * channels;
            let want = sample as usize * channels;
            assert_eq!(
                joined[at..at + channels],
                whole[want..want + channels],
                "{} sample {sample}",
                fixture.name
            );
        }
    }
}

#[test]
fn the_scan_places_time_and_samples() {
    let fixture = named("truehd_st48_s24");
    let stream = stream_of(&fixture);
    let scanned = ScannedStream::new(&stream, 64).unwrap();
    let units: Vec<u64> = scanned.restarts().iter().map(|r| r.unit).collect();
    assert_eq!(units, [0, 64, 128, 192, 256]);
    assert_eq!(scanned.samples_per_unit(), 40);
    assert!(scanned.restart_for_sample(scanned.samples()).is_none());
    assert_eq!(scanned.restart_for_sample(64 * 40).unwrap().unit, 64);
    assert_eq!(scanned.sample_at_seconds(0.125), 6000);
    assert_eq!(scanned.sample_at_seconds(-2.0), 0);
    // A spacing of 0 keeps every unit with a major sync, as 1 does.
    assert_eq!(ScannedStream::new(&stream, 0).unwrap().restarts().len(), 19);
}

#[test]
fn units_one_at_a_time_carry_their_own_description() {
    let fixture = named("truehd_m51_s24");
    let stream = stream_of(&fixture);
    let units = truehd::scan(&stream).unwrap();
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
    let mut total = 0;
    for unit in &units.units {
        let decoded = decoder
            .decode(unit)
            .unwrap()
            .expect("a fixture starts at a major sync");
        assert_eq!(decoded.pcm().len(), decoded.samples() * decoded.channels());
        assert_eq!(decoded.sample_rate(), fixture.sample_rate);
        assert_eq!(decoded.padding_samples(), 0);
        total += decoded.samples();
    }
    assert_eq!(total, fixture.pcm_samples);
    let counters = decoder.counters();
    assert_eq!(counters.units as usize, units.units.len());
    assert_eq!(counters.units_decoded as usize, units.units.len());
    assert_eq!(counters.check_failures, 0);
}

#[test]
fn a_decoded_unit_owns_its_samples() {
    let stream = stream_of(&named("truehd_st48_s24"));
    let units = truehd::scan(&stream).unwrap().units;
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
    let first = decoder.decode(units[0]).unwrap().unwrap();
    let before = first.pcm().to_vec();
    // The next decodes, and the decoder itself, do not touch what `first` holds.
    for unit in &units[1..8] {
        decoder.decode(unit).unwrap();
    }
    drop(decoder);
    assert_eq!(first.pcm(), before);
    assert!(first.samples() > 0);
}

#[test]
fn a_decoded_unit_names_its_channels() {
    let stream = stream_of(&named("truehd_m51_s24"));
    let units = truehd::scan(&stream).unwrap();
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
    let decoded = decoder.decode(units.units[0]).unwrap().unwrap();
    assert_eq!(decoded.channels(), 6);
    let names: Vec<_> = (0..6).map(|i| decoded.channel_name(i)).collect();
    assert_eq!(
        names,
        [
            Some("L"),
            Some("R"),
            Some("C"),
            Some("LFE"),
            Some("Ls"),
            Some("Rs")
        ]
    );
    assert_eq!(decoded.channel_name(6), None);
    assert!(decoded.restarted());
    assert_eq!(decoded.substream(), 1);
}

#[test]
fn a_decoder_that_joins_late_waits_for_a_restart_and_a_lost_unit_makes_it_wait() {
    let stream = stream_of(&named("truehd_st48_s24"));
    let units = truehd::scan(&stream).unwrap().units;
    let scanned = ScannedStream::new(&stream, 1).unwrap();
    let restarts: Vec<u64> = scanned.restarts().iter().map(|r| r.unit).collect();
    assert_eq!(restarts[..3], [0, 16, 32]);

    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
    // Joined at unit 5, between restarts: nothing to give until the major sync at 16.
    for unit in &units[5..16] {
        assert!(decoder.decode(unit).unwrap().is_none());
    }
    assert!(decoder.decode(units[16]).unwrap().is_some());
    assert!(decoder.decode(units[17]).unwrap().is_some());

    decoder.lost_data();
    assert!(decoder.decode(units[18]).unwrap().is_none());
    assert!(decoder.decode(units[32]).unwrap().is_some());
    decoder.reset();
    assert!(decoder.decode(units[33]).unwrap().is_none());
    assert!(decoder.counters().units_waiting > 0);
}

#[test]
fn presentation_picks_the_substreams_read() {
    let fixture = named("truehd_m51_s24");
    let stream = stream_of(&fixture);
    let (best, best_channels) = decode_all(&stream, &DecoderConfig::default());
    assert_eq!(best_channels, fixture.channels);
    let two = DecoderConfig {
        presentation: Presentation::TwoChannel,
        ..Default::default()
    };
    let (two_pcm, two_channels) = decode_all(&stream, &two);
    assert_eq!(two_channels, 2);
    assert_eq!(two_pcm.len() / 2, best.len() / best_channels);
}

#[test]
fn the_default_config_is_the_libraries_own() {
    let config = DecoderConfig::default();
    assert_eq!(config.presentation, Presentation::Best);
    assert!(!config.lenient);
}

#[test]
fn bytes_ahead_of_the_stream_are_counted_not_decoded() {
    let stream = stream_of(&named("truehd_st48_s24"));
    let (clean, _) = decode_all(&stream, &DecoderConfig::default());
    let mut shifted = vec![0x55; 37];
    shifted.extend_from_slice(&stream);
    let (pcm, _) = decode_all(&shifted, &DecoderConfig::default());
    assert_eq!(pcm, clean);
    let scanned = ScannedStream::new(&shifted, 0).unwrap();
    assert_eq!(scanned.leading_bytes(), 37);
    assert_eq!(scanned.skipped_bytes(), 37);
    assert_eq!(scanned.restarts()[0].offset, 37);
    assert_eq!(truehd::probe(&shifted).unwrap().first_unit_offset, 37);
    assert_eq!(truehd::scan(&shifted).unwrap().skipped_bytes, 37);
}

#[test]
fn what_is_not_a_stream_is_a_bad_sync() {
    let junk = [0x5Au8; 4096];
    let bad_sync = Error::Truehd(ErrorKind::BadSync);
    assert_eq!(truehd::probe(&junk).unwrap_err(), bad_sync);
    assert_eq!(truehd::scan(&junk).unwrap_err(), bad_sync);
    assert_eq!(ScannedStream::new(&junk, 0).unwrap_err(), bad_sync);
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
    assert!(decoder.decode(&junk[..64]).is_err());
    assert!(!decoder.last_error_detail().is_empty());
}

#[test]
fn a_corrupt_unit_stops_a_strict_decode_and_is_counted_by_a_lenient_one() {
    let stream = stream_of(&named("truehd_st48_s24"));
    let units = truehd::scan(&stream).unwrap().units;
    let mut damaged = units[3].to_vec();
    damaged[0] ^= 0xF0; // the check nibble of the unit's header (TH spec 3.2)

    let mut strict = Decoder::new(&DecoderConfig::default()).unwrap();
    for unit in &units[..3] {
        assert!(strict.decode(unit).unwrap().is_some());
    }
    assert_eq!(
        strict.decode(&damaged).unwrap_err(),
        Error::Truehd(ErrorKind::BadCheckNibble)
    );
    // Waiting for the next restart header.
    assert!(strict.decode(units[4]).unwrap().is_none());

    let mut lenient = Decoder::new(&DecoderConfig {
        lenient: true,
        ..Default::default()
    })
    .unwrap();
    for unit in &units[..3] {
        lenient.decode(unit).unwrap();
    }
    assert!(lenient.decode(&damaged).unwrap().is_some());
    assert!(lenient.counters().check_failures >= 1);
}

#[test]
fn a_scanned_stream_is_shareable_between_threads() {
    let stream = stream_of(&named("truehd_st48_s24"));
    let scanned = ScannedStream::new(&stream, 1).unwrap();
    std::thread::scope(|scope| {
        let handles: Vec<_> = (0..4)
            .map(|n| {
                let scanned = &scanned;
                scope.spawn(move || {
                    let sample = scanned.samples() * n / 4;
                    scanned.restart_for_sample(sample).unwrap().unit
                })
            })
            .collect();
        let units: Vec<u64> = handles.into_iter().map(|h| h.join().unwrap()).collect();
        assert!(units.windows(2).all(|pair| pair[0] <= pair[1]));
    });
}

// --- the same samples as `forge decode` ------------------------------------------------------

/// With `ICLFORGE_FORGE` set, `forge decode` of each fixture gives the 24-bit WAV whose samples
/// are the binding's. Skipped, with a note, when it is not.
#[test]
fn decode_equals_forge_decode() {
    let Some(forge) = std::env::var_os("ICLFORGE_FORGE") else {
        eprintln!("ICLFORGE_FORGE is not set: the comparison with forge decode is skipped");
        return;
    };
    let scratch = std::env::temp_dir().join(format!("iclforge-rust-truehd-{}", std::process::id()));
    std::fs::create_dir_all(&scratch).unwrap();
    for fixture in fixtures() {
        let stream = stream_of(&fixture);
        let (pcm, channels) = decode_all(&stream, &DecoderConfig::default());
        let input = scratch.join(&fixture.file);
        let output = scratch.join(format!("{}.wav", fixture.name));
        std::fs::write(&input, &stream).unwrap();
        let status = std::process::Command::new(&forge)
            .arg("decode")
            .arg(&input)
            .arg(&output)
            .output()
            .unwrap();
        assert!(status.status.success(), "forge decode {}", fixture.name);
        let wav = std::fs::read(&output).unwrap();
        let data = wav
            .windows(4)
            .position(|w| w == b"data")
            .map(|at| at + 8)
            .expect("a data chunk");
        let theirs: Vec<i32> = wav[data..]
            .as_chunks::<3>()
            .0
            .iter()
            .map(|b| i32::from_le_bytes([0, b[0], b[1], b[2]]) >> 8)
            .collect();
        assert_eq!(channels, fixture.channels);
        assert_eq!(theirs, pcm, "{}", fixture.name);
    }
    let _ = std::fs::remove_dir_all(&scratch);
}
