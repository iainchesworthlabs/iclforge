#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "probe_truehd.hpp"

// The stream-end gate of `forge qc` and the Forge window's QC (probe_truehd.hpp, terminator_report):
// the rules of the DEE findings report spec-answers-2026-11-04 D10, read from a decode's tail
// counters. The decoder counts what follows the last block of each segment and the number of the
// unit it was in; these cases give the report counters of that shape for each rule, and
// libs/truehd/tests/decoder/test_substream3.cpp holds that the decoder fills them.

namespace {

namespace th = iclforge::truehd;
using iclforge::apps::truehd_probe::terminator_report;

// A stream of `units` units whose substream 0 sent zero bits after its last block in all but the
// ones set by the caller.
th::DecoderCounters stream_of(std::uint64_t units) {
    th::DecoderCounters counters;
    counters.units = units;
    counters.tails[0].segments = units;
    counters.tails[0].zero = units;
    return counters;
}

void put_word(th::SegmentTailCounters& tail, std::uint64_t at_unit) {
    tail.end_marker = 1;
    tail.zero -= 1;
    tail.last_word_unit = at_unit;
}

void put_other(th::SegmentTailCounters& tail, std::uint64_t at_unit, std::uint32_t word) {
    tail.other = 1;
    tail.zero -= 1;
    tail.last_other_unit = at_unit;
    tail.last_other_word = word;
}

}  // namespace

TEST_CASE("a stream with a terminator word in its last unit, or none, passes the stream-end gate",
          "[truehd][terminators][qc]") {
    auto none = stream_of(100);
    CHECK(terminator_report(none).violations() == 0);
    CHECK(terminator_report(none).words == 0);

    auto ends = stream_of(100);
    put_word(ends.tails[0], 100);
    const auto report = terminator_report(ends);
    CHECK(report.violations() == 0);
    CHECK(report.words == 1);

    // Every substream of the last unit may carry one.
    for (std::size_t i = 0; i < 3; ++i) {
        ends.tails[i].segments = 100;
        ends.tails[i].zero = 99;
        ends.tails[i].end_marker = 1;
        ends.tails[i].last_word_unit = 100;
    }
    CHECK(terminator_report(ends).violations() == 0);
}

TEST_CASE("the end-of-stream marker in the last unit is a legal word the decoder counts as other data",
          "[truehd][terminators][qc]") {
    auto marker = stream_of(100);
    put_other(marker.tails[0], 100, 0xD234D234U);
    const auto report = terminator_report(marker);
    CHECK(report.violations() == 0);
    CHECK(report.words == 1);
}

TEST_CASE("a terminator word in a unit that is not the last fails (status 0xE5 in the player)",
          "[truehd][terminators][qc]") {
    auto early = stream_of(100);
    put_word(early.tails[0], 99);
    CHECK(terminator_report(early).in_non_last_unit == 1);
    CHECK(terminator_report(early).violations() == 1);

    auto first = stream_of(100);
    put_word(first.tails[0], 1);
    CHECK(terminator_report(first).violations() == 1);

    // A word in every unit, the last included: 99 of them are out of place.
    auto every = stream_of(100);
    every.tails[0].end_marker = 100;
    every.tails[0].zero = 0;
    every.tails[0].last_word_unit = 100;
    CHECK(terminator_report(every).in_non_last_unit == 99);

    // terminatorA right after the last block counts the same as one at the end.
    auto at_once = stream_of(100);
    at_once.tails[0].terminator_a = 1;
    at_once.tails[0].zero -= 1;
    at_once.tails[0].last_word_unit = 50;
    CHECK(terminator_report(at_once).violations() == 1);
}

TEST_CASE("anything but zeros after the last block of the last unit fails (status 0xBF)",
          "[truehd][terminators][qc]") {
    // 0xD234 and then neither form, the shifted pattern, and arbitrary data.
    for (const std::uint32_t word : {0xD234A000U, 0xD234D233U, 0x348D3000U, 0xDEADBEEFU, 0U}) {
        auto bad = stream_of(100);
        put_other(bad.tails[0], 100, word);
        const auto report = terminator_report(bad);
        CHECK(report.illegal_words == 1);
        CHECK(report.zero_samples_too_large == 0);
        CHECK(report.violations() == 1);
    }
    // In a unit that is not the last it is not a terminator rule: nothing is counted.
    auto early = stream_of(100);
    put_other(early.tails[0], 50, 0xD234A000U);
    CHECK(terminator_report(early).violations() == 0);
}

TEST_CASE("a zero_samples of 4096 or more fails by itself (status 0x98 in the player)",
          "[truehd][terminators][qc]") {
    // 0xD234E000 | zero_samples with bit 12 of the word set.
    for (const std::uint32_t count : {0x1000U, 0x1001U, 0x1FFFU}) {
        auto big = stream_of(100);
        put_other(big.tails[0], 100, 0xD234E000U | count);
        const auto report = terminator_report(big);
        CHECK(report.zero_samples_too_large == 1);
        CHECK(report.illegal_words == 0);
        CHECK(report.violations() == 1);
    }
    // 4095 is a legal count: the decoder takes the word as a terminator, so it never reaches here
    // as anything else, and a pattern with bit 12 clear is judged as an illegal word if it does.
    auto small = stream_of(100);
    put_other(small.tails[0], 100, 0xD234EFFFU);
    CHECK(terminator_report(small).zero_samples_too_large == 0);
}

TEST_CASE("the object substream's tail and substreams that did not decode are not judged",
          "[truehd][terminators][qc]") {
    auto counters = stream_of(100);
    counters.tails[3].end_marker = 7;
    counters.tails[3].last_word_unit = 3;
    counters.tails[3].other = 2;
    counters.tails[3].last_other_unit = 100;
    CHECK(terminator_report(counters).violations() == 0);
    CHECK(terminator_report(counters).words == 0);
}
