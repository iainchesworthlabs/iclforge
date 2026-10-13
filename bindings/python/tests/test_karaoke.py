"""Annex C karaoke through the Python binding: the decoder's choice of reproduction and the
lyrics reader, held against the committed karaoke examples (examples/karaoke/) - a real 3/2
karaoke stream and the words that go with it, so that nothing here is a signal made up for the
test.

What a reproduction *is*, level for level, is held by libs/ac3's output-stage tests; these hold
that the Python surface reaches it in the library's own terms, and that what is read through it is
what the C++ reader read."""

from pathlib import Path

import iclforge as ac3
import numpy as np
import pytest

EXAMPLES = Path(__file__).resolve().parents[3] / "examples" / "karaoke"

# Eight seconds of the example: sung material in every part, not its silent head.
FRAMES = 200


def example(directory: str, name: str) -> bytes:
    return (EXAMPLES / directory / name).read_bytes()


@pytest.fixture(scope="module")
def frames() -> list[bytes]:
    stream = example("auld-lang-syne", "auld-lang-syne.ac3")
    split = ac3.split_frames(stream)
    assert len(split) >= FRAMES
    return [bytes(frame) for frame in split[:FRAMES]]


def decode(frames: list[bytes], **config: object) -> list[list[np.ndarray]]:
    decoder = ac3.FrameDecoder(ac3.DecoderConfig(**config))
    return [[np.array(channel) for channel in decoder.decode_frame(f).channels] for f in frames]


def peak(decoded: list[list[np.ndarray]], channel: int) -> float:
    return float(max(np.abs(frame[channel]).max() for frame in decoded))


def test_karaoke_choice_defaults_are_off_both_and_the_table() -> None:
    config = ac3.DecoderConfig()
    assert config.karaoke == ac3.KaraokeReproduction.kOff
    assert config.karaoke_vocals == ac3.KaraokeVocals.kBoth
    assert config.karaoke_melody_db == 0.0
    assert config.karaoke_vocal1_db == 0.0
    assert config.karaoke_vocal2_db == 0.0
    assert ac3.KARAOKE_MIN_LEVEL_DB == -24.0
    assert ac3.KARAOKE_MAX_LEVEL_DB == 12.0


def test_decoder_config_takes_the_choice_by_keyword_and_by_property() -> None:
    config = ac3.DecoderConfig(
        karaoke=ac3.KaraokeReproduction.kCapable,
        karaoke_vocals=ac3.KaraokeVocals.kV1,
        karaoke_melody_db=-6.0,
        karaoke_vocal2_db=3.0,
    )
    assert config.karaoke == ac3.KaraokeReproduction.kCapable
    assert config.karaoke_vocals == ac3.KaraokeVocals.kV1
    assert config.karaoke_melody_db == pytest.approx(-6.0)
    assert config.karaoke_vocal1_db == 0.0
    assert config.karaoke_vocal2_db == pytest.approx(3.0)

    config.karaoke_vocal1_db = -30.0  # off the span: held to its bottom, which is off
    assert config.karaoke_vocal1_db == ac3.KARAOKE_MIN_LEVEL_DB
    config.karaoke_melody_db = 40.0
    assert config.karaoke_melody_db == ac3.KARAOKE_MAX_LEVEL_DB
    config.karaoke_melody_db = float("nan")  # not a number is the table's own 0 dB
    assert config.karaoke_melody_db == 0.0

    with pytest.raises(TypeError):
        ac3.DecoderConfig(karaoke_nonsense=1)


def test_a_plain_decoder_leaves_a_karaoke_stream_as_coded_and_says_what_it_is(
    frames: list[bytes],
) -> None:
    decoded = ac3.FrameDecoder().decode_frame(frames[0])
    assert decoded.bsmod == 7
    assert decoded.is_karaoke
    assert decoded.acmod == ac3.Acmod.k3_2
    assert len(decoded.channels) == 5
    assert decoded.output_acmod == ac3.Acmod.k3_2
    assert not decoded.output_lfe
    assert not decoded.karaoke_reproduced
    assert decoded.channel_labels == ["L", "C", "R", "Ls", "Rs"]


def test_the_aware_decoder_reproduces_three_channels_where_five_were_coded(
    frames: list[bytes],
) -> None:
    decoder = ac3.FrameDecoder(ac3.DecoderConfig(karaoke=ac3.KaraokeReproduction.kAware))
    decoded = decoder.decode_frame(frames[0])
    assert decoded.is_karaoke
    assert decoded.karaoke_reproduced
    assert decoded.acmod == ac3.Acmod.k3_2  # what was coded
    assert len(decoded.channels) == 3
    assert decoded.output_acmod == ac3.Acmod.k3_0
    assert not decoded.output_lfe
    # The labels are the channels that are there, not the coded layout's five.
    assert decoded.channel_labels == ["L", "C", "R"]


def test_the_into_form_says_the_same(frames: list[bytes]) -> None:
    decoder = ac3.FrameDecoder(ac3.DecoderConfig(karaoke=ac3.KaraokeReproduction.kAware))
    reference = ac3.FrameDecoder(ac3.DecoderConfig(karaoke=ac3.KaraokeReproduction.kAware))
    out = np.zeros((ac3.MAX_AC3_CHANNELS, ac3.SAMPLES_PER_FRAME), dtype=np.float32)
    for frame in frames[:8]:
        into = decoder.decode_frame_into(frame, out)
        owned = reference.decode_frame(frame)
        assert into.karaoke_reproduced
        assert into.output_acmod == ac3.Acmod.k3_0
        for index, channel in enumerate(owned.channels):
            assert np.array_equal(out[index], np.array(channel))


def test_the_capable_decoders_vocals_and_levels_change_what_is_heard(frames: list[bytes]) -> None:
    capable = ac3.KaraokeReproduction.kCapable
    none = decode(frames, karaoke=capable, karaoke_vocals=ac3.KaraokeVocals.kNone)
    v1 = decode(frames, karaoke=capable, karaoke_vocals=ac3.KaraokeVocals.kV1)
    # L, C, R in coded order. With no vocals the centre is the melody alone; V1 puts the vocal in.
    assert len(none[0]) == 3
    assert peak(none, 1) > 0.05, "the melody is real signal"
    assert any(not np.array_equal(a[1], b[1]) for a, b in zip(none, v1, strict=True))

    # The melody 6 dB down moves only the centre, by exactly that: the sums are 1, 1 and the
    # gain, so nothing is scaled down.
    down = decode(
        frames,
        karaoke=capable,
        karaoke_vocals=ac3.KaraokeVocals.kNone,
        karaoke_melody_db=-6.0,
    )
    gain = 10 ** (-6.0 / 20.0)
    for a, b in zip(none, down, strict=True):
        np.testing.assert_allclose(b[0], a[0], atol=1e-6)
        np.testing.assert_allclose(b[2], a[2], atol=1e-6)
        np.testing.assert_allclose(b[1], a[1] * gain, atol=1e-6)

    # The bottom of the span is off: the centre is silent, and the rest is not.
    off = decode(
        frames,
        karaoke=capable,
        karaoke_vocals=ac3.KaraokeVocals.kNone,
        karaoke_melody_db=ac3.KARAOKE_MIN_LEVEL_DB,
    )
    assert peak(off, 1) == 0.0
    assert peak(off, 0) > 0.0


def test_a_stream_that_is_not_karaoke_is_left_alone_whatever_was_asked() -> None:
    encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0))
    silence = np.zeros((2, ac3.SAMPLES_PER_FRAME), dtype=np.float32)
    coded = encoder.encode_frame(silence)
    decoder = ac3.FrameDecoder(
        ac3.DecoderConfig(
            karaoke=ac3.KaraokeReproduction.kCapable, karaoke_vocals=ac3.KaraokeVocals.kV2
        )
    )
    decoded = decoder.decode_frame(coded)
    assert not decoded.is_karaoke
    assert not decoded.karaoke_reproduced
    assert len(decoded.channels) == 2
    assert decoded.output_acmod == ac3.Acmod.k2_0


# --- the lyrics reader -----------------------------------------------------------------------

# Every wheel has the reader but the Intel macOS one (bindings/python/CMakeLists.txt: its deployment
# target has no std::filesystem), where the submodule is absent and these have nothing to hold.
needs_lyrics = pytest.mark.skipif(
    not hasattr(ac3, "lyrics"), reason="this build of the extension has no lyrics submodule"
)


@needs_lyrics
def test_the_example_lyrics_read_as_a_word_timed_song() -> None:
    text = example("auld-lang-syne", "auld-lang-syne.lrc")
    lyrics = ac3.lyrics.parse(text, 16_500)
    assert lyrics.title == "Auld Lang Syne"
    assert lyrics.album == "iclforge karaoke example"
    assert lyrics.length_ms == 16_000
    assert lyrics.timed
    assert not lyrics.has_singers

    # Four sung lines and the time that only marks the end of the last.
    sung = [line for line in lyrics.lines if line.text]
    assert len(sung) == 4
    first = sung[0]
    assert first.start_ms == 0
    assert first.end_ms == 4_000
    assert first.text == "Should auld acquaintance be forgot,"
    assert first.singer == ac3.lyrics.Singer.kUnmarked
    assert first.word_timed
    # Syllables are words: Should auld ac quain tance be for got,
    assert len(first.words) == 8
    assert [w.start_ms for w in first.words[:3]] == [0, 500, 1_250]
    assert "".join(w.text for w in first.words) == first.text

    # The cursor: 700 ms is 200 ms into "auld" (500 to 1250).
    at = lyrics.position(700)
    assert at.line == 0
    assert at.in_line
    assert at.word == 1
    assert at.fraction == pytest.approx(200 / 750)
    assert lyrics.word_state(0, 0, 700) == ac3.lyrics.WordState.kSung
    assert lyrics.word_state(0, 1, 700) == ac3.lyrics.WordState.kBeingSung
    assert lyrics.word_state(0, 2, 700) == ac3.lyrics.WordState.kNotYetSung
    assert lyrics.word_state(0, 99, 700) == ac3.lyrics.WordState.kNotYetSung
    # The singer's nudge: shown 300 ms later, so the position is 300 ms earlier.
    assert lyrics.position(700, 300).position_ms == 400
    # Before the first line, and after the last.
    assert lyrics.position(-5).line is None
    assert not lyrics.position(60_000).in_line


@needs_lyrics
def test_a_duet_says_who_sings_each_line() -> None:
    lyrics = ac3.lyrics.parse(example("leave-her-johnny", "leave-her-johnny.lrc"))
    assert lyrics.first_singer_name == "Shantyman"
    assert lyrics.second_singer_name == "Crew"
    assert lyrics.has_singers
    singers = [line.singer for line in lyrics.lines if line.text]
    assert singers == [
        ac3.lyrics.Singer.kFirst,
        ac3.lyrics.Singer.kSecond,
        ac3.lyrics.Singer.kFirst,
        ac3.lyrics.Singer.kSecond,
    ]
    assert lyrics.lines[0].text.startswith("I thought")  # the marker is not text
    assert ac3.lyrics.singer_marker(ac3.lyrics.Singer.kBoth) == "V1+V2:"


@needs_lyrics
def test_read_and_find_file_work_beside_a_media_item() -> None:
    directory = EXAMPLES / "auld-lang-syne"
    found = ac3.lyrics.find_file(str(directory / "auld-lang-syne.ac3"))
    assert found is not None
    assert Path(found).name == "auld-lang-syne.lrc"
    assert ac3.lyrics.read(found).title == "Auld Lang Syne"
    assert ac3.lyrics.find_file(str(directory / "no-such-song.ac3")) is None


@needs_lyrics
def test_str_content_is_read_as_text() -> None:
    lyrics = ac3.lyrics.parse("[00:01.00]Hello\n[00:03.00]World\n")
    assert [line.text for line in lyrics.lines] == ["Hello", "World"]
    assert lyrics.lines[0].end_ms == 3_000


@needs_lyrics
def test_utf16_lyrics_come_back_as_utf8() -> None:
    # "shalom": what an editor saves as "Unicode", and the reason UTF-16 is read at all.
    shalom = "שלום"
    data = b"\xff\xfe" + f"[ti:{shalom}]\n[00:01.00]{shalom}\n".encode("utf-16-le")
    lyrics = ac3.lyrics.parse(data)
    assert lyrics.title == shalom
    assert lyrics.lines[0].text == shalom


@needs_lyrics
def test_bytes_that_are_not_lyrics_are_refused_with_the_reason() -> None:
    with pytest.raises(ValueError, match=r"."):
        ac3.lyrics.parse(b"")
    # Latin-1 is named and not guessed at.
    with pytest.raises(ValueError, match=r"(?i)utf"):
        ac3.lyrics.parse(b"[00:01.00]caf\xe9\n")
