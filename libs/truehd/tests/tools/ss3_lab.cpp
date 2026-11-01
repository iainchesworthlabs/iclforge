// iclforge-truehd-ss3-lab: runs the decoder over a TrueHD Atmos stream under each setting of
// DecoderLab, the switches for the points of the object substream (restart sync 0x31EC) that no
// stream has settled (libs/truehd/ERRATA.md entry 69), and prints one line a run: how many units
// parsed, the first error, and the lossless checks of the object substream and of the others. The
// setting whose object substream passes every check says what the point is.
//
//   iclforge-truehd-ss3-lab STREAM
//
// Prints counts and error texts only: no sample leaves the decoder.

#include <algorithm>
#include <cmath>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"

namespace {

using namespace iclforge::truehd;

struct Setting {
    const char* name;
    DecoderLab lab;
};

void print_stats(const DecoderCounters& c, int channels) {
    std::printf("    object substream, first intervals (%llu samples), by matrix channel:\n"
                "    ch  coded min..max (changes)        out min..max (changes)\n",
                static_cast<unsigned long long>(c.object_stats.samples));
    for (int ch = 0; ch < channels; ++ch) {
        const auto i = static_cast<std::size_t>(ch);
        std::printf("    %2d  %11d..%-11d (%5u)   %11d..%-11d (%5u)\n", ch,
                    c.object_stats.coded_min[i], c.object_stats.coded_max[i],
                    c.object_stats.coded_changes[i], c.object_stats.out_min[i],
                    c.object_stats.out_max[i], c.object_stats.out_changes[i]);
    }
}

// Which sets of per-channel folds XOR to the stored check byte: every subset of the 24 columns
// (the matrix channels after the matrices, then before them) is tried, and the five that match most
// of the kept intervals are printed. The fold is linear, so a check that covers some channels is
// the XOR of those channels' columns.
void print_subsets(const DecoderCounters& c) {
    constexpr std::size_t kColumns = DecoderCounters::kObjectColumns;
    const std::size_t n = c.object_checks_kept;
    if (n == 0) {
        return;
    }
    struct Best {
        std::size_t matches = 0;
        std::uint32_t subset = 0;
    };
    std::array<Best, 5> best{};
    std::array<std::array<std::uint8_t, DecoderCounters::kObjectChecksKept>, kColumns + 1> sums{};
    for (std::size_t i = 0; i < n; ++i) {
        sums[0][i] = 0;
    }
    const auto visit = [&](auto&& self, std::size_t column, std::uint32_t subset) -> void {
        if (column == kColumns) {
            std::size_t matches = 0;
            for (std::size_t i = 0; i < n; ++i) {
                matches += sums[column][i] == c.object_check_stored[i] ? 1U : 0U;
            }
            if (matches > best.back().matches) {
                best.back() = {matches, subset};
                std::sort(best.begin(), best.end(),
                          [](const Best& a, const Best& b) { return a.matches > b.matches; });
            }
            return;
        }
        sums[column + 1] = sums[column];
        self(self, column + 1, subset);
        for (std::size_t i = 0; i < n; ++i) {
            sums[column + 1][i] = static_cast<std::uint8_t>(sums[column][i] ^
                                                           c.object_column_bytes[i][column]);
        }
        self(self, column + 1, subset | (1U << column));
    };
    visit(visit, 0, 0);
    std::printf("    subsets of per-channel folds that XOR to the stored byte (of %zu intervals):\n", n);
    for (const Best& b : best) {
        std::printf("      %zu matched: out", b.matches);
        for (std::size_t col = 0; col < 12; ++col) {
            if ((b.subset >> col) & 1U) {
                std::printf(" %zu", col);
            }
        }
        std::printf("  before");
        for (std::size_t col = 12; col < kColumns; ++col) {
            if ((b.subset >> col) & 1U) {
                std::printf(" %zu", col - 12);
            }
        }
        std::printf("\n");
    }
}

// For a stream whose objects are constant (a synthetic control): the values each matrix channel's
// output takes in the first intervals, and where it leaves its most common value. A channel that
// is not constant in such a stream shows its errors here without the lossless check.
void print_interval_values(const DecoderCounters& c) {
    std::printf("    output values by interval (distinct values, smallest..largest, by matrix channel; "
                "o passed, F failed):\n");
    std::size_t begin = 0;
    for (std::size_t i = 0; i < c.object_interval_facts.size() && i < 10; ++i) {
        // the check at the end of interval k is for the samples before the restart that holds it
        const std::size_t end = c.object_interval_facts[i].samples_so_far;
        if (end <= begin) {
            continue;
        }
        std::printf("      interval %zu %c (%zu samples):", i,
                    (c.object_interval_log[i] & 1U) != 0 ? 'o' : 'F', end - begin);
        for (std::size_t ch = 0; ch < 12; ++ch) {
            std::vector<std::int32_t> values;
            for (std::size_t s = begin; s < end; ++s) {
                values.push_back(c.object_first_out[s * 12 + ch]);
            }
            std::sort(values.begin(), values.end());
            const auto distinct = std::unique(values.begin(), values.end()) - values.begin();
            std::printf(" %zu:%td[%d..%d]", ch, distinct, values.front(), values.back());
        }
        std::printf("\n");
        begin = end;
    }
}

void print_constant_deviations(const DecoderCounters& c) {
    const std::size_t samples = c.object_first_out.size() / 12;
    if (samples == 0) {
        return;
    }
    std::printf("    output values of the first %zu samples by matrix channel (value x count; the "
                "first samples that are not the most common value, as index:value):\n",
                samples);
    for (std::size_t ch = 0; ch < 12; ++ch) {
        std::vector<std::pair<std::int32_t, std::size_t>> counts;
        for (std::size_t i = 0; i < samples; ++i) {
            const std::int32_t v = c.object_first_out[i * 12 + ch];
            auto it = std::find_if(counts.begin(), counts.end(),
                                   [v](const auto& e) { return e.first == v; });
            if (it == counts.end()) {
                counts.emplace_back(v, 1);
            } else {
                ++it->second;
            }
        }
        std::sort(counts.begin(), counts.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf("      ch %2zu:", ch);
        for (std::size_t k = 0; k < counts.size() && k < 5; ++k) {
            std::printf(" %d x%zu", counts[k].first, counts[k].second);
        }
        if (counts.size() > 5) {
            std::printf(" (%zu distinct)", counts.size());
        }
        if (counts.size() > 1 && counts.front().second * 10 >= samples * 9) {
            std::printf("  off:");
            std::size_t shown = 0;
            for (std::size_t i = 0; i < samples && shown < 16; ++i) {
                const std::int32_t v = c.object_first_out[i * 12 + ch];
                if (v != counts.front().first) {
                    std::printf(" %zu:%d", i, v);
                    ++shown;
                }
            }
        }
        std::printf("\n");
    }
}

void run(const std::vector<std::byte>& stream, const char* name, const DecoderLab& lab,
         bool stats = false, bool brief = false) {
    DecoderConfig config;
    config.presentation = Presentation::kAll;
    config.strictness = Strictness::kLenient;
    config.lab = lab;
    Decoder decoder(config);
    ElementaryReader reader(stream);
    std::uint64_t units = 0;
    std::uint64_t errors = 0;
    std::string first_error;
    while (const auto unit = reader.next()) {
        ++units;
        const auto decoded = decoder.decode(*unit);
        if (!decoded) {
            if (errors == 0) {
                first_error = "unit " + std::to_string(units - 1) + ": " + std::string(decoded.error().detail);
            }
            ++errors;
        }
    }
    const DecoderCounters& c = decoder.counters();
    std::printf("%-34s units %llu decoded %llu errors %llu  ss3 ok %llu failed %llu  "
                "ss0-2 ok %llu failed %llu",
                name, static_cast<unsigned long long>(units),
                static_cast<unsigned long long>(c.units_decoded),
                static_cast<unsigned long long>(errors),
                static_cast<unsigned long long>(c.lossless_ok_by_substream[3]),
                static_cast<unsigned long long>(c.lossless_failed_by_substream[3]),
                static_cast<unsigned long long>(c.lossless_ok_by_substream[0] +
                                                c.lossless_ok_by_substream[1] +
                                                c.lossless_ok_by_substream[2]),
                static_cast<unsigned long long>(c.lossless_failed_by_substream[0] +
                                                c.lossless_failed_by_substream[1] +
                                                c.lossless_failed_by_substream[2]));
    if (!first_error.empty()) {
        std::printf("  first: %s", first_error.c_str());
    }
    std::printf("\n");
    if (!c.object_error_context.empty()) {
        std::printf("    %s\n", c.object_error_context.c_str());
    }
    if (brief) {
        return;
    }
    // The first checks of the object substream: the stored byte, the folded byte, and their XOR.
    std::printf("    stored/folded/xor:");
    for (std::size_t i = 0; i < c.object_checks_kept; ++i) {
        std::printf(" %02x/%02x/%02x", c.object_check_stored[i], c.object_check_folded[i],
                    static_cast<unsigned>(c.object_check_stored[i] ^ c.object_check_folded[i]));
    }
    std::printf("\n");
    if (stats) {
        print_stats(c, 12);
        std::printf("    first object matrices (out frac f_shift 2b m4 mask-by-channel-0-first):\n");
        for (const auto& m : c.object_first_matrices) {
            std::printf("      out %2u frac %2u f_shift %2d 2b %u m4 %2u mask ", m.out, m.frac_bits,
                        m.f_shift, m.field_2b, m.field_m4);
            for (int ch = 0; ch < 12; ++ch) {
                std::printf("%c", ((m.mask >> ch) & 1U) != 0 ? '1' : '.');
            }
            std::printf("\n        raw coefficients:");
            for (int ch = 0; ch < 12; ++ch) {
                if (((m.mask >> ch) & 1U) != 0) {
                    std::printf(" %d:%d", ch, m.coeff[static_cast<std::size_t>(ch)]);
                }
            }
            std::printf("\n");
        }
        std::printf("    object substream, each interval's check (o passed, F failed) and the "
                    "matrix sections it carried (0-9, + for more):\n      ");
        for (const std::uint8_t entry : c.object_interval_log) {
            std::printf("%c", (entry & 1U) != 0 ? 'o' : 'F');
        }
        std::printf("\n      ");
        for (const std::uint8_t entry : c.object_interval_log) {
            const unsigned updates = entry >> 1U;
            std::printf("%c", updates > 9 ? '+' : static_cast<char>('0' + updates));
        }
        std::printf("\n");
        print_interval_values(c);
        print_constant_deviations(c);
        std::printf("    quantiser steps by matrix channel at the first check:");
        for (int ch = 0; ch < 12; ++ch) {
            std::printf(" %u", c.object_quant_step[static_cast<std::size_t>(ch)]);
        }
        std::printf("\n    output shifts by matrix channel at the first check:");
        for (int ch = 0; ch < 12; ++ch) {
            std::printf(" %d", c.object_output_shift[static_cast<std::size_t>(ch)]);
        }
        std::printf("\n");
        std::printf("    per interval of the object substream: check, matrices, largest noise shift, "
                    "bits of the largest coded / bed / output sample:\n");
        for (std::size_t i = 0; i < c.object_interval_facts.size() && i < 80; ++i) {
            const auto& f = c.object_interval_facts[i];
            std::printf("      %2zu %c  mats %2u  noise %2u  coded %2u  bed %2u  out %2u\n", i,
                        (c.object_interval_log[i] & 1U) != 0 ? 'o' : 'F', f.matrices,
                        f.max_noise_shift, f.coded_bits, f.bed_bits, f.out_bits);
        }
        print_subsets(c);
    }
}

}  // namespace

// For a synthetic control that holds one sine in one matrix channel: how far the decoded output of
// that channel is from the ideal sine (amplitude, frequency in Hz, phase 0, 48 kHz), over the first
// intervals kept. The offset of the best match is searched, the deviations are listed by size, and
// the samples that are far from the sine are named.
void sine_check(const std::vector<std::byte>& stream, int channel, double amplitude, double hz) {
    DecoderConfig config;
    config.presentation = Presentation::kAll;
    config.strictness = Strictness::kLenient;
    Decoder decoder(config);
    ElementaryReader reader(stream);
    while (const auto unit = reader.next()) {
        (void)decoder.decode(*unit);
    }
    const DecoderCounters& c = decoder.counters();
    const std::size_t samples = c.object_first_out.size() / 12;
    const double pi = 3.14159265358979323846;
    const auto ideal = [&](long n) {
        return std::llround(amplitude * std::sin(2.0 * pi * hz * static_cast<double>(n) / 48000.0));
    };
    long best_offset = 0;
    double best_cost = 1e300;
    for (long offset = -400; offset <= 400; ++offset) {
        double cost = 0;
        for (std::size_t i = 1000; i < 3000 && i < samples; ++i) {
            const long n = static_cast<long>(i) + offset;
            cost += std::fabs(static_cast<double>(c.object_first_out[i * 12 + static_cast<std::size_t>(channel)]) -
                              static_cast<double>(ideal(n)));
        }
        if (cost < best_cost) {
            best_cost = cost;
            best_offset = offset;
        }
    }
    std::printf("sine check of matrix channel %d over %zu samples: best offset %ld (output sample i is "
                "input sample i + %ld), mean |deviation| %.2f\n",
                channel, samples, best_offset, best_offset, best_cost / 2000.0);
    std::vector<long> histogram(41, 0);  // deviation / 16 from -20 to 20, the ends hold the rest
    std::vector<std::pair<std::size_t, long>> outliers;
    for (std::size_t i = 0; i < samples; ++i) {
        const long dev = static_cast<long>(c.object_first_out[i * 12 + static_cast<std::size_t>(channel)]) -
                         ideal(static_cast<long>(i) + best_offset);
        const long bucket = std::clamp(static_cast<long>(std::lround(static_cast<double>(dev) / 16.0)), -20L, 20L);
        ++histogram[static_cast<std::size_t>(bucket + 20)];
        if (std::labs(dev) >= 64 && outliers.size() < 40) {
            outliers.emplace_back(i, dev);
        }
    }
    std::printf("deviation in units of 16 (value: count):");
    for (long b = -20; b <= 20; ++b) {
        if (histogram[static_cast<std::size_t>(b + 20)] != 0) {
            std::printf(" %ld:%ld", b, histogram[static_cast<std::size_t>(b + 20)]);
        }
    }
    std::printf("\nsamples whose deviation is 64 or more (index:deviation, first 40):");
    for (const auto& [index, dev] : outliers) {
        std::printf(" %zu:%ld", index, dev);
    }
    std::printf("\n");
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 6) {
        std::fprintf(stderr,
                     "usage: iclforge-truehd-ss3-lab STREAM\n"
                     "       iclforge-truehd-ss3-lab STREAM sine CHANNEL AMPLITUDE HZ\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> stream(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        stream[i] = static_cast<std::byte>(raw[i]);
    }
    if (argc == 6) {
        sine_check(stream, std::atoi(argv[3]), std::atof(argv[4]), std::atof(argv[5]));
        return 0;
    }
    std::vector<Setting> settings;
    DecoderLab base;
    settings.push_back({"standard", base});
    DecoderLab after = base;
    after.lower_rows_after_matrices = true;
    settings.push_back({"lower rows after their matrices", after});
    DecoderLab keep = base;
    keep.no_quantiser_mask = true;
    settings.push_back({"no quantiser mask", keep});
    DecoderLab reverse = base;
    reverse.reverse_matrices = true;
    settings.push_back({"matrices last to first", reverse});
    DecoderLab shift = base;
    shift.inverse_f_shift = true;
    settings.push_back({"inverse f_shift", shift});
    // Every combination of the four arithmetic switches (now that the matrices draw their noise).
    static std::vector<std::string> combination_names;
    combination_names.reserve(16);
    for (int bits = 1; bits < 16; ++bits) {
        DecoderLab combined = base;
        combined.lower_rows_after_matrices = (bits & 1) != 0;
        combined.no_quantiser_mask = (bits & 2) != 0;
        combined.reverse_matrices = (bits & 4) != 0;
        combined.inverse_f_shift = (bits & 8) != 0;
        combination_names.push_back(std::string("combo:") + ((bits & 1) != 0 ? " lower-after" : "") +
                                    ((bits & 2) != 0 ? " no-mask" : "") +
                                    ((bits & 4) != 0 ? " reversed" : "") +
                                    ((bits & 8) != 0 ? " inverse-fshift" : ""));
        settings.push_back({combination_names.back().c_str(), combined});
    }
    DecoderLab mask_coded = base;
    mask_coded.mask_rows = 1;
    settings.push_back({"quantiser mask: coded rows only", mask_coded});
    DecoderLab mask_bed = base;
    mask_bed.mask_rows = 2;
    settings.push_back({"quantiser mask: bed rows only", mask_bed});
    static const char* const kTieNames[] = {"", "matrix sums: a tie rounds up", "matrix sums: a tie rounds to even",
                                            "matrix sums: round to nearest (control)"};
    for (int mode = 1; mode <= 3; ++mode) {
        DecoderLab tied = base;
        tied.object_tie_mode = mode;
        settings.push_back({kTieNames[mode], tied});
    }
    static const char* const kScaleNames[] = {"", "noise scaled: minus the output shift", "noise scaled: minus the quantiser step",
                                              "noise scaled: minus both", "noise scaled: plus the output shift"};
    for (int mode = 1; mode <= 4; ++mode) {
        DecoderLab scaled = base;
        scaled.object_noise_scale = mode;
        settings.push_back({kScaleNames[mode], scaled});
    }
    for (int cap = 3; cap <= 8; ++cap) {
        static std::vector<std::string> cap_names;
        cap_names.reserve(8);
        DecoderLab capped = base;
        capped.object_noise_cap = cap;
        cap_names.push_back("noise number capped at " + std::to_string(cap));
        settings.push_back({cap_names.back().c_str(), capped});
    }
    for (int mask : {15, 31}) {
        static std::vector<std::string> mask_names;
        mask_names.reserve(4);
        DecoderLab masked = base;
        masked.object_noise_stride_mask = mask;
        mask_names.push_back("noise stride masked with " + std::to_string(mask));
        settings.push_back({mask_names.back().c_str(), masked});
    }
    DecoderLab noisy_only = base;
    noisy_only.object_noise_left = 1;
    settings.push_back({"noise start index: noisy matrices only", noisy_only});
    DecoderLab cleared = base;
    cleared.object_restart_clears = true;
    settings.push_back({"object restart clears histories", cleared});
    static const char* const kIndexNames[] = {"", "noise index: from the block", "noise index: matrices from the first",
                                              "noise index: from the block, matrices from the first"};
    for (int mode = 1; mode <= 3; ++mode) {
        DecoderLab indexed = base;
        indexed.object_noise_index = mode;
        settings.push_back({kIndexNames[mode], indexed});
    }
    static const char* const kScopeNames[] = {
        "",
        "fold: coded channels, shift by matrix channel",
        "fold: coded channels, shift by place",
        "fold: channels below, shift by matrix channel",
        "fold: channels below, shift by place",
        "fold: coded samples before matrices, shift by matrix channel",
        "fold: coded samples before matrices, shift by place"};
    for (int scope = 1; scope <= 6; ++scope) {
        DecoderLab folded = base;
        folded.fold_scope = scope;
        settings.push_back({kScopeNames[scope], folded});
    }
    static const char* const kUpwardNames[] = {
        "fold: matrix channels 1 up", "fold: matrix channels 2 up", "fold: matrix channels 3 up",
        "fold: matrix channels 4 up", "fold: matrix channels 5 up", "fold: matrix channels 6 up",
        "fold: matrix channels 7 up", "fold: matrix channels 8 up"};
    for (int from = 1; from <= 8; ++from) {
        DecoderLab folded = base;
        folded.fold_scope = 10 + from;
        settings.push_back({kUpwardNames[from - 1], folded});
    }
    bool first = true;
    for (const Setting& setting : settings) {
        run(stream, setting.name, setting.lab, first);
        first = false;
    }
    return 0;
}
