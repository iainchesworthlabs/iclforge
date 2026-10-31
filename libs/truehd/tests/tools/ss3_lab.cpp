// iclforge-truehd-ss3-lab: runs the decoder over a TrueHD Atmos stream under each setting of
// DecoderLab, the switches for the points of the object substream (restart sync 0x31EC) that no
// stream has settled (libs/truehd/ERRATA.md entry 69), and prints one line a run: how many units
// parsed, the first error, and the lossless checks of the object substream and of the others. The
// setting whose object substream passes every check says what the point is.
//
//   iclforge-truehd-ss3-lab STREAM
//
// Prints counts and error texts only: no sample leaves the decoder.

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

void run(const std::vector<std::byte>& stream, const char* name, const DecoderLab& lab) {
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
    for (int ext = 0; ext < 4; ++ext) {
        DecoderLab base;
        base.extension_variant = ext;
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
    }
    for (const Setting& setting : settings) {
        run(stream, setting.name, setting.lab);
    }
    return 0;
}
