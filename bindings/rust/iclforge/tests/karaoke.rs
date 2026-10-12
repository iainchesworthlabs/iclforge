//! Annex C karaoke through the Rust binding: the decoder's choice of reproduction and the lyrics
//! reader, held against the committed karaoke examples (`examples/karaoke/`) - a real 3/2
//! karaoke stream and the words that go with it, so that nothing here is a signal made up for
//! the test.
//!
//! What a reproduction *is*, level for level, is held by libs/ac3's output-stage tests and
//! libs/capi's tests; these hold that the Rust surface reaches it in the library's own terms.

use iclforge::ac3::Decoder;
use iclforge::lyrics::{Lyrics, LyricsError, Singer, WordState};
use iclforge::stream::split_frames;
use iclforge::types::{
    Acmod, DecoderConfig, KaraokeConfig, KaraokeReproduction, KaraokeVocals, KARAOKE_MAX_LEVEL_DB,
    KARAOKE_MIN_LEVEL_DB,
};
use std::path::PathBuf;

fn example(directory: &str, name: &str) -> Vec<u8> {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../examples/karaoke")
        .join(directory)
        .join(name);
    std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

/// Eight seconds of the example's frames: sung material in every part, not its silent head.
const FRAMES: usize = 200;

fn karaoke_stream() -> Vec<u8> {
    example("auld-lang-syne", "auld-lang-syne.ac3")
}

/// Decodes the first `FRAMES` frames of the example with `decoder`, one `Vec` of planar channels
/// per frame.
fn decode(decoder: &mut Decoder, stream: &[u8]) -> Vec<Vec<Vec<f32>>> {
    let frames = split_frames(stream).unwrap();
    assert!(
        frames.len() >= FRAMES,
        "the example is longer than {FRAMES} frames"
    );
    frames[..FRAMES]
        .iter()
        .map(|frame| {
            let decoded = decoder.decode_frame(frame).unwrap();
            (0..decoded.channel_count())
                .map(|c| decoded.channel_samples(c).to_vec())
                .collect()
        })
        .collect()
}

fn peak(frames: &[Vec<Vec<f32>>], channel: usize) -> f32 {
    frames
        .iter()
        .flat_map(|f| f[channel].iter())
        .fold(0.0f32, |m, s| m.max(s.abs()))
}

#[test]
fn karaoke_config_defaults_are_the_librarys() {
    let config = KaraokeConfig::default();
    assert_eq!(config.reproduction, KaraokeReproduction::Off);
    assert_eq!(config.vocals, KaraokeVocals::Both);
    assert_eq!(config.melody_db, 0.0);
    assert_eq!(config.vocal1_db, 0.0);
    assert_eq!(config.vocal2_db, 0.0);
    assert_eq!(KARAOKE_MIN_LEVEL_DB, -24.0);
    assert_eq!(KARAOKE_MAX_LEVEL_DB, 12.0);
}

#[test]
fn a_plain_decoder_leaves_a_karaoke_stream_as_coded_and_says_what_it_is() {
    let stream = karaoke_stream();
    let first = split_frames(&stream).unwrap()[0];
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
    let frame = decoder.decode_frame(first).unwrap();
    assert_eq!(frame.bsmod(), 7);
    assert!(frame.is_karaoke());
    assert_eq!(frame.acmod(), Some(Acmod::Channels3_2));
    assert_eq!(frame.channel_count(), 5);
    assert_eq!(frame.output_acmod(), Some(Acmod::Channels3_2));
    assert!(!frame.output_lfe());
    assert!(!frame.karaoke_reproduced());
}

#[test]
fn the_aware_decoder_reproduces_three_channels_where_five_were_coded() {
    let stream = karaoke_stream();
    let mut decoder = Decoder::with_karaoke(
        &DecoderConfig::default(),
        &KaraokeConfig {
            reproduction: KaraokeReproduction::Aware,
            ..Default::default()
        },
    )
    .unwrap();
    let first = split_frames(&stream).unwrap()[0];
    let frame = decoder.decode_frame(first).unwrap();
    assert!(frame.is_karaoke());
    assert!(frame.karaoke_reproduced());
    assert_eq!(frame.acmod(), Some(Acmod::Channels3_2)); // what was coded
    assert_eq!(frame.channel_count(), 3);
    assert_eq!(frame.output_acmod(), Some(Acmod::Channels3_0));
    assert!(!frame.output_lfe());
}

#[test]
fn the_capable_decoders_vocals_and_levels_change_what_is_heard() {
    let stream = karaoke_stream();
    let capable = |vocals, melody_db: f64| {
        let mut decoder = Decoder::with_karaoke(
            &DecoderConfig::default(),
            &KaraokeConfig {
                reproduction: KaraokeReproduction::Capable,
                vocals,
                melody_db,
                ..Default::default()
            },
        )
        .unwrap();
        decode(&mut decoder, &stream)
    };

    let none = capable(KaraokeVocals::None, 0.0);
    let v1 = capable(KaraokeVocals::V1, 0.0);
    // L, C, R in coded order. With no vocals the centre is the melody alone and nothing moves;
    // choosing V1 puts the vocal into it.
    assert_eq!(none[0].len(), 3);
    let none_peak = peak(&none, 1);
    assert!(
        none_peak > 0.05,
        "the melody is real signal, got {none_peak}"
    );
    assert!(none != v1, "choosing V1 changed nothing");

    // The melody 6 dB down moves only the centre, by exactly that: the sums are 1, 1 and the
    // gain, so nothing is scaled down.
    let down = capable(KaraokeVocals::None, -6.0);
    let gain = 10f32.powf(-6.0 / 20.0);
    let mut worst = 0.0f32;
    for (a, b) in none.iter().zip(&down) {
        for i in 0..a[0].len() {
            worst = worst.max((a[0][i] - b[0][i]).abs());
            worst = worst.max((a[2][i] - b[2][i]).abs());
            worst = worst.max((a[1][i] * gain - b[1][i]).abs());
        }
    }
    assert!(worst < 1e-5, "worst deviation {worst}");

    // The bottom of the span is off.
    let off = capable(KaraokeVocals::None, KARAOKE_MIN_LEVEL_DB);
    assert_eq!(peak(&off, 1), 0.0);
    assert!(peak(&off, 0) > 0.0);
}

#[test]
fn an_enum_the_c_library_would_refuse_cannot_be_built() {
    // The Rust enums cannot name a reproduction outside the library's three, so the only ways to
    // a refusal are the pointers (impossible from safe Rust) and this: a stream that is not
    // karaoke is simply left alone whatever the decoder was asked for.
    let mut decoder = Decoder::with_karaoke(
        &DecoderConfig::default(),
        &KaraokeConfig {
            reproduction: KaraokeReproduction::Capable,
            vocals: KaraokeVocals::V2,
            ..Default::default()
        },
    )
    .unwrap();
    let encoder_config = iclforge::ac3::EncoderConfig {
        acmod: Acmod::Stereo,
        ..Default::default()
    };
    let mut encoder = iclforge::ac3::Encoder::new(&encoder_config).unwrap();
    let silence = vec![0.0f32; iclforge::SAMPLES_PER_FRAME];
    let coded = encoder.encode_frame(&[&silence, &silence]).unwrap();
    let frame = decoder.decode_frame(&coded).unwrap();
    assert!(!frame.is_karaoke());
    assert!(!frame.karaoke_reproduced());
    assert_eq!(frame.channel_count(), 2);
    assert_eq!(frame.output_acmod(), Some(Acmod::Stereo));
}

#[test]
fn the_example_lyrics_read_as_a_word_timed_song() {
    let bytes = example("auld-lang-syne", "auld-lang-syne.lrc");
    let lyrics = Lyrics::parse(&bytes, Some(16_500)).unwrap();
    assert_eq!(lyrics.title(), "Auld Lang Syne");
    assert_eq!(lyrics.album(), "iclforge karaoke example");
    assert_eq!(lyrics.length_ms(), Some(16_000));
    assert!(lyrics.is_timed());
    assert!(!lyrics.has_singers());

    // Four sung lines and the time that only marks the end of the last.
    let sung: Vec<_> = lyrics.lines().filter(|l| !l.text().is_empty()).collect();
    assert_eq!(sung.len(), 4);
    let first = &sung[0];
    assert_eq!(first.start_ms(), 0);
    assert_eq!(first.end_ms(), 4_000);
    assert_eq!(first.text(), "Should auld acquaintance be forgot,");
    assert_eq!(first.singer(), Singer::Unmarked);
    // Syllables are words: Should auld ac quain tance be for got,
    assert_eq!(first.word_count(), 8);
    let words: Vec<_> = first.words().collect();
    assert_eq!(words[0].start_ms(), 0);
    assert_eq!(words[1].start_ms(), 500);
    assert_eq!(words[2].start_ms(), 1_250);
    assert!(words[2].text().starts_with("ac"));
    // The joined words are the line's text.
    let joined: String = first.words().map(|w| w.text()).collect();
    assert_eq!(joined, first.text());

    // The cursor: 700 ms is 200 ms into "auld" (500 to 1250).
    let at = lyrics.position(700, 0);
    assert_eq!(at.line, Some(0));
    assert!(at.in_line);
    assert_eq!(at.word, Some(1));
    assert!((at.fraction - 200.0 / 750.0).abs() < 1e-9);
    assert_eq!(words[0].state(700), WordState::Sung);
    assert_eq!(words[1].state(700), WordState::BeingSung);
    assert_eq!(words[2].state(700), WordState::NotYetSung);
    // The singer's nudge: shown 300 ms later, so the position is 300 ms earlier.
    assert_eq!(lyrics.position(700, 300).position_ms, 400);
}

#[test]
fn a_duet_says_who_sings_each_line() {
    let bytes = example("leave-her-johnny", "leave-her-johnny.lrc");
    let lyrics = Lyrics::parse(&bytes, None).unwrap();
    assert_eq!(lyrics.first_singer_name(), "Shantyman");
    assert_eq!(lyrics.second_singer_name(), "Crew");
    assert!(lyrics.has_singers());
    let singers: Vec<_> = lyrics
        .lines()
        .filter(|l| !l.text().is_empty())
        .map(|l| l.singer())
        .collect();
    assert_eq!(
        singers,
        [Singer::First, Singer::Second, Singer::First, Singer::Second]
    );
    // The marker is not part of the text.
    assert!(lyrics.line(0).unwrap().text().starts_with("I thought"));
    assert!(lyrics.line(lyrics.line_count()).is_none());
}

#[test]
fn bytes_that_are_not_lyrics_are_refused_with_the_reason() {
    match Lyrics::parse(&[], None) {
        Err(LyricsError::Unreadable(reason)) => assert!(!reason.is_empty()),
        other => panic!("expected an unreadable refusal, got {other:?}"),
    }
    // Latin-1 is named and not guessed at.
    let latin1 = b"[00:01.00]caf\xE9\n";
    match Lyrics::parse(latin1, None) {
        Err(LyricsError::Unreadable(reason)) => assert!(reason.to_lowercase().contains("utf")),
        other => panic!("expected an unreadable refusal, got {other:?}"),
    }
}

#[test]
fn utf16_lyrics_come_back_as_utf8() {
    // "shalom", the reason UTF-16 is read at all: what an editor saves as "Unicode".
    let text = "[ti:\u{5E9}\u{5DC}\u{5D5}\u{5DD}]\n[00:01.00]\u{5E9}\u{5DC}\u{5D5}\u{5DD}\n";
    let mut bytes = vec![0xFF, 0xFE];
    for unit in text.encode_utf16() {
        bytes.extend_from_slice(&unit.to_le_bytes());
    }
    let lyrics = Lyrics::parse(&bytes, None).unwrap();
    assert_eq!(lyrics.title(), "\u{5E9}\u{5DC}\u{5D5}\u{5DD}");
    assert_eq!(
        lyrics.line(0).unwrap().text(),
        "\u{5E9}\u{5DC}\u{5D5}\u{5DD}"
    );
}
