// Tests for firmware_update.hpp: the part of "Check for updates" that is data and rules
// (planning/esp32-ota.md, O10). The sample index and signature file are a throwaway CA's, made by
// tools/hearth/sign_index.py selftest (the same files as tools/hearth/testdata/update_index/ec/):
// the signing tool wrote them and this suite reads them, so the two cannot drift. The cases of
// versions and of what an index may hold are also in tools/hearth/test_index_verify.py: one rule,
// two languages.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "iclforge/firmware_update.hpp"

using namespace iclforge::update;

namespace {

constexpr char kSampleIndex[] =
    R"json({"format":1,"channel":"stable","sequence":1793000000,"version":"v0.11.0-beta.1","released":"2026-11-05","images":[{"name":"hearth-sink-esp32s3","file":"hearth-sink-esp32s3-v0.11.0-beta.1.bin","size":1583104,"sha256":"02d6f1f144d1f5d65b002ef9345b956f9bcbee7e7022246c5d6aec3236fd14a1","project":"iclforge_hearth_sink","chip_id":9,"min_rev_full":0,"max_rev_full":99,"flash_size":"16MB","psram":true,"secure_version":0}]})json";
constexpr char kSampleSignature[] =
    R"json({"format":1,"alg":"ecdsa-p256-sha256","sha256":"b0d2ba4a6a4442c478fc843d494551eaa75447702dafde58450aaf2be020553a","chain":["MIIB1TCCAXygAwIBAgIUL9ti+AYPwG2u4snDkSjYnIWU2CYwCgYIKoZIzj0EAwIwPjEcMBoGA1UECgwTVGhyb3dhd2F5IFRlc3QgTGFiczEeMBwGA1UEAwwVVGhyb3dhd2F5IEZpcm13YXJlIENBMB4XDTI2MTAyNTA1MzQ1OFoXDTI4MTAyNDA1Mzk1OFowPzEcMBoGA1UECgwTVGhyb3dhd2F5IFRlc3QgTGFiczEfMB0GA1UEAwwWSGVhcnRoIEZpcm13YXJlIFN0YWJsZTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABGPekbkcuPohw6ZcKhVLizE+Ys16Hy7wPMwTRPp6GL6W9cMnAoH9lLwLPVYMHquuQcNeP7krLzbsgwL/SjMWChujVzBVMAwGA1UdEwEB/wQCMAAwHQYDVR0OBBYEFKONIkAeGUKswmwbSCh+3v7L/jQzMA4GA1UdDwEB/wQEAwIHgDAWBgNVHSUBAf8EDDAKBggrBgEFBQcDAzAKBggqhkjOPQQDAgNHADBEAiA4xV1hizs1Q/YAWAg1yrlNlz64cmu27g9SE59P+9JzxQIgU08K/0GZKTsuomWpoj2erSBysTTfdnHzMbJYjxFrxBQ=","MIIBvDCCAWKgAwIBAgIUHb0GArSQxx4yah1J2Bcy7xu+kzEwCgYIKoZIzj0EAwIwNzEcMBoGA1UECgwTVGhyb3dhd2F5IFRlc3QgTGFiczEXMBUGA1UEAwwOVGhyb3dhd2F5IFJvb3QwHhcNMjYxMDI1MDUzNDU4WhcNMzYxMDIyMDUzOTU4WjA+MRwwGgYDVQQKDBNUaHJvd2F3YXkgVGVzdCBMYWJzMR4wHAYDVQQDDBVUaHJvd2F3YXkgRmlybXdhcmUgQ0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATSdWqwrRu9zRsguh5s281O3dFjj4NrXuSbBb22QKdT/XbS63GJGbaACKf62ffUFI2V7qs636eohCHOVGozMph6o0UwQzASBgNVHRMBAf8ECDAGAQH/AgEAMB0GA1UdDgQWBBRb+wVScO9U09KnbdamThJ0oSTlzzAOBgNVHQ8BAf8EBAMCAQYwCgYIKoZIzj0EAwIDSAAwRQIhALPwV6Y9edsc6QQFH/XnBqQV1eMcTaaJiYzE3T44/edhAiB8k7Cwc9usnIOcCVk1FD9Eeh+5/54KCL7TPNFVPikR7w=="],"signature":"ie0kOUlZsbjl0FKv0w0VIv7ikX6FLXxMyjJjcNczNL1Gm0+C9IdYt2kBQGVXTqUF4CAO4xuyKJVNkaPhLv6KCA=="})json";

std::string index_with(const std::string& from, const std::string& to) {
    std::string text = kSampleIndex;
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

UpdateIndex sample() {
    const ParsedIndex parsed = parse_index(kSampleIndex);
    REQUIRE(parsed.index.has_value());
    return *parsed.index;
}

BoardTarget s3_board() {
    BoardTarget board;
    board.project = "iclforge_hearth_sink";
    board.chip_id = 9;
    board.revision_full = 2;
    board.flash_mb = 16;
    board.psram = true;
    board.slot_bytes = 0x400000;
    return board;
}

PullFacts running(std::string version) {
    PullFacts facts;
    facts.channel = "stable";
    facts.running_version = std::move(version);
    return facts;
}

}  // namespace

TEST_CASE("versions order the way an update needs", "[firmware-update]") {
    struct Case {
        const char* a;
        const char* b;
        int order;  // of a against b
    };
    constexpr Case kCases[] = {
        {"v0.10.0-beta.1", "v0.11.0-beta.1", -1},
        {"v0.10.0-beta.1-2039-gf5aded471", "v0.11.0-beta.1", -1},
        {"v0.10.0-beta.1", "v0.10.0-beta.1-2039-gf5aded471", -1},
        {"v0.10.0-beta.1-2039-gf5aded471", "v0.10.0-beta.1", 1},
        {"v0.10.0-beta.2", "v0.10.0-beta.1-2039-gabc", 1},
        {"v0.10.0-beta.1", "v0.10.0", -1},
        {"v0.10.0-rc.1", "v0.10.0-beta.9", 1},
        {"v0.10.0-alpha.3", "v0.10.0-beta.1", -1},
        {"0.10.0", "v0.10.0", 0},
        {"v0.9.9", "v0.10.0-alpha.1", -1},
        {"v1.0.0", "v0.99.99", 1},
        {"v0.10.0-beta.1-5-gabc-dirty", "v0.10.0-beta.1-5-gdef", 0},
    };
    for (const Case& c : kCases) {
        INFO(c.a << " against " << c.b);
        const auto a = parse_version(c.a);
        const auto b = parse_version(c.b);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        const int order = (*b < *a) - (*a < *b);
        CHECK(order == c.order);
    }
}

TEST_CASE("text that is not a version is not one", "[firmware-update]") {
    for (const char* text : {"", "1", "fa3b9c2", "v1.2", "v1.2.3.4", "main", "v1.2.3-beta",
                             "v1.2.3-gamma.1", "v1.2.3\n", "v1.2.3 ", "v1.2.3-5", "v1.2.3-5-x",
                             "v1234567890.0.0", "v1.2.3-dirty-dirty"}) {
        INFO("'" << text << "'");
        CHECK_FALSE(parse_version(text).has_value());
    }
}

TEST_CASE("a version that cannot be understood is never newer", "[firmware-update]") {
    CHECK_FALSE(compare_versions("fa3b9c2", "v0.10.0").has_value());
    CHECK_FALSE(compare_versions("v0.10.0", "1").has_value());
    CHECK(*compare_versions("v0.11.0", "v0.10.0") == Order::kNewer);
    CHECK(*compare_versions("v0.10.0", "v0.11.0") == Order::kOlder);
    CHECK(*compare_versions("v0.10.0", "v0.10.0-dirty") == Order::kSame);
}

TEST_CASE("base64 is strict", "[firmware-update]") {
    const auto man = base64_decode("TWFu");
    REQUIRE(man.has_value());
    CHECK(std::string(man->begin(), man->end()) == "Man");
    const auto ma = base64_decode("TWE=");
    REQUIRE(ma.has_value());
    CHECK(std::string(ma->begin(), ma->end()) == "Ma");
    const auto m = base64_decode("TQ==");
    REQUIRE(m.has_value());
    CHECK(std::string(m->begin(), m->end()) == "M");
    for (const char* bad :
         {"", "TWF", "TWFu\n", "TW=u", "T===", "TQ==TQ==", "TWF*", "TQ=a", "=QQQ"}) {
        INFO("'" << bad << "'");
        CHECK_FALSE(base64_decode(bad).has_value());
    }
}

TEST_CASE("the small JSON reader is bounded and strict", "[firmware-update]") {
    auto ok = json::parse(R"({"a":[1,-2,true,false,null,"x\n\u00e9\"\\"],"b":{"c":1.5e3}})");
    REQUIRE(ok.value.has_value());
    const json::Value* a = ok.value->find("a");
    REQUIRE(a != nullptr);
    REQUIRE(a->items.size() == 6);
    CHECK(a->items[0].integral);
    CHECK(a->items[0].integer == 1);
    CHECK(a->items[1].integer == -2);
    CHECK(a->items[2].boolean);
    CHECK(a->items[5].text == "x\n\xC3\xA9\"\\");
    CHECK_FALSE(ok.value->find("b")->find("c")->integral);

    for (const char* bad :
         {"", "{", "[1,]", "{\"a\":1,}", "{\"a\":1 \"b\":2}", "{\"a\"}", "01", "1.", "1e", "-",
          "\"abc", "\"\\q\"", "\"\\ud800\"", "tru", "{} x", "{\"a\":1,\"a\":2}", "\"\x01\""}) {
        INFO("'" << bad << "'");
        CHECK_FALSE(json::parse(bad).value.has_value());
    }
    CHECK_FALSE(json::parse(std::string(10, '[') + std::string(10, ']')).value.has_value());
    CHECK(json::parse(std::string(9, '[') + std::string(9, ']')).value.has_value());
    std::string many = "[";
    for (int i = 0; i < 1100; ++i) {
        many += "1,";
    }
    many += "1]";
    CHECK_FALSE(json::parse(many).value.has_value());
    CHECK_FALSE(json::parse("\"" + std::string(5000, 'a') + "\"").value.has_value());
}

TEST_CASE("the index the signing tool made is read", "[firmware-update]") {
    const UpdateIndex index = sample();
    CHECK(index.channel == "stable");
    CHECK(index.sequence == 1793000000U);
    CHECK(index.version == "v0.11.0-beta.1");
    CHECK(index.released == "2026-11-05");
    REQUIRE(index.images.size() == 1);
    const IndexImage& image = index.images[0];
    CHECK(image.name == "hearth-sink-esp32s3");
    CHECK(image.file == "hearth-sink-esp32s3-v0.11.0-beta.1.bin");
    CHECK(image.size == 1583104U);
    CHECK(image.sha256.size() == 64);
    CHECK(image.project == "iclforge_hearth_sink");
    CHECK(image.chip_id == 9U);
    CHECK(image.min_rev_full == 0U);
    CHECK(image.max_rev_full == 99U);
    CHECK(image.flash_mb == 16U);
    CHECK(image.psram);
    CHECK(image.secure_version == 0U);
}

TEST_CASE("an index is refused for each way it may not look", "[firmware-update]") {
    struct Case {
        const char* from;
        const char* to;
    };
    constexpr Case kCases[] = {
        {"\"format\":1", "\"format\":2"},
        {"\"channel\":\"stable\"", "\"channel\":\"nightly\""},
        {"\"sequence\":1793000000", "\"sequence\":0"},
        {"\"sequence\":1793000000", "\"sequence\":4294967296"},
        {"\"sequence\":1793000000", "\"sequence\":true"},
        {"\"sequence\":1793000000", "\"sequence\":1.5"},
        {"\"version\":\"v0.11.0-beta.1\"", "\"version\":\"1\""},
        {"\"released\":\"2026-11-05\"", "\"released\":\"yesterday\""},
        {"\"images\":[", "\"images\":[],\"x\":["},
        {"\"file\":\"hearth-sink-esp32s3-v0.11.0-beta.1.bin\"", "\"file\":\"../evil.bin\""},
        {"\"file\":\"hearth-sink-esp32s3-v0.11.0-beta.1.bin\"", "\"file\":\"a/b.bin\""},
        {"\"file\":\"hearth-sink-esp32s3-v0.11.0-beta.1.bin\"", "\"file\":\".hidden\""},
        {"\"file\":\"hearth-sink-esp32s3-v0.11.0-beta.1.bin\"", "\"file\":\"\""},
        {"\"size\":1583104", "\"size\":0"},
        {"\"size\":1583104", "\"size\":17825792"},
        {"\"flash_size\":\"16MB\"", "\"flash_size\":\"16 MB\""},
        {"\"psram\":true", "\"psram\":\"yes\""},
        {"\"chip_id\":9", "\"chip_id\":-1"},
        {"\"project\":\"iclforge_hearth_sink\",", ""},
        {"\"secure_version\":0", "\"secure_version\":-1"},
    };
    for (const Case& c : kCases) {
        INFO(c.from << " -> " << c.to);
        const ParsedIndex parsed = parse_index(index_with(c.from, c.to));
        CHECK_FALSE(parsed.index.has_value());
        CHECK_FALSE(parsed.why.empty());
    }
    std::string upper = kSampleIndex;
    const auto hash_at = upper.find("\"sha256\":\"") + 10;
    upper[hash_at] = 'A';
    CHECK_FALSE(parse_index(upper).index.has_value());
    CHECK_FALSE(parse_index(std::string(kMaxIndexBytes + 1, ' ')).index.has_value());
    CHECK_FALSE(parse_index("[]").index.has_value());
}

TEST_CASE("an index with keys a later one adds is still read", "[firmware-update]") {
    CHECK(parse_index(index_with("\"channel\"", "\"min_leaf_issued\":\"2026-11-01\",\"channel\""))
              .index.has_value());
    CHECK(parse_index(index_with("\"psram\":true", "\"psram\":true,\"note\":[1,{\"x\":2}]"))
              .index.has_value());
}

TEST_CASE("the signature file the signing tool made is read", "[firmware-update]") {
    const ParsedSignature parsed = parse_signature_file(kSampleSignature);
    REQUIRE(parsed.file.has_value());
    CHECK(parsed.code.empty());
    CHECK(parsed.file->algorithm == SignatureAlgorithm::kEcdsaP256Sha256);
    CHECK(std::string(signature_algorithm_name(parsed.file->algorithm)) == "ecdsa-p256-sha256");
    REQUIRE(parsed.file->chain.size() == 2);
    CHECK(parsed.file->chain[0][0] == 0x30);  // a DER SEQUENCE: a certificate
    CHECK(parsed.file->chain[1][0] == 0x30);
    CHECK(parsed.file->signature.size() == 64);
    CHECK(parsed.file->index_sha256.size() == 64);
}

TEST_CASE("a signature file that is not one is refused, not crashed on", "[firmware-update]") {
    for (const char* bad :
         {"not json", "[]", "{}", "{\"format\":2}", "{\"format\":1,\"alg\":\"md5\"}",
          "{\"format\":1,\"alg\":\"rsa-pss-sha256\",\"chain\":[],\"signature\":\"AAAA\"}",
          "{\"format\":1,\"alg\":\"rsa-pss-sha256\",\"chain\":[\"AAAA\",\"AAAA\",\"AAAA\"],"
          "\"signature\":\"AAAA\"}",
          "{\"format\":1,\"alg\":\"rsa-pss-sha256\",\"chain\":[5],\"signature\":\"AAAA\"}",
          "{\"format\":1,\"alg\":\"rsa-pss-sha256\",\"chain\":[\"***\"],\"signature\":\"AAAA\"}",
          "{\"format\":1,\"alg\":\"rsa-pss-sha256\",\"chain\":[\"AAAA\"],\"signature\":\"**\"}",
          "{\"format\":1,\"alg\":\"rsa-pss-sha256\",\"chain\":[\"AAAA\"]}"}) {
        INFO(bad);
        const ParsedSignature parsed = parse_signature_file(bad);
        CHECK_FALSE(parsed.file.has_value());
        CHECK(parsed.code == "bad_sig_file");
        CHECK_FALSE(parsed.why.empty());
    }
    CHECK_FALSE(parse_signature_file(std::string(kMaxSignatureBytes + 1, ' ')).file.has_value());
}

TEST_CASE("a signature file knows which index it was made for", "[firmware-update]") {
    const ParsedSignature parsed = parse_signature_file(kSampleSignature);
    REQUIRE(parsed.file.has_value());
    CHECK(signature_is_for(*parsed.file, parsed.file->index_sha256));
    CHECK_FALSE(signature_is_for(*parsed.file, std::string(64, '0')));
    SignatureFile none;
    CHECK_FALSE(signature_is_for(none, ""));
}

TEST_CASE("an image is chosen by what the board is", "[firmware-update]") {
    UpdateIndex index = sample();
    IndexImage c6 = index.images[0];
    c6.name = "hearth-sink-esp32c6";
    c6.chip_id = 13;
    c6.flash_mb = 4;
    c6.psram = false;
    IndexImage p4_old = index.images[0];
    p4_old.name = "p4-rev1";
    p4_old.chip_id = 18;
    p4_old.min_rev_full = 100;
    p4_old.max_rev_full = 199;
    IndexImage p4_new = p4_old;
    p4_new.name = "p4-rev3";
    p4_new.min_rev_full = 300;
    p4_new.max_rev_full = 399;
    index.images.push_back(c6);
    index.images.push_back(p4_old);
    index.images.push_back(p4_new);

    BoardTarget board = s3_board();
    CHECK(select_image(index, board)->name == "hearth-sink-esp32s3");
    board.chip_id = 13;
    board.flash_mb = 4;
    board.psram = false;
    CHECK(select_image(index, board)->name == "hearth-sink-esp32c6");
    board = s3_board();
    board.chip_id = 18;
    board.revision_full = 103;
    CHECK(select_image(index, board)->name == "p4-rev1");
    board.revision_full = 301;
    CHECK(select_image(index, board)->name == "p4-rev3");
    board.revision_full = 250;
    CHECK(select_image(index, board) == nullptr);

    board = s3_board();
    board.project = "another_project";
    CHECK(select_image(index, board) == nullptr);
    board = s3_board();
    board.psram = false;
    CHECK(select_image(index, board) == nullptr);
    board = s3_board();
    board.flash_mb = 8;
    CHECK(select_image(index, board) == nullptr);
    board = s3_board();
    board.slot_bytes = 1'000'000;  // the image is 1,583,104 bytes
    CHECK(select_image(index, board) == nullptr);
}

TEST_CASE("an index is decided on", "[firmware-update]") {
    const UpdateIndex index = sample();  // stable, sequence 1793000000, v0.11.0-beta.1
    const BoardTarget board = s3_board();

    SECTION("a newer release is available") {
        const Decision d = decide(index, board, running("v0.10.0-beta.1-2039-gf5aded471"));
        CHECK(d.verdict == Verdict::kAvailable);
        REQUIRE(d.image != nullptr);
        CHECK(d.image->name == "hearth-sink-esp32s3");
        CHECK_FALSE(d.skipped);
    }
    SECTION("the same release, or a newer build, is up to date") {
        CHECK(decide(index, board, running("v0.11.0-beta.1")).verdict == Verdict::kUpToDate);
        CHECK(decide(index, board, running("v0.11.0-beta.1-3-gabc")).verdict == Verdict::kUpToDate);
        CHECK(decide(index, board, running("v0.12.0")).verdict == Verdict::kUpToDate);
        CHECK(decide(index, board, running("v0.12.0")).image == nullptr);
    }
    SECTION("a version it cannot compare is refused, not guessed") {
        const Decision d = decide(index, board, running("fa3b9c2"));
        CHECK(d.verdict == Verdict::kRefused);
        CHECK(d.code == "unknown_version");
    }
    SECTION("the channel must be the one the board follows") {
        PullFacts facts = running("v0.10.0");
        facts.channel = "beta";
        CHECK(decide(index, board, facts).code == "bad_index");
    }
    SECTION("an older index than one the board has seen is a replay") {
        PullFacts facts = running("v0.10.0");
        facts.last_sequence = 1793000001U;
        CHECK(decide(index, board, facts).code == "replayed");
        facts.last_sequence = 1793000000U;  // the same one again is fine
        CHECK(decide(index, board, facts).verdict == Verdict::kAvailable);
    }
    SECTION("a release with no image for the board says so") {
        BoardTarget other = board;
        other.chip_id = 13;
        const Decision d = decide(index, other, running("v0.10.0"));
        CHECK(d.verdict == Verdict::kRefused);
        CHECK(d.code == "no_image");
    }
    SECTION("an image below the running security level is not offered") {
        PullFacts facts = running("v0.10.0");
        facts.running_secure_version = 1;
        const Decision d = decide(index, board, facts);
        CHECK(d.code == "older_security");
        CHECK(d.image == nullptr);
        UpdateIndex higher = index;
        higher.images[0].secure_version = 2;
        CHECK(decide(higher, board, facts).verdict == Verdict::kAvailable);
    }
    SECTION("a version that went back on this board is offered with a warning") {
        PullFacts facts = running("v0.10.0");
        facts.skipped_version = "v0.11.0-beta.1";
        const Decision d = decide(index, board, facts);
        CHECK(d.verdict == Verdict::kAvailable);
        CHECK(d.skipped);
        facts.skipped_version = "v0.10.9";
        CHECK_FALSE(decide(index, board, facts).skipped);
    }
}

TEST_CASE("addresses are built from the board's settings, never the index's", "[firmware-update]") {
    constexpr const char* kBase = "https://github.com/iainchesworthlabs/iclforge/releases/download";
    CHECK(index_url(kBase, "beta", false) ==
          "https://github.com/iainchesworthlabs/iclforge/releases/download/firmware-channels/"
          "hearth-update-beta.json");
    CHECK(index_url(kBase, "stable", true) ==
          "https://github.com/iainchesworthlabs/iclforge/releases/download/firmware-channels/"
          "hearth-update-stable.sig.json");
    CHECK(*image_url(kBase, "v0.11.0-beta.1", "hearth-sink-esp32s3-v0.11.0-beta.1.bin") ==
          "https://github.com/iainchesworthlabs/iclforge/releases/download/v0.11.0-beta.1/"
          "hearth-sink-esp32s3-v0.11.0-beta.1.bin");
    CHECK_FALSE(image_url(kBase, "v0.11.0", "../x").has_value());
    CHECK_FALSE(image_url(kBase, "../v0.11.0", "x.bin").has_value());
    CHECK_FALSE(image_url(kBase, "v0.11.0", "a?b").has_value());
    CHECK(notes_url(kBase, "v0.11.0-beta.1") ==
          "https://github.com/iainchesworthlabs/iclforge/releases/tag/v0.11.0-beta.1");
    CHECK(notes_url("https://example.com/files", "v1.0.0").empty());
}

TEST_CASE("what a board cannot do for good is said first and plainly", "[firmware-update]") {
    SupportFacts facts;
    facts.pull_built = true;
    facts.trust_anchors = 1;
    facts.has_network = true;
    facts.has_clock = true;
    facts.has_second_slot = true;
    CHECK(unsupported_reason(facts).empty());

    SupportFacts none = facts;
    none.has_network = false;
    CHECK(unsupported_reason(none) == "This board has no network.");
    none = facts;
    none.has_second_slot = false;
    CHECK(unsupported_reason(none).find("one app slot") != std::string::npos);
    none = facts;
    none.pull_built = false;
    CHECK(unsupported_reason(none) == "This firmware has no room for a secure connection.");
    none = facts;
    none.trust_anchors = 0;
    CHECK(unsupported_reason(none) == "This firmware carries no update key.");
    none = facts;
    none.has_clock = false;
    CHECK(unsupported_reason(none).find("needs the date") != std::string::npos);
}

TEST_CASE("what stops a check right now is said in the order that matters", "[firmware-update]") {
    BlockFacts facts;
    facts.network_up = true;
    facts.clock_set = true;
    CHECK(blocked_reason(facts).empty());

    facts.room_beside_stream = false;
    CHECK(blocked_reason(facts) == "Stop what is playing and try again.");
    facts.clock_set = false;
    CHECK(blocked_reason(facts) == "The player does not know the date yet.");
    facts.time_sync_on = false;
    CHECK(blocked_reason(facts) == "Time sync is off.");
    facts.network_up = false;
    CHECK(blocked_reason(facts) == "The player has no network address yet.");
    facts.on_trial = true;
    facts.trial_minutes_left = 4;
    CHECK(blocked_reason(facts) ==
          "This version is still being tried; wait 4 minutes, or go back to the previous one.");
    facts.trial_minutes_left = 0;
    CHECK(blocked_reason(facts).find("wait 1 minute,") != std::string::npos);
    facts.flash_mode = true;
    CHECK(blocked_reason(facts) == "The player is waiting for a firmware upload.");
    facts.update_running = true;
    CHECK(blocked_reason(facts) == "An update is already running.");
}

TEST_CASE("every failure code has words", "[firmware-update]") {
    for (const char* code :
         {"no_network",     "no_clock",           "tls",           "unreachable",
          "bad_index",      "bad_sig_file",       "hash_mismatch", "untrusted",
          "leaf_usage",     "weak_key",           "bad_signature", "leaf_name",
          "cert_expired",   "cert_not_yet_valid", "replayed",      "no_image",
          "no_memory",      "bad_image",          "bad_hash",      "unknown_version",
          "older_security", "write_failed",       "no_slot",       "on_trial",
          "busy",           "nothing_offered",    "offer_stale",   "not_supported"}) {
        INFO(code);
        const std::string text = error_text(code);
        CHECK_FALSE(text.empty());
        CHECK(text.find("Something went wrong") == std::string::npos);
    }
    CHECK(error_text("novel") == "Something went wrong (novel).");
}

TEST_CASE("the status renders every key, null where it does not apply", "[firmware-update]") {
    UpdateStatus status;
    status.channel = "beta";
    status.running = "v0.10.0-beta.1";
    CHECK(render_status(status) ==
          "{\"supported\":false,\"why\":null,\"blocked\":null,\"state\":\"idle\",\"channel\":"
          "\"beta\","
          "\"running\":\"v0.10.0-beta.1\",\"checked\":null,\"latest\":null,"
          "\"trust\":{\"anchors\":0,\"source\":\"build\",\"fingerprints\":[]},\"signed_by\":null,"
          "\"error\":null,\"skipped\":null}\n");

    status.supported = true;
    status.state = State::kAvailable;
    status.checked = 1793000100;
    status.latest = Latest{"v0.11.0-beta.1", "2026-11-05", 1583104,
                           "https://example.test/tag/v0.11.0-beta.1", "hearth-sink-esp32s3"};
    status.trust.anchors = 1;
    status.trust.fingerprints = {"3f2a91bc"};
    status.signed_by = "Hearth Firmware Beta";
    status.skipped = "v0.10.9";
    CHECK(
        render_status(status) ==
        "{\"supported\":true,\"why\":null,\"blocked\":null,\"state\":\"available\",\"channel\":"
        "\"beta\","
        "\"running\":\"v0.10.0-beta.1\",\"checked\":1793000100,"
        "\"latest\":{\"version\":\"v0.11.0-beta.1\",\"released\":\"2026-11-05\",\"size\":1583104,"
        "\"notes\":\"https://example.test/tag/v0.11.0-beta.1\",\"image\":\"hearth-sink-esp32s3\"},"
        "\"trust\":{\"anchors\":1,\"source\":\"build\",\"fingerprints\":[\"3f2a91bc\"]},"
        "\"signed_by\":\"Hearth Firmware Beta\",\"error\":null,\"skipped\":\"v0.10.9\"}\n");

    status.state = State::kFailed;
    status.error_code = "untrusted";
    status.error_text = error_text("untrusted");
    status.blocked = "An update is already running.";
    const std::string failed = render_status(status);
    CHECK(failed.find("\"error\":{\"code\":\"untrusted\",\"text\":\"The update list is not "
                      "trusted: nothing was changed.\"}") != std::string::npos);
    CHECK(failed.find("\"blocked\":\"An update is already running.\"") != std::string::npos);
}

TEST_CASE("the status says what a person reads", "[firmware-update]") {
    UpdateStatus status;
    status.supported = true;
    status.channel = "beta";
    status.running = "v0.10.0-beta.1";

    CHECK(status_sentence(status) == "Version v0.10.0-beta.1. Channel: beta.");
    status.state = State::kChecking;
    CHECK(status_sentence(status) == "Checking for updates...");
    status.state = State::kUpToDate;
    CHECK(status_sentence(status, "14:02") ==
          "Version v0.10.0-beta.1. You have the newest version. Checked 14:02");
    CHECK(status_sentence(status) == "Version v0.10.0-beta.1. You have the newest version.");

    status.state = State::kAvailable;
    status.latest = Latest{"v0.11.0-beta.1", "2026-11-05", 1583104, "", "hearth-sink-esp32s3"};
    CHECK(status_sentence(status) == "Version v0.11.0-beta.1 is available (1.6 MB, 5 Nov).");
    status.skipped = "v0.11.0-beta.1";
    CHECK(
        status_sentence(status) ==
        "Version v0.11.0-beta.1 is available (1.6 MB, 5 Nov). It went back on this player before.");

    status.state = State::kInstalling;
    status.stage = "downloading";
    status.percent = 62;
    CHECK(status_sentence(status) == "Downloading v0.11.0-beta.1 62%");
    status.percent = -1;
    CHECK(status_sentence(status) == "Downloading v0.11.0-beta.1...");
    status.stage = "checking";
    CHECK(status_sentence(status) == "Checking the download of v0.11.0-beta.1...");
    status.stage = "restarting";
    CHECK(status_sentence(status) == "Restarting into v0.11.0-beta.1...");

    status.state = State::kFailed;
    status.error_code = "cert_expired";
    CHECK(status_sentence(status) ==
          "The update certificate has expired, or this player's date is wrong.");

    status.supported = false;
    status.why = "This firmware carries no update key.";
    CHECK(status_sentence(status) == "This firmware carries no update key.");
}
