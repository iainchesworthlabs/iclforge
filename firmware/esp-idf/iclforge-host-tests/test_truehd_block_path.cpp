// The component's TrueHD path from the Sendspin ring to the sink, on the host: TruehdStream's blocks
// of several access units, and the BurstPlayer that renders them, held to the output the player
// gave when every access unit was a block of its own (BurstPlayerConfig::truehd_block_samples = one
// unit), sample for sample and bit for bit.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "harness.hpp"
#include "host_control.hpp"
#include "iclforge/render/speaker_trim.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "truehd_stream.hpp"

namespace {

namespace th = iclforge::truehd;
using iclforge::TruehdStream;

#ifndef ICLFORGE_TESTDATA_DIR
#error "ICLFORGE_TESTDATA_DIR names the repository's testdata directory"
#endif

[[nodiscard]] std::vector<std::uint8_t> fixture(const std::string& name) {
    return fwhost::read_file(std::string(ICLFORGE_TESTDATA_DIR) + "/" + name);
}

// --- TruehdStream alone ------------------------------------------------------------------------

struct Collected {
    // Each block: its first unit's number, its length in samples, its unit length, its channels.
    struct BlockInfo {
        int index = 0;
        std::size_t samples = 0;
        std::size_t unit_samples = 0;
        int channels = 0;
        iclforge::ac3::eac3::chanmap::Layout bed;
    };
    std::vector<BlockInfo> blocks;
    // Every channel's samples, block after block.
    std::vector<std::vector<float>> planes;
};

void collect(void* context, const TruehdStream::Block& block) {
    auto& c = *static_cast<Collected*>(context);
    c.blocks.push_back({.index = block.index,
                        .samples = block.samples,
                        .unit_samples = block.unit_samples,
                        .channels = static_cast<int>(block.channels.size()),
                        .bed = block.bed});
    if (c.planes.size() < block.channels.size()) {
        c.planes.resize(block.channels.size());
    }
    for (std::size_t ch = 0; ch < block.channels.size(); ++ch) {
        REQUIRE(block.channels[ch].size() == block.samples);
        c.planes[ch].insert(c.planes[ch].end(), block.channels[ch].begin(), block.channels[ch].end());
    }
}

struct StreamRun {
    Collected collected;
    std::vector<TruehdStream::Result> results;
    std::vector<std::string> reasons;
};

// The stream's chunks of `units_per_chunk` units through a TruehdStream with the block bound given.
[[nodiscard]] StreamRun through_stream(const std::vector<std::uint8_t>& bytes, std::uint32_t rate, std::size_t block_samples,
                                       std::size_t max_channels = 0, bool ahead = false, std::size_t units_per_chunk = 24) {
    const auto units = fwhost::split_units(bytes);
    TruehdStream stream;
    REQUIRE(stream.open({.max_channels = max_channels,
                         .sample_rate_hz = rate,
                         .max_block_samples = block_samples,
                         .max_run_samples = ahead ? TruehdStream::kMaxRunSamples : 0}));
    StreamRun run;
    std::vector<std::uint8_t> chunk;
    for (std::size_t first = 0; first < units.size(); first += units_per_chunk) {
        chunk.clear();
        for (std::size_t u = first; u < std::min(units.size(), first + units_per_chunk); ++u) {
            chunk.insert(chunk.end(), units[u].begin(), units[u].end());
        }
        run.results.push_back(stream.decode(std::as_bytes(std::span<const std::uint8_t>(chunk)), &collect, &run.collected));
        run.reasons.emplace_back(stream.reason());
        if (run.results.back() == TruehdStream::Result::kRefused) {
            break;
        }
    }
    return run;
}

// What today's conversion gives for the stream: the decoder's samples, converted to float with one
// multiply by 2^-23, a unit at a time, a channel's samples end to end.
[[nodiscard]] std::vector<std::vector<float>> reference_planes(const std::vector<std::uint8_t>& bytes) {
    th::Decoder decoder(th::DecoderConfig{});
    std::vector<std::vector<float>> planes;
    for (const auto unit : fwhost::split_units(bytes)) {
        const auto decoded = decoder.decode(std::as_bytes(unit));
        REQUIRE(decoded);
        if (!decoded->has_value()) {
            continue;
        }
        const th::DecodedAccessUnit& au = **decoded;
        const auto channels = static_cast<std::size_t>(au.channels);
        if (planes.size() < channels) {
            planes.resize(channels);
        }
        constexpr float kScale = 1.0F / 8388608.0F;
        for (std::size_t s = 0; s < static_cast<std::size_t>(au.samples); ++s) {
            for (std::size_t c = 0; c < channels; ++c) {
                planes[c].push_back(static_cast<float>(au.pcm[(s * channels) + c]) * kScale);
            }
        }
    }
    return planes;
}

[[nodiscard]] bool bits_equal(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

[[nodiscard]] bool bits_equal(const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!bits_equal(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::size_t total_samples(const Collected& c) {
    std::size_t n = 0;
    for (const auto& b : c.blocks) {
        n += b.samples;
    }
    return n;
}

// --- the player ----------------------------------------------------------------------------------

[[nodiscard]] bool any_sound(const fwhost::Outcome& o) {
    for (const auto& output : o.samples) {
        for (const float x : output) {
            if (x != 0.0F) {
                return true;
            }
        }
    }
    return false;
}

// Every difference between two outcomes, as text; nothing when they are the same to the last bit.
[[nodiscard]] std::string differences(const fwhost::Outcome& a, const fwhost::Outcome& b) {
    std::ostringstream out;
    if (a.samples.size() != b.samples.size()) {
        out << "outputs " << a.samples.size() << " vs " << b.samples.size() << "; ";
    } else {
        for (std::size_t o = 0; o < a.samples.size(); ++o) {
            if (a.samples[o].size() != b.samples[o].size()) {
                out << "output " << o << " length " << a.samples[o].size() << " vs " << b.samples[o].size() << "; ";
                continue;
            }
            for (std::size_t k = 0; k < a.samples[o].size(); ++k) {
                if (std::memcmp(&a.samples[o][k], &b.samples[o][k], sizeof(float)) != 0) {
                    out << "output " << o << " first differs at sample " << k << ": " << a.samples[o][k] << " vs "
                        << b.samples[o][k] << "; ";
                    break;
                }
            }
        }
    }
    if (a.play_us != b.play_us) {
        out << "block play times; ";
    }
    if (a.writes != b.writes) {
        out << "sink writes " << a.writes << " vs " << b.writes << "; ";
    }
    if (!(a.levels == b.levels)) {
        out << "published levels; ";
    }
    const auto eq = [&out](const char* name, auto x, auto y) {
        if (x != y) {
            out << name << " " << x << " vs " << y << "; ";
        }
    };
    const auto near = [&out](const char* name, auto x, auto y) {
        if (x - y > 1 || y - x > 1) {
            out << name << " " << x << " vs " << y << "; ";
        }
    };
    const auto& pa = a.status.playout;
    const auto& pb = b.status.playout;
    eq("blocks", pa.blocks, pb.blocks);
    eq("stream_frames", pa.stream_frames, pb.stream_frames);
    eq("silence_frames", pa.silence_frames, pb.silence_frames);
    eq("skipped_frames", pa.skipped_frames, pb.skipped_frames);
    eq("dropped_frames", pa.dropped_frames, pb.dropped_frames);
    eq("repeated_frames", pa.repeated_frames, pb.repeated_frames);
    eq("resyncs", pa.resyncs, pb.resyncs);
    eq("late_blocks", pa.late_blocks, pb.late_blocks);
    eq("underruns", pa.underruns, pb.underruns);
    eq("last_frame", pa.last_frame, pb.last_frame);
    eq("last_play_us", pa.last_play_us, pb.last_play_us);
    // The playout's error figures are the play time less the TARGET of the block's first frame, which
    // the player works out for the first frame of what it pushes and the playout moves on to the
    // block's first frame: a push of several units rounds that target to the microsecond from a
    // frame other than the one a unit's own push did, so it can differ by one. They are statistics of
    // the clock, not samples; every sample and the time every block plays at are equal above.
    near("error_us", pa.error_us, pb.error_us);
    near("smoothed_error_us", pa.smoothed_error_us, pb.smoothed_error_us);
    near("worst_error_us", pa.worst_error_us, pb.worst_error_us);
    near("last_target_us", pa.last_target_us, pb.last_target_us);
    const auto& ca = a.status.counters;
    const auto& cb = b.status.counters;
    eq("bursts_played", ca.bursts_played, cb.bursts_played);
    eq("late_chunks", ca.late_chunks, cb.late_chunks);
    eq("dropped_chunks", ca.dropped_chunks, cb.dropped_chunks);
    eq("invalid_chunks", ca.invalid_chunks, cb.invalid_chunks);
    eq("coded_bytes", a.status.coded_bytes, b.status.coded_bytes);
    eq("have_decoder", a.status.have_decoder, b.status.have_decoder);
    eq("decoder acmod", a.status.decoder.acmod, b.status.decoder.acmod);
    eq("decoder lfe", a.status.decoder.lfe, b.status.decoder.lfe);
    for (std::size_t o = 0; o < a.status.peak_db.size(); ++o) {
        if (std::memcmp(&a.status.peak_db[o], &b.status.peak_db[o], sizeof(float)) != 0 ||
            std::memcmp(&a.status.rms_db[o], &b.status.rms_db[o], sizeof(float)) != 0 ||
            a.status.stream_rms[o] != b.status.stream_rms[o]) {
            out << "meter of output " << o << "; ";
        }
    }
    return out.str();
}

[[nodiscard]] fwhost::Scenario scenario(const std::vector<std::uint8_t>& stream, std::uint32_t rate, const char* layout,
                                        std::size_t block_samples) {
    fwhost::Scenario sc;
    sc.stream = stream;
    sc.rate = rate;
    sc.layout = layout;
    sc.block_samples = block_samples;
    return sc;
}

// The same scenario one unit a block and as many units as the player's block holds; the batched run
// must be the unit run, and (so as not to compare silence with silence) must have made a sound.
void require_same(fwhost::Scenario sc, bool expect_sound = true) {
    const std::size_t unit = sc.rate >= 176400 ? 160 : sc.rate >= 88200 ? 80 : 40;
    sc.block_samples = unit;
    sc.decode_ahead = false;
    const fwhost::Outcome one = fwhost::run(sc);
    if (expect_sound) {
        REQUIRE(any_sound(one));
    }
    REQUIRE(one.chunks > 0);
    // Units together in the player's block, the chunk converted ahead of them or not.
    for (const bool ahead : {false, true}) {
        sc.block_samples = 256;
        sc.decode_ahead = ahead;
        const fwhost::Outcome many = fwhost::run(sc);
        const std::string diff = differences(one, many);
        INFO("layout " << sc.layout << ", rate " << sc.rate << ", converted ahead: " << ahead << ": " << diff);
        REQUIRE(diff.empty());
    }
}

}  // namespace

// --- TruehdStream ------------------------------------------------------------------------------

TEST_CASE("a TrueHD chunk of 24 units at 48 kHz is four blocks of six units", "[truehd][block-path]") {
    for (const char* name : {"truehd_st48_s24.thd", "truehd_m51_s24.thd", "truehd_m51_s16.thd"}) {
        const auto bytes = fixture(std::string("truehd/") + name);
        const StreamRun one = through_stream(bytes, 48000, 0);
        const StreamRun many = through_stream(bytes, 48000, 256);
        const StreamRun ahead = through_stream(bytes, 48000, 256, 0, true);
        INFO(name);
        // Converted a chunk ahead, the blocks handed over are the same blocks.
        REQUIRE(ahead.collected.blocks.size() == many.collected.blocks.size());
        for (std::size_t i = 0; i < many.collected.blocks.size(); ++i) {
            REQUIRE(ahead.collected.blocks[i].samples == many.collected.blocks[i].samples);
            REQUIRE(ahead.collected.blocks[i].index == many.collected.blocks[i].index);
        }
        REQUIRE(bits_equal(ahead.collected.planes, many.collected.planes));
        REQUIRE(!one.collected.blocks.empty());
        for (const auto& b : one.collected.blocks) {
            REQUIRE(b.samples == 40);
            REQUIRE(b.unit_samples == 40);
        }
        // Every full chunk is 960 samples: 24 blocks of 40 before, 4 of 240 now.
        std::size_t full = 0;
        for (const auto& b : many.collected.blocks) {
            REQUIRE(b.samples % 40 == 0);
            REQUIRE(b.samples <= 240);
            REQUIRE(b.unit_samples == 40);
            full += b.samples == 240 ? 1 : 0;
        }
        REQUIRE(many.collected.blocks.size() * 5 <= one.collected.blocks.size());
        REQUIRE(total_samples(many.collected) == total_samples(one.collected));
        REQUIRE(full > 0);
        // The first unit of each block is where the chunk's numbering says.
        int expect = 0;
        for (const auto& b : many.collected.blocks) {
            if (b.index == 0) {
                expect = 0;
            }
            REQUIRE(b.index == expect);
            expect += static_cast<int>(b.samples / 40);
        }
        // The same samples, bit for bit, as the decoder's with today's conversion, with either bound.
        const auto reference = reference_planes(bytes);
        REQUIRE(bits_equal(one.collected.planes, reference));
        REQUIRE(bits_equal(many.collected.planes, reference));
    }
}

TEST_CASE("TrueHD units of 80 and 160 samples are batched to the player's block, never past it", "[truehd][block-path]") {
    {
        const auto bytes = fixture("truehd/truehd_st96_s24.thd");
        const StreamRun one = through_stream(bytes, 96000, 0);
        const StreamRun many = through_stream(bytes, 96000, 256);
        for (const auto& b : many.collected.blocks) {
            REQUIRE(b.samples <= 240);
            REQUIRE(b.unit_samples == 80);
        }
        REQUIRE(many.collected.blocks.size() < one.collected.blocks.size());
        REQUIRE(bits_equal(many.collected.planes, one.collected.planes));
        REQUIRE(bits_equal(many.collected.planes, reference_planes(bytes)));
    }
    {
        const auto bytes = fixture("truehd/truehd_st192_s24.thd");
        const StreamRun one = through_stream(bytes, 192000, 0);
        const StreamRun many = through_stream(bytes, 192000, 256);
        for (const auto& b : many.collected.blocks) {
            REQUIRE(b.samples == 160);
        }
        REQUIRE(many.collected.blocks.size() == one.collected.blocks.size());
        REQUIRE(bits_equal(many.collected.planes, reference_planes(bytes)));
    }
}

TEST_CASE("a unit that does not decode is silence in its place in a batched block", "[truehd][block-path]") {
    auto bytes = fixture("truehd/truehd_m51_s24.thd");
    const auto units = fwhost::split_units(bytes);
    REQUIRE(units.size() > 40);
    // A byte in the middle of unit 30 (chunk 1, unit 6 of its 24): the unit does not read, and nor may
    // those after it until the stream's next major sync.
    bytes[static_cast<std::size_t>(units[30].data() - bytes.data()) + (units[30].size() / 2)] ^= 0x5A;
    const StreamRun one = through_stream(bytes, 48000, 0);
    const StreamRun many = through_stream(bytes, 48000, 256);
    bool refused_unit = false;
    for (const auto& reason : many.reasons) {
        refused_unit = refused_unit || reason == "a unit the decoder refused";
    }
    REQUIRE(refused_unit);
    REQUIRE(one.results == many.results);
    REQUIRE(one.reasons == many.reasons);
    REQUIRE(total_samples(many.collected) == units.size() * 40);
    REQUIRE(total_samples(one.collected) == units.size() * 40);
    REQUIRE(bits_equal(many.collected.planes, one.collected.planes));
    // Silence is zeros: the channels' samples over the lost units.
    bool zeros = false;
    for (std::size_t k = 30 * 40; k < 31 * 40; ++k) {
        zeros = zeros || many.collected.planes[0][k] == 0.0F;
    }
    REQUIRE(zeros);
}

TEST_CASE("a chunk that changes layout is cut into one block per layout", "[truehd][block-path]") {
    // Five channels' worth of 5.1 then stereo, end to end: the major sync of the second stream
    // arrives in the middle of a chunk.
    auto bytes = fixture("truehd/truehd_m51_s24.thd");
    const auto second = fixture("truehd/truehd_st48_s24.thd");
    bytes.insert(bytes.end(), second.begin(), second.end());
    const StreamRun one = through_stream(bytes, 48000, 0);
    const StreamRun many = through_stream(bytes, 48000, 256);
    REQUIRE(one.results == many.results);
    REQUIRE(total_samples(one.collected) == total_samples(many.collected));
    REQUIRE(bits_equal(many.collected.planes, one.collected.planes));
    bool six = false;
    bool two = false;
    for (const auto& b : many.collected.blocks) {
        six = six || b.channels == 6;
        two = two || b.channels == 2;
        // A block holds one layout: its bed has as many speakers as its channels.
        REQUIRE(b.bed.count == b.channels);
    }
    REQUIRE(six);
    REQUIRE(two);
}

TEST_CASE("a chunk that changes to other channels of the same number is cut at the change", "[truehd][block-path]") {
    // 5.0 (L R C Ls Rs) then 4.1 (L R Ls Rs and the LFE): five channels each, so the count does not
    // tell them apart.
    using Location = iclforge::ac3::eac3::chanmap::Location;
    auto bytes = fixture("truehd/truehd_c50_s16.thd");
    const auto second = fixture("truehd/truehd_c41_s16.thd");
    bytes.insert(bytes.end(), second.begin(), second.end());
    const StreamRun one = through_stream(bytes, 48000, 0);
    const StreamRun many = through_stream(bytes, 48000, 256);
    const StreamRun ahead = through_stream(bytes, 48000, 256, 0, true);
    REQUIRE(one.results == many.results);
    REQUIRE(total_samples(one.collected) == total_samples(many.collected));
    REQUIRE(bits_equal(many.collected.planes, one.collected.planes));
    REQUIRE(bits_equal(ahead.collected.planes, one.collected.planes));
    // The bed of each unit, as the blocks of one unit gave it.
    std::vector<iclforge::ac3::eac3::chanmap::Layout> bed_of_unit;
    for (const auto& b : one.collected.blocks) {
        bed_of_unit.push_back(b.bed);
    }
    bool centre = false;
    bool lfe = false;
    for (const auto& b : many.collected.blocks) {
        REQUIRE(b.channels == 5);
        centre = centre || b.bed.index_of(Location::kCentre) >= 0;
        lfe = lfe || b.bed.index_of(Location::kLfe) >= 0;
    }
    REQUIRE(centre);
    REQUIRE(lfe);
    // Every unit of a block has the bed the block names: the units of 'one' in the block's span.
    std::size_t unit = 0;
    for (const auto& b : many.collected.blocks) {
        for (std::size_t u = 0; u < b.samples / 40; ++u, ++unit) {
            REQUIRE(unit < bed_of_unit.size());
            REQUIRE(bed_of_unit[unit].count == b.bed.count);
            for (int k = 0; k < b.bed.count; ++k) {
                REQUIRE(bed_of_unit[unit][k] == b.bed[k]);
            }
        }
    }
    REQUIRE(unit == bed_of_unit.size());
}

TEST_CASE("a unit refused in the middle of a chunk finds what came before it handed over", "[truehd][block-path]") {
    // 48 kHz stereo, then a 96 kHz stream: the first unit of the second is refused as another rate.
    auto bytes = fixture("truehd/truehd_st48_s24.thd");
    const std::size_t first_units = fwhost::split_units(bytes).size();
    const auto second = fixture("truehd/truehd_st96_s24.thd");
    bytes.insert(bytes.end(), second.begin(), second.end());
    // Chunks that put the join among the units of one: 24 per chunk, so the join is unit first_units % 24.
    const StreamRun one = through_stream(bytes, 48000, 0);
    const StreamRun many = through_stream(bytes, 48000, 256);
    REQUIRE(one.results.back() == TruehdStream::Result::kRefused);
    REQUIRE(many.results == one.results);
    REQUIRE(many.reasons == one.reasons);
    REQUIRE(total_samples(many.collected) == first_units * 40);
    REQUIRE(total_samples(one.collected) == first_units * 40);
    REQUIRE(bits_equal(many.collected.planes, one.collected.planes));
}

TEST_CASE("more channels than the board decodes are refused before any plays", "[truehd][block-path]") {
    const auto bytes = fixture("truehd/truehd_m51_s24.thd");
    const StreamRun run = through_stream(bytes, 48000, 256, 2);
    REQUIRE(run.results.front() == TruehdStream::Result::kRefused);
    REQUIRE(run.reasons.front() == "more channels than this board decodes of TrueHD");
    REQUIRE(run.collected.blocks.empty());
}

TEST_CASE("the planes are one allocation, in PSRAM where the part has it", "[truehd][block-path][memory]") {
    for (const bool psram : {true, false}) {
        fwhost::set_psram_present(psram);
        const std::uint32_t where = psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
        // 6 channels at 48 kHz: a chunk is 960 samples, which a part with PSRAM converts ahead; a
        // part without converts the 240 of a block.
        const std::size_t bytes = (psram ? 960 : 240) * 6 * sizeof(float);
        const std::size_t before = fwhost::live_allocations().size();
        {
            TruehdStream stream;
            REQUIRE(stream.open({.max_channels = 6, .sample_rate_hz = 48000, .max_block_samples = 256,
                                 .max_run_samples = TruehdStream::kMaxRunSamples}));
            const auto after = fwhost::live_allocations();
            REQUIRE(after.size() == before + 1);
            std::size_t found = 0;
            for (const auto& a : after) {
                if ((a.caps & where) != 0 && a.bytes == bytes) {
                    ++found;
                }
            }
            REQUIRE(found == 1);
            // Let go with the decoder, and made again by the next open().
            stream.close();
            REQUIRE(fwhost::live_allocations().size() == before);
            REQUIRE(stream.open({.max_channels = 6, .sample_rate_hz = 48000, .max_block_samples = 256,
                                 .max_run_samples = TruehdStream::kMaxRunSamples}));
            REQUIRE(fwhost::live_allocations().size() == before + 1);
        }
        REQUIRE(fwhost::live_allocations().size() == before);
    }
    fwhost::set_psram_present(true);
}

TEST_CASE("the planes are sized from the channels, the rate and the bounds", "[truehd][block-path][memory]") {
    const auto bytes_of = [](std::size_t channels, std::uint32_t rate, std::size_t block, std::size_t run) {
        const std::size_t before = fwhost::live_allocations().size();
        TruehdStream stream;
        REQUIRE(stream.open({.max_channels = channels, .sample_rate_hz = rate, .max_block_samples = block, .max_run_samples = run}));
        const auto after = fwhost::live_allocations();
        REQUIRE(after.size() == before + 1);
        return after.back().bytes;
    };
    constexpr std::size_t kAll = TruehdStream::kMaxRunSamples;
    // One unit a block and nothing ahead: the largest unit (160) a plane.
    CHECK(bytes_of(8, 48000, 0, 0) == 8 * 160 * 4);
    // A block of 256, nothing ahead: 240 samples at 48 and 96 kHz, 160 at 192.
    CHECK(bytes_of(6, 48000, 256, 0) == 6 * 240 * 4);
    CHECK(bytes_of(6, 96000, 256, 0) == 6 * 240 * 4);
    CHECK(bytes_of(6, 192000, 256, 0) == 6 * 160 * 4);
    // A chunk ahead (24 units): 960, 1,920 and 3,840 samples, the last held to the 64 KiB budget.
    CHECK(bytes_of(6, 48000, 256, kAll) == 6 * 960 * 4);
    CHECK(bytes_of(2, 48000, 256, kAll) == 2 * 960 * 4);
    CHECK(bytes_of(6, 96000, 256, kAll) == 6 * 1920 * 4);
    CHECK(bytes_of(6, 192000, 256, kAll) == 6 * 2720 * 4);
    CHECK(bytes_of(8, 192000, 256, kAll) <= TruehdStream::kPlaneBudgetBytes);
    // A board that states no limit gets the renderer's most channels, within the budget.
    CHECK(bytes_of(0, 48000, 256, kAll) <= TruehdStream::kPlaneBudgetBytes);
    CHECK(bytes_of(1, 48000, 256, 0) == 2 * 240 * 4);
}

TEST_CASE("a part with no PSRAM converts a block at a time, however far ahead it is asked to", "[truehd][block-path][memory]") {
    fwhost::set_psram_present(false);
    const auto bytes = fixture("truehd/truehd_m51_s24.thd");
    const auto reference = reference_planes(bytes);
    const std::size_t before = fwhost::live_allocations().size();
    TruehdStream stream;
    REQUIRE(stream.open({.max_channels = 6, .sample_rate_hz = 48000, .max_block_samples = 256, .max_run_samples = TruehdStream::kMaxRunSamples}));
    const auto after = fwhost::live_allocations();
    REQUIRE(after.size() == before + 1);
    CHECK(after.back().bytes == 6 * 240 * 4);
    CHECK((after.back().caps & MALLOC_CAP_INTERNAL) != 0);
    fwhost::set_psram_present(true);
    Collected collected;
    const auto units = fwhost::split_units(bytes);
    std::vector<std::uint8_t> chunk;
    for (std::size_t first = 0; first < units.size(); first += 24) {
        chunk.clear();
        for (std::size_t u = first; u < std::min(units.size(), first + 24); ++u) {
            chunk.insert(chunk.end(), units[u].begin(), units[u].end());
        }
        REQUIRE(stream.decode(std::as_bytes(std::span<const std::uint8_t>(chunk)), &collect, &collected) == TruehdStream::Result::kDecoded);
    }
    REQUIRE(bits_equal(collected.planes, reference));
    for (const auto& b : collected.blocks) {
        REQUIRE(b.samples <= 240);
    }
}

// --- BurstPlayer -------------------------------------------------------------------------------

TEST_CASE("the player renders batched TrueHD blocks as it rendered units", "[truehd][block-path][player]") {
    struct Case {
        const char* file;
        std::uint32_t rate;
        std::size_t max_channels;
    };
    const Case cases[] = {{"truehd/truehd_st48_s24.thd", 48000, 0},        {"truehd/truehd_m51_s24.thd", 48000, 0},
                          {"truehd/truehd_m51_s16.thd", 48000, 6},         {"truehd/truehd_st96_s24.thd", 96000, 0},
                          {"truehd/truehd_st192_s24.thd", 192000, 0},      {"truehd/truehd_c50_s16.thd", 48000, 0},
                          {"truehd/truehd_c41_s16.thd", 48000, 0}};
    for (const Case& c : cases) {
        const auto stream = fixture(c.file);
        for (const char* layout : {"2.0", "5.1", "5.1.2"}) {
            SECTION(std::string(c.file) + " to " + layout) {
                fwhost::Scenario sc = scenario(stream, c.rate, layout, 256);
                sc.max_channels = c.max_channels;
                require_same(sc);
            }
        }
    }
}

TEST_CASE("the player renders a seven-channel-and-LFE TrueHD stream as it rendered units", "[truehd][block-path][player]") {
    // handbuilt/hb_eight_channels.thd: eight channels at 48 kHz.
    const auto stream = fixture("truehd/handbuilt/hb_eight_channels.thd");
    for (const char* layout : {"2.0", "5.1", "5.1.2", "7.1"}) {
        SECTION(layout) {
            // Whatever the player makes of the stream, with the units one at a time and together.
            require_same(scenario(stream, 48000, layout, 256), /*expect_sound=*/false);
        }
    }
}

TEST_CASE("the player plays through a unit that does not decode as it did", "[truehd][block-path][player]") {
    auto stream = fixture("truehd/truehd_m51_s24.thd");
    const auto units = fwhost::split_units(stream);
    stream[static_cast<std::size_t>(units[30].data() - stream.data()) + (units[30].size() / 2)] ^= 0x5A;
    for (const char* layout : {"2.0", "5.1", "5.1.2"}) {
        SECTION(layout) {
            require_same(scenario(stream, 48000, layout, 256));
        }
    }
}

TEST_CASE("the player ends a stream that changes layout, and one that is refused, as it did", "[truehd][block-path][player]") {
    {
        auto stream = fixture("truehd/truehd_m51_s24.thd");
        const auto second = fixture("truehd/truehd_st48_s24.thd");
        stream.insert(stream.end(), second.begin(), second.end());
        SECTION("5.1 then 2.0") {
            require_same(scenario(stream, 48000, "5.1", 256));
        }
    }
    {
        auto stream = fixture("truehd/truehd_c50_s16.thd");
        const auto second = fixture("truehd/truehd_c41_s16.thd");
        stream.insert(stream.end(), second.begin(), second.end());
        SECTION("5.0 then 4.1") {
            require_same(scenario(stream, 48000, "5.1", 256));
        }
    }
    {
        auto stream = fixture("truehd/truehd_st48_s24.thd");
        const auto second = fixture("truehd/truehd_st96_s24.thd");
        stream.insert(stream.end(), second.begin(), second.end());
        SECTION("48 kHz then 96 kHz") {
            require_same(scenario(stream, 48000, "2.0", 256));
        }
    }
}

TEST_CASE("the player moves the volume over the unit it did, whatever block the units come in", "[truehd][block-path][player][hazard]") {
    const auto stream = fixture("truehd/truehd_m51_s24.thd");
    for (const char* layout : {"2.0", "5.1"}) {
        SECTION(layout) {
            fwhost::Scenario sc = scenario(stream, 48000, layout, 256);
            sc.before_chunk = [](iclforge::BurstPlayer& player, std::size_t chunk) {
                if (chunk == 2) {
                    player.set_volume(50, false);
                } else if (chunk == 5) {
                    player.set_volume(100, false);
                } else if (chunk == 8) {
                    player.set_volume(30, true);
                } else if (chunk == 11) {
                    player.set_volume(80, false);
                }
            };
            require_same(sc);
        }
    }
}

TEST_CASE("the player ramps a changed trim over the unit it did, and plays delays as it did", "[truehd][block-path][player][hazard]") {
    const auto stream = fixture("truehd/truehd_m51_s24.thd");
    for (const char* layout : {"5.1", "5.1.2"}) {
        SECTION(layout) {
            fwhost::Scenario sc = scenario(stream, 48000, layout, 256);
            sc.before_chunk = [](iclforge::BurstPlayer& player, std::size_t chunk) {
                iclforge::render::speaker_trim::SpeakerTrimTable table;
                if (chunk == 0) {
                    // Levels and distances from the first block: delays on the nearer speakers.
                    REQUIRE(table.apply_text("L level=-3; R level=-1.5 distance=2.4; C distance=2.8; Ls distance=1.9; Rs distance=2.0").ok);
                    player.set_speaker_trim(table);
                } else if (chunk == 6) {
                    // A different table in the middle: levels ramp, delays are made again.
                    REQUIRE(table.apply_text("L level=-6; R level=-6 distance=2.4; C level=2 distance=2.8; Ls distance=2.5").ok);
                    player.set_speaker_trim(table);
                } else if (chunk == 11) {
                    REQUIRE(table.apply_text("reset").ok);
                    player.set_speaker_trim(table);
                }
            };
            require_same(sc);
        }
    }
}

TEST_CASE("the player plays a server's identify tone over a TrueHD stream as it did", "[truehd][block-path][player][hazard]") {
    const auto stream = fixture("truehd/truehd_m51_s24.thd");
    fwhost::Scenario sc = scenario(stream, 48000, "5.1", 256);
    sc.before_chunk = [](iclforge::BurstPlayer& player, std::size_t chunk) {
        if (chunk == 4) {
            player.identify(iclforge::sendspin::player::Identify{.output = 1, .level_db = -30.0});
        } else if (chunk == 9) {
            player.identify(std::nullopt);
        }
    };
    require_same(sc);
}

TEST_CASE("the player plays volume, trims and delays together as it did", "[truehd][block-path][player][hazard]") {
    const auto stream = fixture("truehd/truehd_m51_s16.thd");
    fwhost::Scenario sc = scenario(stream, 48000, "5.1.2", 256);
    sc.before_chunk = [](iclforge::BurstPlayer& player, std::size_t chunk) {
        iclforge::render::speaker_trim::SpeakerTrimTable table;
        if (chunk == 1) {
            REQUIRE(table.apply_text("L level=-1.5 distance=2.2; Ls distance=1.6").ok);
            player.set_speaker_trim(table);
        } else if (chunk == 4) {
            player.set_volume(40, false);
        } else if (chunk == 7) {
            REQUIRE(table.apply_text("L level=-4 distance=2.2; Ls level=1 distance=1.8").ok);
            player.set_speaker_trim(table);
            player.set_volume(90, false);
        }
    };
    require_same(sc);
}
