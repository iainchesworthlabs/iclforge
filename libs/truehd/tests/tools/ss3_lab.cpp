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
    std::printf("%-34s ext=%d  units %llu decoded %llu errors %llu  ss3 ok %llu failed %llu  "
                "ss0-2 ok %llu failed %llu",
                name, lab.extension_variant, static_cast<unsigned long long>(units),
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
        print_subsets(c);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: iclforge-truehd-ss3-lab STREAM\n");
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
    // The grammars of the second precision term, under the standard arithmetic: a grammar that
    // reads the owner's stream pushes the first error far past the first extension.
    std::printf("grammars of the second precision term (variant: bit 0 all channels, bit 1 shared "
                "width, bits 2-3 structure, bit 4 width + 1, bits 5-6 field bits):\n");
    for (int variant = 0; variant < 128; ++variant) {
        if (((variant >> 2) & 3) == 3 && variant != 12) {
            continue;  // structure 3 is one bit per matrix channel; only its plain form varies
        }
        DecoderLab lab;
        lab.extension_variant = variant;
        char name[32];
        std::snprintf(name, sizeof name, "extension variant %3d", variant);
        run(stream, name, lab, false, true);
    }
    return 0;
}
