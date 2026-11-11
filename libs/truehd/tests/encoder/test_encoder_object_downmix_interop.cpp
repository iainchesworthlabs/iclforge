#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <numbers>
#include <string>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

// Hidden lab cases for the outside-decoder question of EncoderConfig::object_downmix (they write
// and read files named by the environment and assert little):
//   [downmix-interop-write]  ICL_DOWNMIX_OUT=<dir>: streams of both scalings, the objects, the
//                            downmix worked out in doubles and this library's decodes, as raw files
//   [downmix-interop-dump]   ICL_DOWNMIX_DUMP=<file;file;...>: the matrices each substream of a
//                            stream sends at its restart headers

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

constexpr int kBed = 8;
constexpr int kObjects = 12;
using Gains = std::vector<std::vector<double>>;

Gains fixture_rows() {
    Gains rows(kBed, std::vector<double>(kObjects, 0.0));
    rows[3][0] = 1.0;
    rows[0][1] = 0.75; rows[0][8] = 0.5;  rows[0][9] = 0.3;
    rows[1][2] = 0.75; rows[1][9] = 0.5;  rows[1][10] = 0.3;
    rows[2][3] = 0.75; rows[2][10] = 0.5; rows[2][11] = 0.3;
    rows[4][4] = 0.84195;
    rows[5][5] = 0.84195;
    rows[6][6] = 0.84195;
    rows[7][7] = 0.84195;
    return rows;
}

void write_raw(const std::string& path, const void* data, std::size_t bytes) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
}

std::vector<std::int32_t> decode(const std::vector<EncodedAccessUnit>& units, Presentation p) {
    DecoderConfig config;
    config.presentation = p;
    config.strictness = Strictness::kStrict;
    Decoder decoder(config);
    std::vector<std::int32_t> pcm;
    for (const EncodedAccessUnit& unit : units) {
        auto decoded = decoder.decode(unit.bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        pcm.insert(pcm.end(), (*decoded)->pcm.begin(), (*decoded)->pcm.end());
    }
    CHECK(decoder.counters().lossless_check_failures == 0);
    return pcm;
}

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

}  // namespace

TEST_CASE("lab: write object_downmix streams of both scalings for an outside decoder",
          "[.][downmix-interop-write]") {
    const std::string dir = env("ICL_DOWNMIX_OUT");
    if (dir.empty()) {
        SKIP("ICL_DOWNMIX_OUT is not set");
    }
    struct Case {
        std::string name;
        Gains rows;
        ObjectDownmixScaling scaling;
    };
    std::vector<Case> cases;
    cases.push_back({"fixture-chain", fixture_rows(), ObjectDownmixScaling::kChain});
    cases.push_back({"fixture-scaled", fixture_rows(), ObjectDownmixScaling::kBedSubstreams});
    {
        Gains rows = fixture_rows();  // the zero-row matrix the chain already relies on
        rows[2].assign(kObjects, 0.0);
        rows[6].assign(kObjects, 0.0);
        cases.push_back({"zerorows-chain", rows, ObjectDownmixScaling::kChain});
        cases.push_back({"zerorows-scaled", rows, ObjectDownmixScaling::kBedSubstreams});
    }
    {
        // rows that need the cross terms: a lone object between L and C, one between Ls and Lrs,
        // and a quiet R (-20 dB), so that a decoder that ignores the matrices is far off
        Gains rows = fixture_rows();
        rows[0].assign(kObjects, 0.0);
        rows[2].assign(kObjects, 0.0);
        rows[0][1] = std::cos(0.4);
        rows[2][1] = std::sin(0.4);
        rows[4].assign(kObjects, 0.0);
        rows[6].assign(kObjects, 0.0);
        rows[4][4] = std::cos(1.0);
        rows[6][4] = std::sin(1.0);
        rows[1].assign(kObjects, 0.0);
        rows[1][2] = 0.1;
        cases.push_back({"cross-scaled", rows, ObjectDownmixScaling::kBedSubstreams});
    }
    const int frames = 40 * 8 * 30;  // 0.2 s
    std::vector<std::int32_t> pcm = make_pcm(kObjects, frames, 24, 21);
    for (std::int32_t& v : pcm) {
        v >>= 3;
    }
    write_raw(dir + "/objects.s32", pcm.data(), pcm.size() * 4);
    for (const Case& c : cases) {
        EncoderConfig config;
        config.channels = kBed;
        config.object_channels = kObjects;
        config.object_layout = ObjectLayout::kBedMatrices;
        config.object_downmix_scaling = c.scaling;
        for (const auto& row : c.rows) {
            config.object_downmix.insert(config.object_downmix.end(), row.begin(), row.end());
        }
        config.restart_interval_units = 8;
        config.object_meaning.dyn_object_only = true;
        config.object_meaning.lfe_present = true;
        config.verify = true;
        const Encoded encoded = encode_all(config, pcm);
        std::vector<std::byte> stream;
        for (const EncodedAccessUnit& unit : encoded.units) {
            stream.insert(stream.end(), unit.bytes.begin(), unit.bytes.end());
        }
        write_raw(dir + "/" + c.name + ".thd", stream.data(), stream.size());
        // gains times objects, in doubles: what the bed should be, whoever decodes it
        std::vector<double> want(static_cast<std::size_t>(frames) * kBed, 0.0);
        for (std::size_t t = 0; t < static_cast<std::size_t>(frames); ++t) {
            for (std::size_t r = 0; r < kBed; ++r) {
                for (std::size_t e = 0; e < kObjects; ++e) {
                    want[t * kBed + r] += c.rows[r][e] * pcm[t * kObjects + e];
                }
            }
        }
        write_raw(dir + "/" + c.name + ".want.f64", want.data(), want.size() * 8);
        const auto eight = decode(encoded.units, Presentation::k8ch);
        const auto six = decode(encoded.units, Presentation::k6ch);
        const auto two = decode(encoded.units, Presentation::k2ch);
        write_raw(dir + "/" + c.name + ".ours8.s32", eight.data(), eight.size() * 4);
        write_raw(dir + "/" + c.name + ".ours6.s32", six.data(), six.size() * 4);
        write_raw(dir + "/" + c.name + ".ours2.s32", two.data(), two.size() * 4);
        std::printf("[interop] wrote %s: %zu units, %zu bytes\n", c.name.c_str(), encoded.units.size(),
                    stream.size());
    }
}

TEST_CASE("lab: the matrices each substream of a stream sends at its restarts",
          "[.][downmix-interop-dump]") {
    std::string list = env("ICL_DOWNMIX_DUMP");
    if (list.empty()) {
        SKIP("ICL_DOWNMIX_DUMP is not set");
    }
    list += ';';
    for (std::size_t at = 0; at < list.size();) {
        const std::size_t end = list.find(';', at);
        const std::string path = list.substr(at, end - at);
        at = end + 1;
        if (path.empty()) {
            continue;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            std::printf("[dump] %s: cannot open\n", path.c_str());
            continue;
        }
        const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
        const std::span<const std::byte> data(reinterpret_cast<const std::byte*>(raw.data()),
                                              raw.size());
        int substreams = 0;
        std::size_t units = 0;
        std::size_t restarts = 0;
        std::size_t failures = 0;
        // per substream: each distinct set of matrices, as text, and how many restarts sent it
        std::array<std::map<std::string, int>, 4> seen;
        std::array<int, 4> without{};
        for (std::size_t offset = 0; offset + 4 <= data.size();) {
            const std::size_t length = access_unit_length_bytes(data.subspan(offset));
            if (length < 4 || offset + length > data.size()) {
                break;
            }
            const auto unit = parse_access_unit(data.subspan(offset, length), substreams, false);
            offset += length;
            if (!unit) {
                ++failures;
                continue;
            }
            ++units;
            substreams = unit->num_substreams;
            for (int s = 0; s < unit->num_substreams && s < 4; ++s) {
                const auto segment = unit->segments[static_cast<std::size_t>(s)];
                if (segment.size() < 4) {
                    continue;
                }
                iclforge::BitReader reader(segment);
                if (reader.read(2) != 3) {
                    continue;  // the unit's first block has no restart header
                }
                const auto restart = read_restart_header(reader, segment, false);
                if (!restart) {
                    ++failures;
                    continue;
                }
                BlockContext context;
                context.min_chan = restart->min_chan;
                context.max_chan = restart->max_chan;
                context.max_matrix_chan = restart->max_matrix_chan;
                context.object_matrices = restart->sync == kRestartSyncAtmos;
                context.noise_type = !context.object_matrices && (restart->sync & 1U) != 0;
                const auto header = read_block_header(reader, context);
                if (!header) {
                    ++failures;
                    continue;
                }
                ++restarts;
                if (!header->matrices || header->matrices->count == 0) {
                    ++without[static_cast<std::size_t>(s)];
                    continue;
                }
                std::string text;
                for (int m = 0; m < header->matrices->count; ++m) {
                    const MatrixSpec& matrix = header->matrices->items[static_cast<std::size_t>(m)];
                    char line[64];
                    std::snprintf(line, sizeof line, "    out %d frac %d:", matrix.out_ch,
                                  matrix.frac_bits);
                    text += line;
                    for (std::size_t c = 0; c < matrix.present.size(); ++c) {
                        if (!matrix.present[c]) {
                            continue;
                        }
                        const double value = std::ldexp(static_cast<double>(matrix.coeff[c]),
                                                        matrix.f_shift - matrix.frac_bits);
                        std::snprintf(line, sizeof line, " %sch%zu=%+.5f",
                                      c == matrix.out_ch ? "OWN " : "", c, value);
                        text += line;
                    }
                    if (!matrix.present[matrix.out_ch]) {
                        text += " (no own coefficient)";
                    }
                    text += "\n";
                }
                ++seen[static_cast<std::size_t>(s)][text];
            }
        }
        std::printf("[dump] %s: %zu units, %d substreams, %zu restarts read, %zu unreadable\n",
                    path.c_str(), units, substreams, restarts, failures);
        for (std::size_t s = 0; s < 3; ++s) {  // the lower substreams are the question
            std::printf("[dump]  substream %zu: %d restarts with no matrices, %zu distinct sets\n", s,
                        without[s], seen[s].size());
            int shown = 0;
            for (const auto& [text, count] : seen[s]) {
                if (shown++ == 4) {
                    std::printf("[dump]   ...\n");
                    break;
                }
                std::printf("[dump]   sent at %d restarts:\n%s", count, text.c_str());
            }
        }
        std::printf("[dump]  substream 3: %d restarts with no matrices, %zu distinct sets\n",
                    without[3], seen[3].size());
    }
}
