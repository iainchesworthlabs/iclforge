// The speech spectral frontend (ETSI TS 103 190-1 V1.4.1 clauses 4.2.9, 4.3.7 and 5.2), held to
// tools/references/ssf_ref.py, a second transcription of the same text written without reading
// this decoder's.
//
// No stream here uses the tool, so the vectors are random bytes: the arithmetic decoder turns any
// bits into symbols, which gives envelopes, predictor gains and coefficients with the model's own
// statistics, and the two transcriptions must agree on the size of ssf_data() (where the
// arithmetic coded data ends, which the next granule starts from), on every stride, band count and
// line, and on which streams are invalid. tests/golden/ac4/ssf/ssf-vectors.txt is its output,
// `python tools/references/ssf_ref.py vectors --seed 1 --cases 32 --frames 4`.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/toc_writer.hpp"
#include "decoder/bits.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"
#include "decoder/syntax/context.hpp"
#include "decoder/syntax/ssf.hpp"

namespace {

using iclforge::BitReader;
using iclforge::ac4::detail::SsfData;
using iclforge::ac4::detail::SsfState;
using iclforge::ac4::detail::SubstreamContext;

std::vector<std::byte> from_hex(const std::string& text) {
    std::vector<std::byte> bytes;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        bytes.push_back(static_cast<std::byte>(std::stoi(text.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

struct VectorBlock {
    std::vector<double> lines;  // the num_bins coded lines
};

struct VectorGranule {
    int stride = 0;
    int num_bands = 0;
    int n_mdct = 0;
    int num_bins = 0;
    std::vector<VectorBlock> blocks;
};

struct VectorFrame {
    bool b_iframe = false;
    std::vector<std::byte> bytes;
    bool ok = false;
    std::size_t bits = 0;
    std::vector<VectorGranule> granules;
};

struct VectorCase {
    std::string name;
    int frame_len_base = 0;
    std::vector<VectorFrame> frames;
};

std::vector<VectorCase> read_vectors() {
    std::ifstream in(std::filesystem::path{AC4_GOLDEN_DIR} / "ssf" / "ssf-vectors.txt");
    REQUIRE(in.good());
    std::vector<VectorCase> cases;
    std::string line;
    REQUIRE(std::getline(in, line));
    REQUIRE(line == "ssfvec 1");
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string word;
        words >> word;
        if (word == "case") {
            VectorCase& c = cases.emplace_back();
            std::string key;
            words >> c.name >> key >> c.frame_len_base;
        } else if (word == "frame") {
            VectorFrame& f = cases.back().frames.emplace_back();
            std::string index, key1, key2, hex;
            int b_iframe = 0;
            words >> index >> key1 >> b_iframe >> key2 >> hex;
            f.b_iframe = b_iframe != 0;
            f.bytes = from_hex(hex);
        } else if (word == "ok") {
            VectorFrame& f = cases.back().frames.back();
            std::string key;
            std::size_t granules = 0;
            f.ok = true;
            words >> key >> f.bits >> key >> granules;
        } else if (word == "granule") {
            VectorGranule& g = cases.back().frames.back().granules.emplace_back();
            std::string key;
            int index = 0;
            words >> index >> key >> g.stride >> key >> g.num_bands >> key >> g.n_mdct >> key >>
                g.num_bins;
        } else if (word == "block") {
            VectorBlock& b = cases.back().frames.back().granules.back().blocks.emplace_back();
            std::string index, value;
            words >> index;
            while (words >> value) {
                b.lines.push_back(std::strtod(value.c_str(), nullptr));
            }
        }
    }
    return cases;
}

}  // namespace

TEST_CASE("ssf_data decodes random streams as the reference transcription does",
          "[ac4][decoder][ssf]") {
    const std::vector<VectorCase> cases = read_vectors();
    REQUIRE_FALSE(cases.empty());
    std::size_t frames_ok = 0;
    std::size_t frames_refused = 0;
    std::size_t short_granules = 0;
    for (const VectorCase& c : cases) {
        SsfState state;
        for (std::size_t index = 0; index < c.frames.size(); ++index) {
            const VectorFrame& want = c.frames[index];
            CAPTURE(c.name, index);
            SubstreamContext ctx;
            ctx.frame_len_base = c.frame_len_base;
            ctx.b_iframe = want.b_iframe;
            BitReader reader(want.bytes, 0, {});
            SsfData got;
            const auto result = iclforge::ac4::detail::parse_ssf_data(reader, ctx, state, got);
            if (!want.ok) {
                CHECK_FALSE(result.has_value());
                ++frames_refused;
                break;  // the reference stops a case at its first error
            }
            REQUIRE(result.has_value());
            ++frames_ok;
            CHECK(reader.bit_position() == want.bits);
            REQUIRE(got.granule_count == static_cast<int>(want.granules.size()));
            std::size_t offset = 0;
            for (std::size_t g = 0; g < want.granules.size(); ++g) {
                CAPTURE(g);
                const VectorGranule& wg = want.granules[g];
                const auto& gg = got.granules[g];
                CHECK(gg.stride_flag == wg.stride);
                CHECK(gg.num_bands == wg.num_bands);
                CHECK(gg.n_mdct == wg.n_mdct);
                CHECK(gg.num_bins == wg.num_bins);
                short_granules += wg.stride != 0 ? 1U : 0U;
                REQUIRE(gg.num_blocks == static_cast<int>(wg.blocks.size()));
                for (std::size_t b = 0; b < wg.blocks.size(); ++b) {
                    CAPTURE(b);
                    const std::vector<double>& lines = wg.blocks[b].lines;
                    REQUIRE(lines.size() == static_cast<std::size_t>(wg.num_bins));
                    for (std::size_t k = 0; k < static_cast<std::size_t>(wg.n_mdct); ++k) {
                        const double want_line = k < lines.size() ? lines[k] : 0.0;
                        const double got_line = got.lines[offset + k];
                        const double tolerance = 1e-9 * std::abs(want_line) + 1e-12;
                        if (std::abs(got_line - want_line) > tolerance) {
                            CHECK(got_line == want_line);  // reports the line
                            k = static_cast<std::size_t>(wg.n_mdct);
                        }
                    }
                    offset += static_cast<std::size_t>(wg.n_mdct);
                }
            }
        }
    }
    // The vectors reach what they are for: valid and invalid frames, short strides.
    CHECK(frames_ok > 50);
    CHECK(frames_refused > 0);
    CHECK(short_granules > 20);
}

TEST_CASE("the dB and linear maps of Pseudocodes 29 and 30 track the functions they approximate",
          "[ac4][decoder][ssf]") {
    // Map_dB_to_Lin takes Q.10 dB and gives Q.10 10^(dB / 20) by a table of chords; its slopes are
    // coarse integers, so it is held to 10 % rather than to a float's accuracy.
    for (int db = 0; db < 40 * 1024; db += 97) {
        std::int32_t lin = 0;
        REQUIRE(iclforge::ac4::detail::ssf_map_db_to_lin(db, lin));
        const double want = std::pow(10.0, db / 1024.0 / 20.0);
        CAPTURE(db);
        CHECK(std::abs(lin / 1024.0 - want) <= 0.1 * want + 1.0 / 512.0);
    }
    // Map_Lin_to_dB, the inverse, within 1.5 dB over 1 to 100.
    for (int lin = 1024; lin < 100 * 1024; lin += 331) {
        std::int32_t db = 0;
        REQUIRE(iclforge::ac4::detail::ssf_map_lin_to_db(lin, db));
        const double want = 20.0 * std::log10(lin / 1024.0);
        CAPTURE(lin);
        CHECK(std::abs(db / 1024.0 - want) <= 1.5);
    }
}

namespace {

using ac4_decoder_test::BitWriter;

// Part 1 Table 83's frame_rate_index for a frame length at 48 kHz.
int frame_rate_index_of(int frame_len_base) {
    switch (frame_len_base) {
        case 1920:
            return 0;
        case 1536:
            return 3;
        case 960:
            return 5;
        case 1024:
            return 7;
        case 768:
            return 8;
        case 512:
            return 10;
        case 384:
            return 11;
        default:
            return 13;  // 2048
    }
}

// A mono SIMPLE audio substream whose one track is the speech spectral frontend's: audio_size,
// mono_codec_mode, spec_frontend 1, ssf_data() from the start of `ssf`, the rest of `ssf` as
// fill_bits (the arithmetic decoder reads ahead of what it consumes, and on random bits that
// matters: it must read what the reference read), and metadata() with nothing optional
// (sus_ver 1).
std::vector<std::byte> ssf_substream(const std::vector<std::byte>& ssf) {
    const std::size_t bits = 8 * ssf.size();
    BitWriter data;
    data.put(0, 1);  // mono_codec_mode: SIMPLE
    data.put(1, 1);  // spec_frontend: SSF
    for (std::size_t i = 0; i < bits; ++i) {
        data.flag(((std::to_integer<unsigned>(ssf[i / 8]) >> (7U - i % 8U)) & 1U) != 0);
    }
    const std::size_t audio_bytes = (data.size() + 7) / 8;
    BitWriter w;
    w.put(audio_bytes, 15);  // audio_size_value
    w.flag(false);           // b_more_bits
    const std::size_t start = w.size();
    const std::vector<std::byte> bytes = data.bytes();
    for (std::size_t i = 0; i < data.size(); ++i) {
        w.flag(((std::to_integer<unsigned>(bytes[i / 8]) >> (7U - i % 8U)) & 1U) != 0);
    }
    while (w.size() < start + 8 * audio_bytes) {
        w.flag(false);  // fill_bits
    }
    w.flag(false);  // b_more_basic_metadata
    w.flag(false);  // b_dialog
    w.flag(false);  // b_channels_classifier
    w.flag(false);  // b_event_probability
    w.put(1, 7);    // tools_metadata_size_value
    w.flag(false);  // b_more_bits
    w.flag(false);  // b_de_data_present
    w.flag(false);  // b_emdf_payloads_substream
    w.align();
    return w.bytes();
}

std::vector<std::byte> presentation_substream() {
    BitWriter w;
    w.flag(false);  // b_additional_data
    w.put(20, 7);   // dialnorm_bits
    w.flag(false);  // b_further_loudness_info
    w.put(1, 5);    // drc_metadata_size_value
    w.flag(false);  // b_more_bits
    w.flag(false);  // b_drc_present
    w.flag(false);  // b_associated
    w.align();
    return w.bytes();
}

}  // namespace

TEST_CASE("a speech spectral frontend track decodes to PCM through the public API",
          "[ac4][decoder][ssf]") {
    const std::vector<VectorCase> cases = read_vectors();
    std::size_t decoded_frames = 0;
    for (const VectorCase& c : cases) {
        // Only streams whose arithmetic coded data is well formed (the "ok" cases): the bits
        // after ssf_data() are not the vector's here, and a malformed stream's decoding depends
        // on them, where a well formed one's does not (Pseudocode 47).
        if (!c.name.starts_with("ok_")) {
            continue;
        }
        iclforge::ac4::Decoder decoder;
        double case_energy = 0.0;
        for (std::size_t index = 0; index < c.frames.size() && c.frames[index].ok; ++index) {
            const VectorFrame& frame = c.frames[index];
            CAPTURE(c.name, index);
            const std::vector<std::byte> audio = ssf_substream(frame.bytes);
            ac4_toc_test::BitWriter toc;
            ac4_toc_test::toc_start(toc, {.sequence_counter = static_cast<int>(index) + 1,
                                          .frame_rate_index = frame_rate_index_of(c.frame_len_base),
                                          .b_iframe_global = frame.b_iframe});
            ac4_toc_test::PresV1 presentation;
            // frame_rate_multiply_info()'s b_multiplier where the index has one (0 to 4, 7 to 9),
            // then frame_rate_fractions_info()'s b_frame_rate_fraction (5 to 12).
            const int rate_index = frame_rate_index_of(c.frame_len_base);
            if (rate_index <= 4 || (rate_index >= 7 && rate_index <= 9)) {
                presentation.frame_rate_bits.push_back(false);
            }
            if (rate_index >= 5 && rate_index <= 12) {
                presentation.frame_rate_bits.push_back(false);
            }
            ac4_toc_test::presentation_v1(toc, presentation);
            ac4_toc_test::ChanInfo info;
            info.ch_mode = 0;
            info.b_audio_ndot = {frame.b_iframe};
            ac4_toc_test::chan_group(toc, {info});
            const std::vector<std::vector<std::byte>> substreams = {audio,
                                                                    presentation_substream()};
            ac4_toc_test::index_table(toc, ac4_toc_test::sizes_of(substreams));
            toc.align();
            const std::vector<std::byte> raw = ac4_toc_test::assemble(toc, substreams);
            const auto decoded = decoder.decode(raw);
            INFO(decoder.refusal_reason());
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->has_value());
            const iclforge::ac4::DecodedFrame& pcm = **decoded;
            REQUIRE(pcm.channels.size() == 1);
            CHECK_FALSE(pcm.channels[0].empty());
            double energy = 0.0;
            for (const float v : pcm.channels[0]) {
                REQUIRE(std::isfinite(v));
                energy += static_cast<double>(v) * static_cast<double>(v);
            }
            case_energy += energy;
            ++decoded_frames;
        }
        // The first frames come out of the decoder's delay; the case as a whole has a signal.
        if (c.frames.size() >= 3 && c.frames[2].ok) {
            CHECK(case_energy > 0.0);
        }
    }
    CHECK(decoded_frames > 30);
}
