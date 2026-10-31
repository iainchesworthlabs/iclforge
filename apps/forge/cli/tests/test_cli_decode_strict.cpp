#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac4/core/toc.hpp"
#include "iclforge/ac4/io/elementary.hpp"
#include "platform/process.hpp"

// `forge decode strict` (planning/ac4.md, option D): a decode that lost frames or units after the
// first one decoded exits 2 once it has written what it could decode, where without `strict` it
// exits 0 and warns as it always has. One case for each loss the decoders count: an AC-4 frame
// waiting for an I-frame, a concealed AC-3, E-AC-3 or AC-4 frame, and a TrueHD unit a lenient
// decode dropped. The leading frames of a stream cut in the middle of a group are not a loss, and
// each case holds that too.
//
// ICLFORGE_CLI_EXE, ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR and ICLFORGE_TRUEHD_FIXTURE_DIR come from
// the test CMakeLists.

namespace fs = std::filesystem;

namespace {

constexpr int kExitUsage = 1;
constexpr int kExitInput = 2;

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_decode_strict_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

void write_bytes(const fs::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

std::string read_text(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// The sync frames of a raw AC-4 stream as (offset, length): the sync word, a 16-bit frame size (or
// 0xFFFF and 24 bits), the frame, and a CRC-16 for 0xAC41 (ETSI TS 103 190-2 Annex G).
std::vector<std::pair<std::size_t, std::size_t>> ac4_frames(const std::vector<std::byte>& data) {
    std::vector<std::pair<std::size_t, std::size_t>> frames;
    std::size_t offset = 0;
    while (offset + 4 <= data.size()) {
        const auto word = (std::to_integer<unsigned>(data[offset]) << 8U) |
                          std::to_integer<unsigned>(data[offset + 1]);
        REQUIRE((word == 0xAC40U || word == 0xAC41U));
        std::size_t size = (std::to_integer<std::size_t>(data[offset + 2]) << 8U) |
                           std::to_integer<std::size_t>(data[offset + 3]);
        std::size_t header = 4;
        if (size == 0xFFFFU) {
            size = (std::to_integer<std::size_t>(data[offset + 4]) << 16U) |
                   (std::to_integer<std::size_t>(data[offset + 5]) << 8U) |
                   std::to_integer<std::size_t>(data[offset + 6]);
            header = 7;
        }
        const std::size_t length = header + size + (word == 0xAC41U ? 2 : 0);
        REQUIRE(offset + length <= data.size());
        frames.emplace_back(offset, length);
        offset += length;
    }
    return frames;
}

// The stream with the frames `drop` names left out.
std::vector<std::byte> ac4_without(const std::vector<std::byte>& data,
                                   const std::vector<std::size_t>& drop) {
    const auto frames = ac4_frames(data);
    std::vector<std::byte> out;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (std::ranges::find(drop, i) == drop.end()) {
            out.insert(
                out.end(), data.begin() + static_cast<std::ptrdiff_t>(frames[i].first),
                data.begin() + static_cast<std::ptrdiff_t>(frames[i].first + frames[i].second));
        }
    }
    return out;
}

fs::path ac4_tones() {
    return fs::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-20-tones-192" / "dee.ac4";
}

}  // namespace

TEST_CASE("decode strict exits 2 when an AC-4 frame waits for an I-frame after the first decoded",
          "[forge-cli][decode-strict][ac4]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "strict_ac4.log";
    const std::vector<std::byte> whole = read_bytes(ac4_tones());
    REQUIRE(ac4_frames(whole).size() == 120);

    SECTION("a frame missing from the middle: the frames after it wait, and strict says so") {
        const fs::path cut = dir / "strict_ac4_gap.ac4";
        write_bytes(cut, ac4_without(whole, {5}));
        const fs::path plain_wav = dir / "strict_ac4_gap_plain.wav";
        const fs::path strict_wav = dir / "strict_ac4_gap_strict.wav";

        // Without strict: exit 0 and the warning, exactly as before.
        REQUIRE(run_cli("decode " + quoted(cut) + " " + quoted(plain_wav), log) == 0);
        const std::string warning = read_text(log);
        CHECK(contains(warning, "warning: "));
        CHECK(contains(warning,
                       " frames waiting for an I-frame produced no output, of 119 in the "
                       "stream; the output holds the other "));
        CHECK_FALSE(contains(warning, "error: strict:"));

        // With it: the same output is written, and the exit status is 2.
        CHECK(run_cli("decode " + quoted(cut) + " " + quoted(strict_wav) + " strict", log) ==
              kExitInput);
        const std::string error = read_text(log);
        CHECK(contains(error, " frames waiting for an I-frame produced no output"));  // still said
        CHECK(contains(error, "error: strict: "));
        CHECK(contains(error, "lost after the first one decoded"));
        CHECK(contains(error, "waited for a sync point"));
        CHECK(contains(error, strict_wav.string()));
        REQUIRE(fs::exists(strict_wav));
        CHECK(read_bytes(strict_wav) == read_bytes(plain_wav));
    }
    SECTION("a stream cut before its first I-frame: the leading frames wait by design") {
        const fs::path film =
            fs::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-ims-film-96-24" / "dee.ac4";
        const fs::path cut = dir / "strict_ac4_lead.ac4";
        write_bytes(cut, ac4_without(read_bytes(film), {0}));
        const fs::path wav = dir / "strict_ac4_lead.wav";
        REQUIRE(run_cli("decode " + quoted(cut) + " " + quoted(wav), log) == 0);
        CHECK(contains(read_text(log), " frames waiting for an I-frame produced no output"));
        REQUIRE(run_cli("decode " + quoted(cut) + " " + quoted(wav) + " strict", log) == 0);
        CHECK(contains(read_text(log), " frames waiting for an I-frame produced no output"));
        CHECK_FALSE(contains(read_text(log), "error: strict:"));
    }
    SECTION("a stream that loses nothing is not changed by it") {
        const fs::path wav = dir / "strict_ac4_whole.wav";
        const fs::path strict_wav = dir / "strict_ac4_whole_strict.wav";
        REQUIRE(run_cli("decode " + quoted(ac4_tones()) + " " + quoted(wav), log) == 0);
        REQUIRE(run_cli("decode " + quoted(ac4_tones()) + " " + quoted(strict_wav) + " strict",
                        log) == 0);
        CHECK(read_bytes(wav) == read_bytes(strict_wav));
    }
    SECTION("a concealed frame is a loss too") {
        // The eleventh frame's audio_size_value set past its audio substream, as
        // test_cli_containers.cpp does: the table of contents reads and the substream does not.
        std::vector<std::byte> bytes = whole;
        const iclforge::ac4::ScanResult scan = iclforge::ac4::scan(bytes);
        REQUIRE(scan.frames.size() == 120);
        const std::span<const std::byte> raw = scan.frames[10].raw_ac4_frame;
        const auto parsed = iclforge::ac4::parse_raw_frame(raw);
        REQUIRE(parsed.has_value());
        const auto audio = std::ranges::find_if(
            parsed->substreams, [](const iclforge::ac4::Substream& s) { return s.is_audio; });
        REQUIRE(audio != parsed->substreams.end());
        const auto at = static_cast<std::size_t>(raw.data() - bytes.data()) + audio->offset;
        bytes[at] = std::byte{0xFF};
        bytes[at + 1] = std::byte{0xFE};
        const fs::path damaged = dir / "strict_ac4_damaged.ac4";
        write_bytes(damaged, bytes);
        const fs::path wav = dir / "strict_ac4_damaged.wav";
        // Without conceal= a damaged frame stops the decode, as it always has.
        CHECK(run_cli("decode " + quoted(damaged) + " " + quoted(wav), log) == kExitInput);
        CHECK_FALSE(contains(read_text(log), "error: strict:"));
        // Concealed, the decode goes on and exits 0 ...
        REQUIRE(run_cli("decode " + quoted(damaged) + " " + quoted(wav) + " conceal=repeat", log) ==
                0);
        CHECK(contains(read_text(log), "of them concealed"));
        // ... unless it is strict: what was concealed was lost.
        CHECK(run_cli("decode " + quoted(damaged) + " " + quoted(wav) + " conceal=repeat strict",
                      log) == kExitInput);
        const std::string error = read_text(log);
        CHECK(contains(error,
                       "error: strict: 1 frame(s) or unit(s) lost after the first one "
                       "decoded (1 concealed)"));
        CHECK(fs::exists(wav));
    }
}

TEST_CASE("decode strict exits 2 when an AC-3 or E-AC-3 frame was concealed",
          "[forge-cli][decode-strict][ac3]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "strict_ac3.log";

    SECTION("AC-3") {
        const fs::path clean = dir / "strict_clean.ac3";
        REQUIRE(run_cli("sine " + quoted(clean) + " 2 192 440 70 stereo", log) == 0);
        // One payload byte flipped in the middle of the fourth frame, sync word and frame size left
        // alone; 192 kbps at 48 kHz is a 768-byte syncframe.
        constexpr std::size_t kFrameBytes = 768;
        std::vector<std::byte> bytes = read_bytes(clean);
        REQUIRE(bytes.size() > 4 * kFrameBytes);
        bytes[(3 * kFrameBytes) + (kFrameBytes / 2)] ^= std::byte{0xFF};
        const fs::path damaged = dir / "strict_damaged.ac3";
        write_bytes(damaged, bytes);
        const fs::path wav = dir / "strict_ac3.wav";
        REQUIRE(run_cli("decode " + quoted(damaged) + " " + quoted(wav) + " conceal=repeat", log) ==
                0);
        CHECK(contains(read_text(log), "concealed 1 of"));
        const std::vector<std::byte> plain = read_bytes(wav);
        fs::remove(wav);
        CHECK(run_cli("decode " + quoted(damaged) + " " + quoted(wav) + " conceal=repeat strict",
                      log) == kExitInput);
        CHECK(contains(read_text(log),
                       "error: strict: 1 frame(s) or unit(s) lost after the first "
                       "one decoded (1 concealed)"));
        REQUIRE(fs::exists(wav));
        CHECK(read_bytes(wav) == plain);
        // The undamaged stream is not touched by it.
        REQUIRE(run_cli("decode " + quoted(clean) + " " + quoted(wav) + " strict", log) == 0);
    }
    SECTION("E-AC-3") {
        const fs::path source = fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / ".." / "audio" /
                                "reference_51_eac3_448k_cplbndstrce0.ec3";
        REQUIRE(fs::exists(source));
        std::vector<std::byte> bytes = read_bytes(source);
        const auto scanned = iclforge::ac3::io::scan(bytes);
        REQUIRE(scanned.has_value());
        REQUIRE(scanned->access_units.size() > 10);
        const auto unit = scanned->access_units[5];
        const auto at = static_cast<std::size_t>(unit.data() - bytes.data()) + (unit.size() / 2);
        bytes[at] ^= std::byte{0xFF};
        const fs::path damaged = dir / "strict_damaged.ec3";
        write_bytes(damaged, bytes);
        const fs::path wav = dir / "strict_eac3.wav";
        REQUIRE(run_cli("decode " + quoted(damaged) + " " + quoted(wav) + " conceal=repeat", log) ==
                0);
        CHECK(contains(read_text(log), "concealed "));
        CHECK(run_cli("decode " + quoted(damaged) + " " + quoted(wav) + " conceal=repeat strict",
                      log) == kExitInput);
        CHECK(contains(read_text(log), "error: strict: "));
        CHECK(contains(read_text(log), "concealed)"));
        REQUIRE(run_cli("decode " + quoted(source) + " " + quoted(wav) + " strict", log) == 0);
    }
}

TEST_CASE("decode strict exits 2 when a lenient TrueHD decode dropped units after the first",
          "[forge-cli][decode-strict][truehd]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "strict_truehd.log";
    const fs::path fixture = fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / "truehd_st48_s24_gap8.thd";
    REQUIRE(fs::exists(fixture));
    std::vector<std::byte> bytes = read_bytes(fixture);
    // The offsets of the units, from their length words.
    std::vector<std::size_t> starts;
    for (std::size_t offset = 0; offset + 2 <= bytes.size();) {
        starts.push_back(offset);
        offset += (((std::to_integer<std::size_t>(bytes[offset]) & 0xFU) << 8U) |
                   std::to_integer<std::size_t>(bytes[offset + 1])) *
                  2;
    }
    REQUIRE(starts.size() > 40);

    SECTION("bytes in the middle that are not a unit: the reader skips on to the next major sync") {
        std::vector<std::byte> junk = bytes;
        junk.insert(junk.begin() + static_cast<std::ptrdiff_t>(starts[11]), 9, std::byte{0x5A});
        const fs::path stream = dir / "strict_junk.thd";
        write_bytes(stream, junk);
        const fs::path plain = dir / "strict_junk_plain.wav";
        const fs::path strict = dir / "strict_junk_strict.wav";
        // Strict checks stop at the damage whatever else is asked (unchanged).
        CHECK(run_cli("decode " + quoted(stream) + " " + quoted(plain), log) == kExitInput);
        CHECK_FALSE(contains(read_text(log), "error: strict:"));
        // Lenient: exit 0, and the skipped bytes are reported where they always were.
        REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(plain) + " truehd-check=lenient",
                        log) == 0);
        CHECK(contains(read_text(log), "bytes before access unit"));
        // Lenient and strict: the same output, and exit 2.
        CHECK(run_cli("decode " + quoted(stream) + " " + quoted(strict) +
                          " truehd-check=lenient strict",
                      log) == kExitInput);
        const std::string error = read_text(log);
        CHECK(contains(error, "error: strict: "));
        CHECK(contains(error, "lost after the first one decoded"));
        CHECK(contains(error, "unreadable or skipped"));
        REQUIRE(fs::exists(strict));
        CHECK(read_bytes(strict) == read_bytes(plain));
    }
    SECTION("an undamaged stream is not changed by it") {
        const fs::path plain = dir / "strict_whole_plain.wav";
        const fs::path strict = dir / "strict_whole_strict.wav";
        REQUIRE(run_cli("decode " + quoted(fixture) + " " + quoted(plain), log) == 0);
        REQUIRE(run_cli("decode " + quoted(fixture) + " " + quoted(strict) + " strict", log) == 0);
        CHECK(read_bytes(plain) == read_bytes(strict));
    }
}

TEST_CASE("strict is decode's alone", "[forge-cli][decode-strict]") {
    const fs::path dir = scratch_dir();
    const fs::path log = dir / "strict_other.log";
    const fs::path fixture = fs::path{ICLFORGE_TRUEHD_FIXTURE_DIR} / "truehd_st48_s24.thd";
    // Another command given it refuses it as an option it does not know, and does not drop it.
    CHECK(run_cli("probe " + quoted(fixture) + " strict", log) == kExitUsage);
    CHECK(run_cli("qc " + quoted(fixture) + " strict", log) == kExitUsage);
    CHECK(run_cli("levels " + quoted(fixture) + " strict", log) == kExitUsage);
    CHECK(run_cli("truehd-encode " + quoted(fixture) + " " + quoted(dir / "x.thd") + " strict",
                  log) == kExitUsage);
}
