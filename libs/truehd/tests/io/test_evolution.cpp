#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/objects/emdf.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/evolution.hpp"

// The evolution frame of a unit's extra data (TH spec v3.6 §15.4, §16.1), read by
// libs/truehd/src/io/evolution.cpp. PROVISIONAL, owner to confirm (libs/truehd/ERRATA.md question
// 20): no stream carries one, so the checks here are
//
//   - the vectors tools/references/truehd_evolution.py writes (testdata/truehd/evolution):
//   hand-built
//     extra data regions that exercise every branch of the syntax, with what an independent Python
//     transcription reads from each under both readings (EMDF and the letter of §15.4);
//   - frames assembled here from a bit string, so that a few values are checked against numbers
//     written by hand and not against the reference;
//   - the same container bytes read by iclforge::objects::emdf::parse_container, the reader E-AC-3
//     streams go through, which must find the same payload ids, sizes and bytes.
//
// Tagged [no-external-oracle]: no decoder outside this repository reads these streams.

namespace {

using namespace iclforge::truehd;
using nlohmann::json;
namespace emdf = iclforge::objects::emdf;
namespace oba = iclforge::objects::oba;

std::vector<std::byte> from_hex(const std::string& text) {
    std::vector<std::byte> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::byte>(std::stoul(text.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::string to_hex(std::span<const std::byte> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    for (const std::byte b : bytes) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4U]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 15U]);
    }
    return out;
}

// "0101 1 00" -> bits, spaces ignored, zero padded to a byte.
std::vector<std::byte> from_bits(std::string_view text) {
    std::vector<std::byte> out;
    unsigned count = 0;
    unsigned current = 0;
    for (const char c : text) {
        if (c != '0' && c != '1') {
            continue;
        }
        current = (current << 1U) | static_cast<unsigned>(c - '0');
        if (++count % 8 == 0) {
            out.push_back(static_cast<std::byte>(current));
            current = 0;
        }
    }
    if (count % 8 != 0) {
        out.push_back(static_cast<std::byte>(current << (8U - count % 8U)));
    }
    return out;
}

json load_vectors() {
    std::ifstream in(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/evolution/vectors.json",
                     std::ios::binary);
    REQUIRE(in.good());
    return json::parse(in);
}

std::optional<std::uint64_t> optional_of(const json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return value.get<std::uint64_t>();
}

EvolutionPayloadConfig config_of(const json& want) {
    EvolutionPayloadConfig config;
    config.sample_offset = optional_of(want["sample_offset"]);
    config.duration = optional_of(want["duration"]);
    config.group_id = optional_of(want["group_id"]);
    if (!want["codec_data"].is_null()) {
        config.codec_data = want["codec_data"].get<std::uint8_t>();
    }
    config.discard_unknown = want["discard_unknown"].get<bool>();
    config.frame_aligned = want["frame_aligned"].get<bool>();
    config.create_duplicate = want["create_duplicate"].get<bool>();
    config.remove_duplicate = want["remove_duplicate"].get<bool>();
    config.priority = want["priority"].get<std::uint8_t>();
    config.proc_allowed = want["proc_allowed"].get<std::uint8_t>();
    return config;
}

// What the Python reference reads from `want`, against what the library reads.
void check_against_reference(const EvolutionRead& got, const json& want) {
    CHECK(got.check_ok == want["check_ok"].get<bool>());
    CHECK(got.framing_ok == want["framing_ok"].get<bool>());
    CHECK(got.parity_ok == want["parity_ok"].get<bool>());
    CHECK(got.reserved_zero == want["reserved_zero"].get<bool>());
    CHECK(got.length_fits == want["length_fits"].get<bool>());
    CHECK(got.extra_data_length == want["length"].get<unsigned>());
    CHECK(got.evo_frame_byte_length == want["frame_bytes"].get<unsigned>());
    const json& container = want["container"];
    if (!container["ok"].get<bool>()) {
        REQUIRE_FALSE(got.container.has_value());
        CHECK(to_string(got.container.error().kind) == container["kind"].get<std::string>());
        CHECK(got.container.error().bit_offset == container["bit_offset"].get<std::size_t>());
        return;
    }
    REQUIRE(got.container.has_value());
    const EvolutionFrame& frame = *got.container;
    CHECK(frame.version == container["version"].get<std::uint64_t>());
    CHECK(frame.key_id == container["key_id"].get<std::uint64_t>());
    CHECK(frame.bits_used == container["bits_used"].get<std::size_t>());
    CHECK(frame.trailing_zero == container["trailing_zero"].get<bool>());
    const json& payloads = container["payloads"];
    REQUIRE(frame.payloads.size() == payloads.size());
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        CHECK(frame.payloads[i].id == payloads[i]["id"].get<std::uint64_t>());
        CHECK(frame.payloads[i].config == config_of(payloads[i]["config"]));
        CHECK(to_hex(frame.payloads[i].bytes) == payloads[i]["bytes"].get<std::string>());
    }
    const json& protection = container["protection"];
    CHECK(frame.protection.primary_code == protection["primary_code"].get<unsigned>());
    CHECK(frame.protection.secondary_code == protection["secondary_code"].get<unsigned>());
    CHECK(to_hex(frame.protection.primary) == protection["primary"].get<std::string>());
    CHECK(to_hex(frame.protection.secondary) == protection["secondary"].get<std::string>());
}

// The E-AC-3 container around a body: sync word, 16-bit length in bytes.
std::vector<std::byte> wrap_as_emdf(std::span<const std::byte> body) {
    std::vector<std::byte> out{std::byte{0x58}, std::byte{0x38},
                               static_cast<std::byte>((body.size() >> 8U) & 0xFFU),
                               static_cast<std::byte>(body.size() & 0xFFU)};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

struct Counts {
    int both_ok = 0;
    int both_failed = 0;
    int version_difference = 0;
};

// The rule the two readers are held to: when this one reads a frame the E-AC-3 reader reads the
// same payload ids, sizes and bytes, except where Annex H defines nothing (a version other than
// 0, which the E-AC-3 reader refuses); when this one refuses, so does the E-AC-3 reader.
void compare_with_emdf_reader(std::span<const std::byte> body, Counts& counts) {
    const auto mine = parse_evolution_frame(body, EvolutionReading::kEmdf);
    const auto theirs = emdf::parse_container(wrap_as_emdf(body));
    const bool theirs_ok = theirs.has_value() && theirs->has_value();
    if (!mine.has_value()) {
        CHECK_FALSE(theirs_ok);
        ++counts.both_failed;
        return;
    }
    if (!theirs_ok) {
        // The one frame this reader accepts and the E-AC-3 reader refuses.
        REQUIRE_FALSE(theirs.has_value());
        CHECK(mine->version != 0);
        CHECK(theirs.error() == emdf::ParseError::kUnsupportedConfig);
        ++counts.version_difference;
        return;
    }
    const auto& payloads = **theirs;
    REQUIRE(mine->payloads.size() == payloads.size());
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        CHECK(mine->payloads[i].id == static_cast<std::uint64_t>(payloads[i].id));
        CHECK(mine->payloads[i].bytes == payloads[i].bytes);
        const auto& want = payloads[i].config;
        const auto& got = mine->payloads[i].config;
        CHECK(got.sample_offset.value_or(~std::uint64_t{0}) ==
              (want.sample_offset < 0 ? ~std::uint64_t{0}
                                      : static_cast<std::uint64_t>(want.sample_offset)));
        CHECK(got.duration.has_value() == (want.duration >= 0));
        CHECK(got.group_id.has_value() == (want.group_id >= 0));
        CHECK(got.codec_data.has_value() == want.codec_data);
        CHECK(got.discard_unknown == want.discard_unknown);
        CHECK(got.frame_aligned == want.frame_aligned);
        CHECK(got.create_duplicate == want.create_duplicate);
        CHECK(got.remove_duplicate == want.remove_duplicate);
        CHECK(got.priority == want.priority);
        CHECK(got.proc_allowed == want.proc_allowed);
    }
    ++counts.both_ok;
}

}  // namespace

TEST_CASE("the reference's vectors read the same in both readings",
          "[truehd][io][evolution][no-external-oracle]") {
    const json vectors = load_vectors();
    REQUIRE(vectors["vectors"].size() >= 120);
    std::set<std::string> kinds_seen;
    std::set<std::string> kinds_wanted;
    for (EvolutionErrorKind kind :
         {EvolutionErrorKind::kNoEvolutionPayload, EvolutionErrorKind::kEmpty,
          EvolutionErrorKind::kTruncatedHeader, EvolutionErrorKind::kMissingTerminator,
          EvolutionErrorKind::kTruncatedPayloadId, EvolutionErrorKind::kTruncatedConfig,
          EvolutionErrorKind::kTruncatedSize, EvolutionErrorKind::kTruncatedPayload,
          EvolutionErrorKind::kReservedProtectionLength,
          EvolutionErrorKind::kTruncatedProtection}) {
        kinds_wanted.insert(std::string(to_string(kind)));
    }
    std::size_t successes = 0;
    for (const json& vector : vectors["vectors"]) {
        const std::string name = vector["name"].get<std::string>();
        const std::vector<std::byte> region = from_hex(vector["extra_data"].get<std::string>());
        const auto extra = read_extra_data(region);
        for (const EvolutionReading reading :
             {EvolutionReading::kEmdf, EvolutionReading::kSpec36}) {
            INFO(name << " read as " << to_string(reading));
            const json& want = vector["expect"][std::string(to_string(reading))];
            if (want.is_null()) {
                CHECK_FALSE(extra.has_value());
                continue;
            }
            REQUIRE(extra.has_value());
            const EvolutionRead got = read_evolution(*extra, reading);
            check_against_reference(got, want);
            if (got.container.has_value()) {
                ++successes;
            } else {
                kinds_seen.insert(std::string(to_string(got.container.error().kind)));
            }
        }
    }
    CHECK(successes >= 135);
    // Every way a frame can fail is in the vectors.
    CHECK(kinds_seen == kinds_wanted);
}

TEST_CASE("a frame written out bit by bit reads as its fields",
          "[truehd][io][evolution][no-external-oracle]") {
    // version 0, key 0, payload id 11 with the configuration TS 103 420 Table 56 asks for of an
    // OAMD payload (group id 2, frame aligned, nothing else), three bytes, the end of the list, a 4
    // byte primary and a 1 byte secondary protection field.
    const auto frame = from_bits(
        "00 000 01011"
        "0 0 1 10 0  0 0 1 0 0 00000 00"
        "00000011 0  10101010 10111011 11001100"
        "00000"
        "10 01 11110000 11110001 11110010 11110011 01010101");
    SECTION("as the EMDF container") {
        const auto parsed = parse_evolution_frame(frame, EvolutionReading::kEmdf);
        REQUIRE(parsed.has_value());
        CHECK(parsed->version == 0);
        CHECK(parsed->key_id == 0);
        REQUIRE(parsed->payloads.size() == 1);
        const EvolutionPayload& payload = parsed->payloads[0];
        CHECK(payload.id == kEvolutionPayloadOamd);
        CHECK(payload.config.group_id == 2);
        CHECK_FALSE(payload.config.sample_offset.has_value());
        CHECK_FALSE(payload.config.duration.has_value());
        CHECK_FALSE(payload.config.codec_data.has_value());
        CHECK(payload.config.frame_aligned);
        CHECK_FALSE(payload.config.create_duplicate);
        CHECK_FALSE(payload.config.remove_duplicate);
        CHECK_FALSE(payload.config.discard_unknown);
        CHECK(payload.config.priority == 0);
        CHECK(payload.config.proc_allowed == 0);
        CHECK(to_hex(payload.bytes) == "aabbcc");
        CHECK(to_hex(parsed->oamd()) == "aabbcc");
        CHECK(parsed->protection.primary_code == 2);
        CHECK(parsed->protection.secondary_code == 1);
        CHECK(to_hex(parsed->protection.primary) == "f0f1f2f3");
        CHECK(to_hex(parsed->protection.secondary) == "55");
        CHECK(parsed->bits_used == 110);
        CHECK(parsed->trailing_zero);
        // And the E-AC-3 reader finds the same payload in the same bytes.
        Counts counts;
        compare_with_emdf_reader(frame, counts);
        CHECK(counts.both_ok == 1);
    }
    SECTION("a frame of the letter of §15.4 has no flag bits in its variable-length numbers") {
        // The same fields in the v3.6 coding: group id 2 and the size 3 are plain groups, and a
        // primary protection length code of 0 is allowed (it means no bytes).
        const auto letter = from_bits(
            "00 000 01011"
            "0 0 1 10  0 0 1 0 0 00000 00"
            "00000011  10101010 10111011 11001100"
            "00000"
            "00 10 11110000 11110001 11110010 11110011");
        const auto parsed = parse_evolution_frame(letter, EvolutionReading::kSpec36);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->payloads.size() == 1);
        CHECK(parsed->payloads[0].id == 11);
        CHECK(parsed->payloads[0].config.group_id == 2);
        CHECK(to_hex(parsed->payloads[0].bytes) == "aabbcc");
        CHECK(parsed->protection.primary_code == 0);
        CHECK(parsed->protection.primary.empty());
        CHECK(to_hex(parsed->protection.secondary) == "f0f1f2f3");
        CHECK(parsed->bits_used == 100);
        // Under the EMDF reading the same bytes are not the same frame: the first variable_bits
        // takes the next bit as a flag and every field after it moves.
        const auto as_emdf = parse_evolution_frame(letter, EvolutionReading::kEmdf);
        CHECK((!as_emdf.has_value() || as_emdf->payloads != parsed->payloads));
    }
}

TEST_CASE("the two readings differ where §15.4 and the EMDF container do",
          "[truehd][io][evolution]") {
    // Payload id 31: the EMDF container escapes it to a longer id, the letter of §15.4 reads 31.
    const auto id31 = from_bits(
        "00 000 11111  00000 0  0 0 0 0 0 1 0 0 00000 00  00000001 0  11111111  00000  01 00 "
        "10100000");
    const auto emdf_reading = parse_evolution_frame(id31, EvolutionReading::kEmdf);
    REQUIRE(emdf_reading.has_value());
    REQUIRE(emdf_reading->payloads.size() == 1);
    CHECK(emdf_reading->payloads[0].id == 31);  // 31 + variable_bits(5) = 31 + 0
    // smploffst: 11 bits and a reserved bit against a variable_bits(11, 2). 100 samples.
    const auto offset_emdf = from_bits(
        "00 000 01011  1 00001100100 0  0 0 0 0 00000 00  00000000 0  00000  01 00 11111111");
    const auto read_emdf = parse_evolution_frame(offset_emdf, EvolutionReading::kEmdf);
    REQUIRE(read_emdf.has_value());
    CHECK(read_emdf->payloads[0].config.sample_offset == 100);
    CHECK(read_emdf->payloads[0].config.priority == 0);
    const auto offset_letter =
        from_bits("00 000 01011  1 00001100100  0 0 0 0 00000 00  00000000  00000  01 00 11111111");
    const auto read_letter = parse_evolution_frame(offset_letter, EvolutionReading::kSpec36);
    REQUIRE(read_letter.has_value());
    CHECK(read_letter->payloads[0].config.sample_offset == 100);
    // A variable_bits that is all ones continues: 255 is 255 and a further group in the escape
    // coding, 255 plus the next group; the EMDF coding has its flag bit after every group.
    const auto size255 = from_bits("00 000 01011  0 0 0 0 0 1 0 0 00000 00  11111111 00000000");
    const auto escape = parse_evolution_frame(size255, EvolutionReading::kSpec36);
    REQUIRE_FALSE(escape.has_value());  // 255 bytes are claimed and the frame ends
    CHECK(escape.error().kind == EvolutionErrorKind::kTruncatedPayload);
}

TEST_CASE("the payload with id 11 is handed out and the others are found by id",
          "[truehd][io][evolution]") {
    EvolutionFrame frame;
    CHECK(frame.oamd().empty());
    CHECK(frame.find(11) == nullptr);
    frame.payloads.push_back({.id = 14, .config = {}, .bytes = {std::byte{1}}});
    frame.payloads.push_back({.id = 11, .config = {}, .bytes = {std::byte{2}, std::byte{3}}});
    frame.payloads.push_back({.id = 11, .config = {}, .bytes = {std::byte{4}}});
    REQUIRE(frame.find(14) != nullptr);
    CHECK(frame.find(14)->bytes.size() == 1);
    REQUIRE(frame.oamd().size() == 2);  // the first of the payloads with id 11
    CHECK(frame.oamd()[0] == std::byte{2});
    CHECK(frame.find(12) == nullptr);
}

TEST_CASE("extra data read from its own bytes is what a whole unit reads",
          "[truehd][io][evolution]") {
    CHECK_FALSE(read_extra_data({}).has_value());
    const std::vector<std::byte> one{std::byte{0x20}};
    CHECK_FALSE(read_extra_data(one).has_value());
    const json vectors = load_vectors();
    const auto region = from_hex(vectors["vectors"][0]["extra_data"].get<std::string>());
    const auto extra = read_extra_data(region);
    REQUIRE(extra.has_value());
    CHECK(extra->bytes.size() == region.size());
    CHECK(extra->check_ok);
    CHECK(extra->framing_ok);
    CHECK(extra->parity_ok);
}

TEST_CASE("the E-AC-3 container's body is the evolution frame", "[truehd][io][evolution]") {
    // iclforge::objects writes the E-AC-3 container; with its sync word and length taken off the
    // rest is §15.4's evo_frame() read as the EMDF container.
    const std::array<std::byte, 5> small{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                         std::byte{5}};
    std::vector<std::byte> large(300);
    for (std::size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<std::byte>((i * 13 + 1) & 0xFF);
    }
    const std::vector<emdf::Payload> payloads{
        {.id = emdf::kPayloadIdOamd, .bytes = small},
        {.id = emdf::kPayloadIdJoc, .bytes = large},
        {.id = 3, .bytes = {}},
    };
    for (const int groupid : {0, 1, 2, 3, 17, 300}) {
        INFO("groupid " << groupid);
        const std::vector<std::byte> container = emdf::build_container(payloads, groupid);
        const std::span<const std::byte> body = std::span{container}.subspan(4);
        const auto parsed = parse_evolution_frame(body, EvolutionReading::kEmdf);
        REQUIRE(parsed.has_value());
        CHECK(parsed->version == 0);
        CHECK(parsed->key_id == 0);
        REQUIRE(parsed->payloads.size() == 3);
        CHECK(parsed->payloads[0].id == 11);
        CHECK(parsed->payloads[1].id == 14);
        CHECK(parsed->payloads[2].id == 3);
        CHECK(parsed->payloads[1].bytes == large);
        CHECK(parsed->payloads[2].bytes.empty());
        for (const EvolutionPayload& payload : parsed->payloads) {
            CHECK(payload.config.group_id == static_cast<std::uint64_t>(groupid));
            CHECK(payload.config.frame_aligned);
            CHECK_FALSE(payload.config.sample_offset.has_value());
            CHECK_FALSE(payload.config.duration.has_value());
            CHECK_FALSE(payload.config.codec_data.has_value());
        }
        // TS 103 420 Table 56's protection reservation: 32 bits primary, 8 bits secondary.
        CHECK(parsed->protection.primary_code == 2);
        CHECK(parsed->protection.secondary_code == 1);
        CHECK(parsed->protection.primary.size() == 4);
        CHECK(parsed->protection.secondary.size() == 1);
        CHECK(parsed->trailing_zero);
        Counts counts;
        compare_with_emdf_reader(body, counts);
        CHECK(counts.both_ok == 1);
    }
}

TEST_CASE("every vector the reference calls an EMDF frame reads the same in the E-AC-3 reader",
          "[truehd][io][evolution][no-external-oracle]") {
    const json vectors = load_vectors();
    Counts counts;
    for (const json& vector : vectors["vectors"]) {
        INFO(vector["name"].get<std::string>());
        const std::vector<std::byte> region = from_hex(vector["extra_data"].get<std::string>());
        const auto extra = read_extra_data(region);
        REQUIRE(extra.has_value());
        if (!extra->framing_ok) {
            continue;
        }
        compare_with_emdf_reader(extra->evolution_payload, counts);
    }
    // Both ways are exercised: frames both read, frames neither reads, and the version difference.
    CHECK(counts.both_ok >= 60);
    CHECK(counts.both_failed >= 50);
    CHECK(counts.version_difference >= 6);
}

TEST_CASE("random bytes read the same in the E-AC-3 reader as in this one",
          "[truehd][io][evolution][no-external-oracle]") {
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    const auto next = [&state]() {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    };
    Counts counts;
    for (int round = 0; round < 40000; ++round) {
        const std::size_t length = 1 + next() % 40;
        std::vector<std::byte> body(length);
        for (std::byte& b : body) {
            b = static_cast<std::byte>(next() & 0xFFU);
        }
        if (next() % 4 != 0) {
            body[0] &=
                std::byte{0x3F};  // version 0 most of the time: it is where the readers agree
        }
        INFO("round " << round << " bytes " << to_hex(body));
        compare_with_emdf_reader(body, counts);
    }
    // A sweep that read nothing, or refused everything, would prove nothing.
    CHECK(counts.both_ok >= 400);
    CHECK(counts.both_failed >= 5000);
    CHECK(counts.version_difference >= 80);
}

namespace {

// A frame of random shape, written with the base library's bit writer in the EMDF coding: random
// version and key id (with their extensions), up to four payloads of random id (with the escape),
// configuration and size, a protection field of random lengths, and zero bits after it.
template <typename Next>
std::vector<std::byte> random_frame(Next&& next) {
    iclforge::BitWriter w;
    const std::uint32_t version = next() % 4 == 0 ? static_cast<std::uint32_t>(next() % 12) : 0U;
    w.put(std::min<std::uint32_t>(version, 3U), 2);
    if (version >= 3) {
        w.write_variable_bits(2, version - 3);
    }
    const auto key_id = static_cast<std::uint32_t>(next() % 12);
    w.put(std::min<std::uint32_t>(key_id, 7U), 3);
    if (key_id >= 7) {
        w.write_variable_bits(3, key_id - 7);
    }
    const auto count = next() % 5;
    for (std::uint64_t n = 0; n < count; ++n) {
        const auto id =
            static_cast<std::uint32_t>(next() % 3 == 0 ? 31 + next() % 70 : 1 + next() % 30);
        w.put(std::min<std::uint32_t>(id, 31U), 5);
        if (id >= 31) {
            w.write_variable_bits(5, id - 31);
        }
        const bool offset = next() % 4 == 0;
        w.put(offset ? 1U : 0U, 1);
        if (offset) {
            w.put(static_cast<std::uint32_t>(next() % 2048), 11);
            w.put(0, 1);
        }
        const bool duration = next() % 4 == 0;
        w.put(duration ? 1U : 0U, 1);
        if (duration) {
            w.write_variable_bits(11, next() % 5000);
        }
        const bool group = next() % 2 == 0;
        w.put(group ? 1U : 0U, 1);
        if (group) {
            w.write_variable_bits(2, next() % 40);
        }
        const bool codec = next() % 4 == 0;
        w.put(codec ? 1U : 0U, 1);
        if (codec) {
            w.put(static_cast<std::uint32_t>(next() % 256), 8);
        }
        const bool discard = next() % 5 == 0;
        w.put(discard ? 1U : 0U, 1);
        if (!discard) {
            bool aligned = false;
            if (!offset) {
                aligned = next() % 2 == 0;
                w.put(aligned ? 1U : 0U, 1);
                if (aligned) {
                    w.put(static_cast<std::uint32_t>(next() % 4), 2);
                }
            }
            if (offset || aligned) {
                w.put(static_cast<std::uint32_t>(next() % 32), 5);
                w.put(static_cast<std::uint32_t>(next() % 4), 2);
            }
        }
        const auto size = static_cast<std::uint32_t>(next() % 4 == 0 ? next() % 600 : next() % 12);
        w.write_variable_bits(8, size);
        for (std::uint32_t i = 0; i < size; ++i) {
            w.put(static_cast<std::uint32_t>(next() % 256), 8);
        }
    }
    w.put(0, 5);
    const auto primary = static_cast<std::uint32_t>(next() % 8 == 0 ? 0 : 1 + next() % 3);
    const auto secondary = static_cast<std::uint32_t>(next() % 4);
    w.put(primary, 2);
    w.put(secondary, 2);
    constexpr std::array<int, 4> kBytes = {0, 1, 4, 16};
    for (int i = 0; i < 8 * (kBytes[primary] + kBytes[secondary]); ++i) {
        w.put(static_cast<std::uint32_t>(next() % 2), 1);
    }
    for (std::uint64_t pad = next() % 12; pad > 0; --pad) {
        w.put(0, 1);
    }
    return w.take();
}

}  // namespace

TEST_CASE("frames of random shape, whole and damaged, read the same in the E-AC-3 reader",
          "[truehd][io][evolution][no-external-oracle]") {
    std::uint64_t state = 0xD1B54A32D192ED03ULL;
    const auto next = [&state]() {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    };
    Counts whole;
    Counts damaged;
    for (int round = 0; round < 6000; ++round) {
        std::vector<std::byte> frame = random_frame(next);
        INFO("round " << round << " bytes " << to_hex(frame));
        compare_with_emdf_reader(frame, whole);
        // One flipped bit, then the frame cut short by up to three bytes.
        std::vector<std::byte> flipped = frame;
        flipped[next() % flipped.size()] ^= static_cast<std::byte>(1U << (next() % 8));
        compare_with_emdf_reader(flipped, damaged);
        const std::size_t cut = std::min<std::size_t>(frame.size(), 1 + next() % 3);
        frame.resize(frame.size() - cut);
        if (!frame.empty()) {
            compare_with_emdf_reader(frame, damaged);
        }
    }
    // Most whole frames read in both (the others have a primary protection code of 0, or a version
    // other than 0); the damaged ones go both ways.
    CHECK(whole.both_ok >= 3000);
    CHECK(whole.version_difference >= 200);
    CHECK(damaged.both_ok >= 3000);
    CHECK(damaged.both_failed >= 1500);
}

TEST_CASE("object audio metadata goes into a unit's extra data and comes out of the decoder",
          "[truehd][io][evolution][encoder]") {
    // The chain the Atmos work will use, in this direction: an OAMD payload (iclforge::objects) in
    // the E-AC-3 container's body, framed as extra data by the encoder, carried by the decoder,
    // read back, and the payload with id 11 parsed by the OAMD reader.
    oba::Program program;
    program.dynamic_only = true;
    program.lfe = true;
    program.dynamic_objects = 3;
    std::vector<oba::DynamicObject> objects(3);
    objects[1].position.x = 0.25;
    objects[2].position.z = 0.5;
    const std::vector<std::byte> oamd = oba::build_payload(program, objects);
    const std::array<emdf::Payload, 1> payloads{{{.id = emdf::kPayloadIdOamd, .bytes = oamd}}};
    const std::vector<std::byte> container = emdf::build_container(payloads, 0);
    const std::vector<std::byte> body(container.begin() + 4, container.end());

    EncoderConfig config;
    config.restart_interval_units = 8;
    config.extra_data = [&body](std::uint64_t index) {
        return index % 2 == 0 ? body : std::vector<std::byte>{};
    };
    const auto encoded = testing::encode_all(config, testing::make_pcm(2, 40 * 12, 24, 3));
    Decoder decoder;
    int frames = 0;
    for (std::size_t i = 0; i < encoded.units.size(); ++i) {
        auto decoded = decoder.decode(encoded.units[i].bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const auto& unit = **decoded;
        if (i % 2 != 0) {
            CHECK(unit.extra_data.empty());
            continue;
        }
        const auto extra = read_extra_data(unit.extra_data);
        REQUIRE(extra.has_value());
        const EvolutionRead read = read_evolution(*extra);
        CHECK(read.check_ok);
        CHECK(read.framing_ok);
        CHECK(read.parity_ok);
        CHECK(read.reserved_zero);
        CHECK(read.length_fits);
        REQUIRE(read.container.has_value());
        REQUIRE(read.container->oamd().size() == oamd.size());
        CHECK(std::equal(oamd.begin(), oamd.end(), read.container->oamd().begin()));
        const auto parsed = oba::parse_payload(read.container->oamd());
        REQUIRE(parsed.has_value());
        CHECK(parsed->program.dynamic_only);
        CHECK(parsed->program.dynamic_objects == 3);
        REQUIRE(parsed->objects.size() == 3);
        ++frames;
    }
    CHECK(frames == static_cast<int>((encoded.units.size() + 1) / 2));
    CHECK(frames >= 5);
}
