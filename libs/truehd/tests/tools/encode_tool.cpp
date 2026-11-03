// iclforge-truehd-encode-tool: a small command line over iclforge::truehd::Encoder for the
// interoperability checks (tools/checks/verify_truehd_encode_interop.py, tools/ci/
// fuzz_truehd_encoder_space.py). WAV in, TrueHD out. The forge `encode` command is the real
// front end and replaces this when it lands (planning/truehd.md, phase T2c).
//
//   iclforge-truehd-encode-tool in.wav out.thd [bits=N] [restart=N] [order=N] [matrix=0|1]
//       [verify=0|1] [chunk=N] [peak=BPS] [assignment=0xNNNN] [stereo-mode=N] [rate-violations=0|1]
//       [object-bed=0|1] [object-noise=N] [object-bypass=0|1] [object-bypass-bits=1..3]
//       [object-motion=static|slow|jump] [object-slopes=0|1] [object-update=N]
//       [object-drop=0|1] [object-split=0|1] [object-stretch=N]
//
// object-bed=1 reads the file's 9 to 16 channels as the objects of an Atmos stream whose 7.1 bed is
// made from them (EncoderConfig::object_layout kBedMatrices, ERRATA.md entry 70, 48 kHz).
// object-motion moves the objects in the bed (tools/checks/verify_truehd_object_bed.py defines the
// same paths): every `object-stretch` samples (24,000) object k goes from its own bed row to the
// next object's (slow: a straight line across the stretch; jump: a step at its end), alternating.
// object-slopes=1 sends the extension's slopes (a section only where the path turns or the
// coefficient has drifted); it needs object-update=1.
//
// Reads 16, 24 or 32 bit integer PCM (WAVE_FORMAT_PCM or EXTENSIBLE). `bits` is the width of the
// samples to code (default: the file's, at most 24); a 24 bit file given bits=16 must carry 16 bit
// samples in its high bits and is refused if its low 8 bits are not clear. Prints one line of
// key=value pairs on success.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/truehd/encoder/encoder.hpp"

namespace {

using namespace iclforge::truehd;

struct Wav {
    std::uint32_t sample_rate = 0;
    int channels = 0;
    int bits = 0;
    std::vector<std::int32_t> samples;  // interleaved, as stored (sign-extended)
};

std::uint32_t le32(const std::vector<unsigned char>& d, std::size_t o) {
    return d[o] | (static_cast<std::uint32_t>(d[o + 1]) << 8U) |
           (static_cast<std::uint32_t>(d[o + 2]) << 16U) |
           (static_cast<std::uint32_t>(d[o + 3]) << 24U);
}
std::uint16_t le16(const std::vector<unsigned char>& d, std::size_t o) {
    return static_cast<std::uint16_t>(d[o] | (d[o + 1] << 8U));
}

bool read_wav(const std::string& path, Wav& wav, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    const std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 ||
        std::memcmp(d.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    std::size_t pos = 12;
    std::size_t data_at = 0;
    std::size_t data_size = 0;
    bool have_fmt = false;
    while (pos + 8 <= d.size()) {
        const std::uint32_t size = le32(d, pos + 4);
        if (std::memcmp(d.data() + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= d.size()) {
            const std::uint16_t tag = le16(d, pos + 8);
            wav.channels = le16(d, pos + 10);
            wav.sample_rate = le32(d, pos + 12);
            wav.bits = le16(d, pos + 22);
            if (tag != 1 && tag != 0xFFFE) {
                error = "not integer PCM";
                return false;
            }
            have_fmt = true;
        } else if (std::memcmp(d.data() + pos, "data", 4) == 0) {
            data_at = pos + 8;
            data_size = std::min<std::size_t>(size, d.size() - data_at);
            break;
        }
        pos += 8 + static_cast<std::size_t>(size) + (size & 1U);
    }
    if (!have_fmt || data_at == 0) {
        error = "no fmt or data chunk";
        return false;
    }
    const int bytes = wav.bits / 8;
    if (wav.bits != 16 && wav.bits != 24 && wav.bits != 32) {
        error = "only 16, 24 and 32 bit PCM";
        return false;
    }
    const std::size_t count = data_size / static_cast<std::size_t>(bytes);
    wav.samples.resize(count - count % static_cast<std::size_t>(wav.channels));
    for (std::size_t i = 0; i < wav.samples.size(); ++i) {
        const std::size_t o = data_at + i * static_cast<std::size_t>(bytes);
        std::uint32_t v = 0;
        for (int b = 0; b < bytes; ++b) {
            v |= static_cast<std::uint32_t>(d[o + static_cast<std::size_t>(b)])
                 << (8U * static_cast<unsigned>(b));
        }
        wav.samples[i] = static_cast<std::int32_t>(v << (32 - wav.bits)) >> (32 - wav.bits);
    }
    return true;
}

bool option(std::string_view arg, std::string_view key, std::string& value) {
    if (arg.size() > key.size() && arg.substr(0, key.size()) == key && arg[key.size()] == '=') {
        value = std::string(arg.substr(key.size() + 1));
        return true;
    }
    return false;
}

}  // namespace

// The paths of the object-motion presets (verify_truehd_object_bed.py has the same rule): object k
// (channel 8 + k) sits on its own bed row at -3 dB for a stretch of samples, then on the next
// object's row for a stretch, and so on. In "slow" the gain moves in a straight line from one
// stretch to the next; in "jump" it holds and steps.
std::vector<ObjectPath> motion_paths(int objects, std::size_t frames, std::int64_t kStretch,
                                     bool jump) {
    constexpr std::int32_t kGain = 46341;
    constexpr int kRowOfObject[8] = {0, 1, 2, 4, 5, 6, 7, 3};
    std::vector<ObjectPath> paths;
    for (int k = 0; k + 8 < objects; ++k) {
        ObjectPath path;
        path.object = 8 + k;
        const auto stretches = static_cast<std::int64_t>(frames) / kStretch + 2;
        for (std::int64_t n = 0; n < stretches; ++n) {
            const int row = kRowOfObject[(k + static_cast<int>(n % 2)) % 8];
            ObjectGainKnot knot;
            knot.sample = n * kStretch;
            knot.gain[static_cast<std::size_t>(row)] = kGain;
            path.knots.push_back(knot);
            if (jump) {
                knot.sample = (n + 1) * kStretch;
                path.knots.push_back(knot);
            }
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s in.wav out.thd [key=value ...]\n", argv[0]);
        return 2;
    }
    Wav wav;
    std::string error;
    if (!read_wav(argv[1], wav, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    EncoderConfig config;
    config.sample_rate = wav.sample_rate;
    config.channels = wav.channels;
    config.bits = std::min(wav.bits, 24);
    std::size_t chunk = 4096;
    bool object_bed = false;
    std::string object_motion = "static";
    std::int64_t object_stretch = 24000;
    for (int i = 3; i < argc; ++i) {
        std::string value;
        const std::string_view arg = argv[i];
        if (option(arg, "bits", value)) {
            config.bits = std::atoi(value.c_str());
        } else if (option(arg, "restart", value)) {
            config.restart_interval_units = std::atoi(value.c_str());
        } else if (option(arg, "order", value)) {
            config.max_fir_order = std::atoi(value.c_str());
        } else if (option(arg, "matrix", value)) {
            config.matrix_search = value != "0";
        } else if (option(arg, "verify", value)) {
            config.verify = value != "0";
        } else if (option(arg, "chunk", value)) {
            chunk = static_cast<std::size_t>(std::atoi(value.c_str()));
        } else if (option(arg, "peak", value)) {
            config.peak_data_rate_bps =
                static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
        } else if (option(arg, "assignment", value)) {
            config.channel_assignment =
                static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
        } else if (option(arg, "stereo-mode", value)) {
            config.stereo_mode = static_cast<unsigned>(std::atoi(value.c_str()));
        } else if (option(arg, "rate-violations", value)) {
            config.allow_rate_violations = value != "0";
        } else if (option(arg, "downmix", value)) {
            config.downmix = value == "standard" ? DownmixMode::kStandard : DownmixMode::kNone;
        } else if (option(arg, "object-bed", value)) {
            object_bed = value != "0";
        } else if (option(arg, "object-noise", value)) {
            config.object_noise_shift = std::atoi(value.c_str());
        } else if (option(arg, "object-bypass", value)) {
            config.object_lsb_bypass = value != "0";
        } else if (option(arg, "object-bypass-bits", value)) {
            config.object_bypass_bits = std::atoi(value.c_str());
        } else if (option(arg, "object-motion", value)) {
            object_motion = value;
        } else if (option(arg, "object-slopes", value)) {
            config.object_extension =
                value != "0" ? ObjectExtension::kSlopes : ObjectExtension::kNone;
        } else if (option(arg, "object-update", value)) {
            config.object_update_units = std::atoi(value.c_str());
        } else if (option(arg, "object-split", value)) {
            config.object_split_mixes = value != "0";
        } else if (option(arg, "object-stretch", value)) {
            object_stretch = std::atoll(value.c_str());
        } else if (option(arg, "object-drop", value)) {
            config.object_drop_idle_matrices = value != "0";
        } else {
            std::fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (object_bed) {
        config.object_channels = wav.channels;
        config.channels = 8;
        config.object_layout = ObjectLayout::kBedMatrices;
        config.object_meaning.dyn_object_only = true;
        config.object_meaning.lfe_present = true;
        if (object_motion != "static") {
            config.object_paths = motion_paths(
                wav.channels, wav.samples.size() / static_cast<std::size_t>(wav.channels),
                object_stretch, object_motion == "jump");
        }
    }
    // A file wider than the coded width carries the samples in its high bits.
    const int file_bits = std::min(wav.bits, 24);
    if (wav.bits == 32) {
        for (std::int32_t& s : wav.samples) {
            s >>= 8;  // 32 bit PCM: take the high 24 bits
        }
    }
    if (config.bits < file_bits) {
        const int drop = file_bits - config.bits;
        for (std::int32_t& s : wav.samples) {
            if ((s & ((1 << drop) - 1)) != 0) {
                std::fprintf(stderr, "error: bits=%d but the file has set bits below them\n",
                             config.bits);
                return 1;
            }
            s >>= drop;
        }
    }
    auto encoder = Encoder::create(config);
    if (!encoder) {
        std::fprintf(stderr, "error: %.*s\n", static_cast<int>(encoder.error().detail.size()),
                     encoder.error().detail.data());
        return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    std::uint64_t written = 0;
    const auto emit = [&](const std::vector<EncodedAccessUnit>& units) {
        for (const EncodedAccessUnit& unit : units) {
            out.write(reinterpret_cast<const char*>(unit.bytes.data()),
                      static_cast<std::streamsize>(unit.bytes.size()));
            written += unit.bytes.size();
        }
    };
    const std::size_t step = chunk * static_cast<std::size_t>(wav.channels);
    for (std::size_t at = 0; at < wav.samples.size(); at += step) {
        const std::size_t count = std::min(step, wav.samples.size() - at);
        auto units = encoder->encode(std::span<const std::int32_t>(wav.samples).subspan(at, count));
        if (!units) {
            std::fprintf(stderr, "error: %.*s\n", static_cast<int>(units.error().detail.size()),
                         units.error().detail.data());
            return 1;
        }
        emit(*units);
    }
    auto tail = encoder->flush();
    if (!tail) {
        std::fprintf(stderr, "error: %.*s\n", static_cast<int>(tail.error().detail.size()),
                     tail.error().detail.data());
        return 1;
    }
    emit(*tail);
    const EncoderStats& stats = encoder->stats();
    std::printf(
        "units=%llu bytes=%llu input_samples=%llu intervals=%llu matrices=%llu "
        "max_unit_bytes=%u max_unit_rate_bps=%llu rate_violations=%llu verified=%llu "
        "substreams=%d\n",
        static_cast<unsigned long long>(stats.access_units),
        static_cast<unsigned long long>(written),
        static_cast<unsigned long long>(stats.input_samples),
        static_cast<unsigned long long>(stats.restart_intervals),
        static_cast<unsigned long long>(stats.matrices_used), stats.max_unit_bytes,
        static_cast<unsigned long long>(stats.max_unit_rate_bps),
        static_cast<unsigned long long>(stats.rate_violations),
        static_cast<unsigned long long>(stats.verified_units), encoder->substreams());
    return 0;
}
