// The words of POST /identify and the JSON of GET /identify (render/identify_request.hpp).

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>

#include "iclforge/render/identify_request.hpp"

namespace {

using iclforge::render::IdentifyOutcome;
using iclforge::render::IdentifyReport;
using iclforge::render::IdentifyRequest;
using Shape = IdentifyRequest::Shape;
namespace st = iclforge::render::speaker_trim;

}  // namespace

TEST_CASE("the words of POST /identify", "[render][identify-request]") {
    const auto off = iclforge::render::parse_identify("off");
    REQUIRE(off.has_value());
    REQUIRE(off->shape == Shape::kOff);

    const auto all = iclforge::render::parse_identify(" ALL\n");
    REQUIRE(all.has_value());
    REQUIRE(all->shape == Shape::kEach);

    const auto left = iclforge::render::parse_identify("L");
    REQUIRE(left.has_value());
    REQUIRE(left->shape == Shape::kOnce);
    REQUIRE(left->role == *st::parse_role("L"));

    const auto surround = iclforge::render::parse_identify("ls loop\r\n");
    REQUIRE(surround.has_value());
    REQUIRE(surround->shape == Shape::kLoop);
    REQUIRE(surround->role == *st::parse_role("Ls"));

    const auto sub = iclforge::render::parse_identify("sub");
    REQUIRE(sub.has_value());
    REQUIRE(sub->role == *st::parse_role("LFE"));

    const auto by_slot = iclforge::render::parse_identify("S3 Loop");
    REQUIRE(by_slot.has_value());
    REQUIRE(by_slot->role == *st::parse_role("S3"));

    // The level is not a word: every request asks for the default, and the sequence caps whatever it is given.
    REQUIRE(left->level_db == iclforge::render::IdentifySequence::kDefaultLevelDb);

    for (const char* bad : {"", "  ", "on", "left", "L now", "L loop loop", "all loop", "off L", "loud", "L,loop", "S17"}) {
        INFO(bad);
        const bool ok = iclforge::render::parse_identify(bad).has_value();
        // "L,loop" is the one spelling a comma allows: a speaker and the word.
        REQUIRE(ok == (std::string(bad) == "L,loop"));
    }
}

TEST_CASE("every refusal says why, and only a start or a stop is accepted", "[render][identify-request]") {
    REQUIRE(iclforge::render::identify_accepted(IdentifyOutcome::kStarted));
    REQUIRE(iclforge::render::identify_accepted(IdentifyOutcome::kStopped));
    for (const IdentifyOutcome refused : {IdentifyOutcome::kPlaying, IdentifyOutcome::kBusy, IdentifyOutcome::kServer,
                                           IdentifyOutcome::kNoSuchSpeaker, IdentifyOutcome::kNoPlayer}) {
        REQUIRE_FALSE(iclforge::render::identify_accepted(refused));
        REQUIRE(std::string(iclforge::render::identify_outcome_text(refused)).size() > 20);
    }
}

TEST_CASE("GET /identify is JSON with the limits in it", "[render][identify-request]") {
    IdentifyReport report;
    report.available = true;
    report.speakers = {*st::parse_role("L"), *st::parse_role("R"), *st::parse_role("LFE")};
    report.speaker_count = 3;
    REQUIRE(iclforge::render::identify_json(report) ==
            "{\"available\":true,\"reason\":\"\",\"active\":false,\"output\":-1,\"role\":\"\",\"label\":\"\","
            "\"remaining_ms\":0,\"level_db\":-30.0,\"max_level_db\":-24.0,\"tone_ms\":1500,\"gap_ms\":500,"
            "\"total_ms\":60000,\"speakers\":[\"L\",\"R\",\"LFE\"]}");

    report.active = true;
    report.output = 1;
    report.role = *st::parse_role("R");
    report.remaining_ms = 43'210;
    const std::string running = iclforge::render::identify_json(report);
    REQUIRE(running.find("\"active\":true,\"output\":1,\"role\":\"R\",\"label\":\"Front right\",\"remaining_ms\":43210") != std::string::npos);

    IdentifyReport none;
    none.reason = "this board has no Sendspin player to play a tone through";
    REQUIRE(iclforge::render::identify_json(none).find(
                "\"available\":false,\"reason\":\"this board has no Sendspin player to play a tone through\"") != std::string::npos);
    // A reason is given only when there is none to play.
    none.available = true;
    REQUIRE(iclforge::render::identify_json(none).find("\"reason\":\"\"") != std::string::npos);
}
