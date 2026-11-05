#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "iclforge/base/bitwriter.hpp"
#include "iclforge/base/crypto/signing_key.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/signing/evolution_signer.hpp"
#include "platform/process.hpp"

// sign-objects, verify-objects and gate-objects on TrueHD, and the key-free tag count in probe: the
// real forge binary over a stream whose extra data holds an evolution frame (the encoder's, with
// the frame written here). The scheme is iclforge's own and the keys are made by the test from a
// seed when it runs; nothing is read from a fixture. The library's own cases are in
// libs/truehd/tests/signing.

namespace fs = std::filesystem;

namespace {

namespace th = iclforge::truehd;

constexpr int kExitUsage = 1;
constexpr int kExitInput = 2;

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_truehd_signing_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string cli_arg(const fs::path& path) {
    return "\"" + path.string() + "\" ";
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

std::string read_text(const fs::path& path) {
    const std::vector<std::byte> bytes = read_bytes(path);
    std::string text;
    for (const std::byte b : bytes) {
        if (b != std::byte{'\r'}) {
            text.push_back(static_cast<char>(b));
        }
    }
    return text;
}

void write_bytes(const fs::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

// A key file: random bytes from a seed, the first one above 0x7F so that the file reads as raw
// bytes and not as base64 or a hex dump.
fs::path write_key(const fs::path& dir, const std::string& name, std::uint32_t seed) {
    std::mt19937 generator(seed);
    std::vector<std::byte> bytes(32);
    for (std::byte& b : bytes) {
        b = static_cast<std::byte>(generator() & 0xFFU);
    }
    bytes[0] = std::byte{0xFF};
    const fs::path path = dir / name;
    write_bytes(path, bytes);
    return path;
}

// evo_frame() with one payload of id 11, a four byte primary field and two bytes of zero fill: the
// layout the library tests write independently (libs/truehd/tests/support/evolution_support.hpp).
std::vector<std::byte> frame_for(unsigned seed) {
    iclforge::BitWriter w;
    w.put(0, 2);
    w.put(1, 3);
    w.put(11, 5);
    w.put(0, 4);
    w.put(0, 1);
    w.put(1, 1);
    w.put(0, 2);
    w.put(31, 5);
    w.put(0, 2);
    w.put(24, 8);
    w.put(0, 1);
    for (unsigned i = 0; i < 24; ++i) {
        w.put((seed * 37U + i * 11U + 5U) & 0xFFU, 8);
    }
    w.put(0, 5);
    w.put(2, 2);
    w.put(0, 2);
    w.put(0, 32);  // the primary field, unsigned
    std::vector<std::byte> out = w.take();
    out.insert(out.end(), 2, std::byte{0});
    return out;
}

// Twelve access units of stereo, an evolution frame in every second one.
fs::path write_stream(const fs::path& dir, const std::string& name) {
    th::EncoderConfig config;
    config.restart_interval_units = 8;
    config.extra_data = [](std::uint64_t index) {
        return index % 2 == 0 ? frame_for(static_cast<unsigned>(index)) : std::vector<std::byte>{};
    };
    auto encoder = th::Encoder::create(config);
    REQUIRE(encoder.has_value());
    std::vector<std::int32_t> pcm(40 * 12 * 2);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::int32_t>((i * 7919U) % 20000U) - 10000;
    }
    std::vector<std::byte> stream;
    auto units = encoder->encode(pcm);
    REQUIRE(units.has_value());
    auto tail = encoder->flush();
    REQUIRE(tail.has_value());
    for (const auto* batch : {&*units, &*tail}) {
        for (const auto& unit : *batch) {
            stream.insert(stream.end(), unit.bytes.begin(), unit.bytes.end());
        }
    }
    const fs::path path = dir / name;
    write_bytes(path, stream);
    return path;
}

iclforge::base::crypto::SigningKey load_key(const fs::path& path) {
    return iclforge::base::crypto::SigningKey(read_bytes(path));
}

}  // namespace

TEST_CASE("metadata sign-objects signs the evolution frames and decode verify-objects checks them",
          "[forge-cli][truehd][signing]") {
    const fs::path dir = scratch_dir();
    const fs::path plain = write_stream(dir, "plain.thd");
    const fs::path key = write_key(dir, "a.key", 1);
    const fs::path other = write_key(dir, "b.key", 2);
    const fs::path signed_stream = dir / "signed.thd";
    const fs::path log = dir / "sign.log";
    const std::string signing_key = "signing-key=\"" + key.string() + "\"";

    REQUIRE(run_cli("metadata " + cli_arg(plain) + cli_arg(signed_stream) + "sign-objects " +
                        signing_key,
                    log) == 0);
    CHECK(read_text(log).find("6 evolution frame(s) signed") != std::string::npos);

    // The library agrees: six frames valid under the key, none under the other.
    const std::vector<std::byte> bytes = read_bytes(signed_stream);
    const auto good = th::signing::verify_evolution_stream(bytes, load_key(key));
    CHECK(good.valid == 6);
    CHECK(good.mismatch == 0);
    CHECK(th::signing::verify_evolution_stream(bytes, load_key(other)).mismatch == 6);
    // Nothing but the tags and parity bytes moved: the stream is the same length.
    CHECK(bytes.size() == read_bytes(plain).size());

    SECTION("verify-objects passes under the key and refuses another") {
        const fs::path wav = dir / "verified.wav";
        REQUIRE(run_cli("decode " + cli_arg(signed_stream) + cli_arg(wav) + "verify-objects " +
                            signing_key,
                        log) == 0);
        CHECK(read_text(log).find("object signature: 6 valid, 0 mismatched") != std::string::npos);
        fs::remove(wav);
        CHECK(run_cli("decode " + cli_arg(signed_stream) + cli_arg(wav) + "verify-objects " +
                          "signing-key=\"" + other.string() + "\"",
                      log) == kExitInput);
        CHECK(read_text(log).find("verification failed (6 of 6") != std::string::npos);
        CHECK_FALSE(fs::exists(wav));
    }
    SECTION("a keyring accepts either key") {
        const fs::path wav = dir / "ring.wav";
        REQUIRE(run_cli("decode " + cli_arg(signed_stream) + cli_arg(wav) + "verify-objects " +
                            "signing-key=\"" + other.string() + "\" " + signing_key,
                        log) == 0);
        CHECK(read_text(log).find("key 2") != std::string::npos);
    }
    SECTION("an unsigned stream does not verify") {
        const fs::path wav = dir / "unsigned.wav";
        CHECK(run_cli("decode " + cli_arg(plain) + cli_arg(wav) + "verify-objects " + signing_key,
                      log) == kExitInput);
    }
    SECTION("gate-objects keeps the signed units and plays the others as the bed") {
        const fs::path gated_wav = dir / "gated.wav";
        const fs::path plain_wav = dir / "plain.wav";
        REQUIRE(run_cli("decode " + cli_arg(signed_stream) + cli_arg(gated_wav) + "gate-objects " +
                            signing_key,
                        log) == 0);
        CHECK(read_text(log).find("6 unit(s) kept their objects") != std::string::npos);
        REQUIRE(run_cli("decode " + cli_arg(plain) + cli_arg(gated_wav) + "gate-objects " +
                            "signing-key=\"" + other.string() + "\"",
                        log) == 0);
        CHECK(read_text(log).find("6 played as the bed") != std::string::npos);
        // The bed is the same audio whether or not the object layer was there.
        REQUIRE(run_cli("decode " + cli_arg(plain) + cli_arg(plain_wav), log) == 0);
        CHECK(read_bytes(gated_wav) == read_bytes(plain_wav));
        // gate-objects and verify-objects are one policy or the other.
        CHECK(run_cli("decode " + cli_arg(plain) + cli_arg(gated_wav) +
                          "gate-objects verify-objects " + signing_key,
                      log) == kExitUsage);
        // The object channels of a gated unit are not read back in.
        CHECK(run_cli("decode " + cli_arg(plain) + cli_arg(gated_wav) +
                          "gate-objects truehd-presentation=all " + signing_key,
                      log) == kExitUsage);
        CHECK(read_text(log).find("truehd-presentation=all") != std::string::npos);
    }
    SECTION("probe says a tag is there without a key") {
        REQUIRE(run_cli("probe " + cli_arg(signed_stream), log) == 0);
        CHECK(read_text(log).find("protection tag present in 6/6 frame(s)") != std::string::npos);
        REQUIRE(run_cli("probe " + cli_arg(plain), log) == 0);
        CHECK(read_text(log).find("protection tag present in 0/6 frame(s)") != std::string::npos);
        REQUIRE(run_cli("probe " + cli_arg(signed_stream) + "json=1", log) == 0);
        const auto document = nlohmann::json::parse(read_text(log));
        const auto& protection = document["truehd"]["evolution"]["protection"];
        CHECK(protection["tagged_frames"] == 6);
        CHECK(protection["verified"] == false);
    }
}

TEST_CASE("the object signing options are applied only where they mean something",
          "[forge-cli][truehd][signing]") {
    const fs::path dir = scratch_dir();
    const fs::path plain = write_stream(dir, "plain_options.thd");
    const fs::path key = write_key(dir, "c.key", 3);
    const fs::path other = write_key(dir, "d.key", 4);
    const fs::path out = dir / "options_out.thd";
    const fs::path log = dir / "options.log";

    SECTION("sign-objects signs with one key") {
        CHECK(run_cli("metadata " + cli_arg(plain) + cli_arg(out) + "sign-objects signing-key=\"" +
                          key.string() + "\" signing-key=\"" + other.string() + "\"",
                      log) == kExitUsage);
        CHECK(read_text(log).find("signs with one key") != std::string::npos);
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("sign-objects needs a key") {
        CHECK(run_cli("metadata " + cli_arg(plain) + cli_arg(out) + "sign-objects", log) ==
              kExitUsage);
        const std::string text = read_text(log);
        CHECK((text.find("needs a key") != std::string::npos));
    }
    SECTION("sign-objects is refused on a decode, and verify-objects on a probe") {
        const fs::path wav = dir / "options.wav";
        CHECK(run_cli("decode " + cli_arg(plain) + cli_arg(wav) + "sign-objects", log) ==
              kExitUsage);
        CHECK(read_text(log).find("sign-objects") != std::string::npos);
        CHECK(run_cli("probe " + cli_arg(plain) + "verify-objects", log) == kExitUsage);
        CHECK(read_text(log).find("Nothing was verified") != std::string::npos);
    }
    SECTION("sign-objects on a stream with no evolution frame says so") {
        // Nothing to sign is not a success to pass over silently.
        iclforge::truehd::EncoderConfig config;
        config.restart_interval_units = 8;
        auto encoder = th::Encoder::create(config);
        REQUIRE(encoder.has_value());
        std::vector<std::int32_t> pcm(40 * 4 * 2, 100);
        std::vector<std::byte> stream;
        for (auto batch : {encoder->encode(pcm), encoder->flush()}) {
            REQUIRE(batch.has_value());
            for (const auto& unit : *batch) {
                stream.insert(stream.end(), unit.bytes.begin(), unit.bytes.end());
            }
        }
        const fs::path bare = dir / "bare.thd";
        write_bytes(bare, stream);
        REQUIRE(run_cli("metadata " + cli_arg(bare) + cli_arg(out) + "sign-objects signing-key=\"" +
                            key.string() + "\"",
                        log) == 0);
        const std::string text = read_text(log);
        CHECK(text.find("0 evolution frame(s) signed") != std::string::npos);
        CHECK(text.find("found no evolution frame to sign") != std::string::npos);
    }
}

TEST_CASE("no TrueHD command takes the content-signature options, and probe has no such line",
          "[forge-cli][truehd][signing]") {
    // The whole-stream content signature is AC-4's. TrueHD signs per frame (sign-objects) and
    // nothing else, so the names are refused, never accepted and dropped.
    const fs::path dir = scratch_dir();
    const fs::path plain = write_stream(dir, "removal.thd");
    const fs::path key = write_key(dir, "r.key", 7);
    const fs::path out = dir / "removal_out.thd";
    const fs::path wav = dir / "removal.wav";
    const fs::path log = dir / "removal.log";

    CHECK(run_cli("metadata " + cli_arg(plain) + cli_arg(out) + "sign-content", log) == kExitUsage);
    CHECK_FALSE(fs::exists(out));
    CHECK(run_cli("decode " + cli_arg(plain) + cli_arg(wav) + "verify-content", log) == kExitUsage);
    CHECK(read_text(log).find("Nothing was verified") != std::string::npos);
    CHECK(run_cli("decode " + cli_arg(plain) + cli_arg(wav) + "check-content", log) == kExitUsage);
    CHECK_FALSE(fs::exists(wav));

    // A signed stream's probe says the tags are there, and says nothing of a content signature.
    const fs::path signed_file = dir / "removal_signed.thd";
    REQUIRE(run_cli("metadata " + cli_arg(plain) + cli_arg(signed_file) +
                        "sign-objects signing-key=\"" + key.string() + "\"",
                    log) == 0);
    REQUIRE(run_cli("probe " + cli_arg(signed_file), log) == 0);
    const std::string text = read_text(log);
    CHECK(text.find("protection tag present in 6/6") != std::string::npos);
    CHECK(text.find("content sig") == std::string::npos);
    REQUIRE(run_cli("probe " + cli_arg(signed_file) + "json=1", log) == 0);
    const auto document = nlohmann::json::parse(read_text(log));
    CHECK_FALSE(document["truehd"].contains("content_signature"));
}

TEST_CASE("a channel-meaning edit breaks the tags it covers, says so, and sign-objects signs again",
          "[forge-cli][truehd][signing]") {
    // The tag covers the whole access unit, the major sync included, so an edit of the major sync
    // fails the tags that were there.
    const fs::path dir = scratch_dir();
    const fs::path plain = write_stream(dir, "edit_plain.thd");
    const fs::path key = write_key(dir, "e.key", 9);
    const fs::path signed_file = dir / "edit_signed.thd";
    const fs::path log = dir / "edit.log";
    const std::string signing_key = "signing-key=\"" + key.string() + "\"";
    REQUIRE(
        run_cli("metadata " + cli_arg(plain) + cli_arg(signed_file) + "sign-objects " + signing_key,
                log) == 0);

    const fs::path edited = dir / "edit_edited.thd";
    REQUIRE(run_cli("metadata " + cli_arg(signed_file) + cli_arg(edited) + "truehd-dialnorm-2ch=20",
                    log) == 0);
    CHECK(read_text(log).find("no longer verify") != std::string::npos);
    const fs::path wav = dir / "edit.wav";
    CHECK(run_cli("decode " + cli_arg(edited) + cli_arg(wav) + "verify-objects " + signing_key,
                  log) == kExitInput);

    const fs::path resigned = dir / "edit_resigned.thd";
    REQUIRE(run_cli("metadata " + cli_arg(signed_file) + cli_arg(resigned) +
                        "truehd-dialnorm-2ch=20 sign-objects " + signing_key,
                    log) == 0);
    CHECK(read_text(log).find("no longer verify") == std::string::npos);
    REQUIRE(run_cli("decode " + cli_arg(resigned) + cli_arg(wav) + "verify-objects " + signing_key,
                    log) == 0);
    CHECK(read_text(log).find("6 valid, 0 mismatched") != std::string::npos);
    // A stream with no tags says nothing: the edit broke none.
    REQUIRE(run_cli("metadata " + cli_arg(plain) + cli_arg(dir / "edit_plain_out.thd") +
                        "truehd-dialnorm-2ch=20",
                    log) == 0);
    CHECK(read_text(log).find("no longer verify") == std::string::npos);
}
