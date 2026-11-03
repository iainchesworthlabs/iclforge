// iclforge-truehd-render-tool: writes the Atmos test streams of the render side (libs/truehd/notes/
// feature-truehd-atmos-encoder-render.md) from deterministic synthetic masters: sine objects with
// key tracks, clustered into 12, 14 or 16 elements, the bed rendered from them (PanLaw::kWall) and
// the plan written through EncoderConfig::object_paths. For handing to a decoder as a black box; no
// object metadata is written beyond what the encoder writes itself.
//
//   iclforge-truehd-render-tool OUT_DIR [CASE ...]
//
// For each case it writes NAME.thd, NAME.master.wav (the objects, 24 bit, one channel each) and
// NAME.master.txt (each object's tone, bed row and keys, in the metadata's cube). No CASE: all.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

#include "encoder/cluster_elements.hpp"
#include "encoder/render_bed.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::detail;

constexpr std::size_t kSamples = 480000;  // 10 s at 48 kHz
constexpr double kMinus20 = 838860.8;     // a -20 dBFS peak in 24 bits
constexpr double kMinus26 = 420426.3;

// A key in the metadata's cube (x -1 left to 1 right, y -1 rear to 1 front, z 0 to 1 up).
struct CubeKey {
    std::int64_t sample;
    double x;
    double y;
    double z;
};

struct Object {
    double hz;
    double amplitude;
    int bed_row;  // -1: dynamic
    std::vector<CubeKey> keys;
    std::vector<std::int32_t> pcm;
};

struct Scene {
    std::string name;
    int elements;
    bool slopes;
    std::vector<Object> objects;
};

Object make_object(double hz, double amplitude, int bed_row, std::vector<CubeKey> keys) {
    Object object{hz, amplitude, bed_row, std::move(keys), {}};
    object.pcm.resize(kSamples);
    for (std::size_t t = 0; t < kSamples; ++t) {
        object.pcm[t] = static_cast<std::int32_t>(std::lround(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(t) / 48000.0)));
    }
    return object;
}

std::vector<Scene> scenes() {
    const std::int64_t last = static_cast<std::int64_t>(kSamples) - 1;
    std::vector<Scene> all;
    {
        Scene s{"atmos-bed-only", 12, false, {}};
        for (int row = 0; row < 8; ++row) {
            s.objects.push_back(
                make_object(110.0 + 90.0 * row, kMinus20, row, {{0, 0.0, 0.0, 0.0}}));
        }
        all.push_back(std::move(s));
    }
    all.push_back({"atmos-static-1obj", 12, false,
                   {make_object(1000.0, kMinus20, -1, {{0, 1.0, 0.85, 0.0}})}});
    for (const bool slopes : {true, false}) {
        const std::string tail = slopes ? "" : "-noext";
        all.push_back({"atmos-moving-1obj-linear" + tail, 12, slopes,
                       {make_object(1000.0, kMinus20, -1,
                                    {{0, -1.0, 1.0, 0.0}, {last, 1.0, 1.0, 0.0}})}});
        all.push_back({"atmos-moving-wall" + tail, 12, slopes,
                       {make_object(1000.0, kMinus20, -1,
                                    {{0, -1.0, 1.0, 0.0}, {last, -1.0, -1.0, 0.0}})}});
    }
    for (const int elements : {12, 14, 16}) {
        Scene s{"atmos-" + std::to_string(elements) + "el", elements, true, {}};
        for (int i = 0; i < 6; ++i) {
            // six objects on circles of different radius and speed round the middle of the room
            const double radius = 0.45 + 0.08 * i;
            const double turn = 2.0 * std::numbers::pi * (0.25 + 0.1 * i);
            const double phase = 2.0 * std::numbers::pi * i / 6.0;
            std::vector<CubeKey> keys;
            for (std::int64_t t = 0; t <= last + 1; t += 24000) {
                const double a = phase + turn * static_cast<double>(t) / kSamples;
                keys.push_back({std::min(t, last), radius * std::cos(a), radius * std::sin(a),
                                0.2 + 0.1 * i});
            }
            s.objects.push_back(make_object(300.0 + 170.0 * i, kMinus26, -1, std::move(keys)));
        }
        all.push_back(std::move(s));
    }
    all.push_back({"atmos-3obj", 12, false,
                   {make_object(1000.0, kMinus20, -1, {{0, 1.0, 0.85, 0.0}}),
                    make_object(3000.0, kMinus26, -1, {{0, -1.0, 0.5, 0.0}}),
                    make_object(440.0, kMinus26, -1, {{0, 0.5, 0.3, 0.0}})}});
    return all;
}

void put32(std::ofstream& out, std::uint32_t v) {
    const char b[4] = {static_cast<char>(v & 255U), static_cast<char>((v >> 8U) & 255U),
                       static_cast<char>((v >> 16U) & 255U), static_cast<char>((v >> 24U) & 255U)};
    out.write(b, 4);
}
void put16(std::ofstream& out, std::uint16_t v) {
    const char b[2] = {static_cast<char>(v & 255U), static_cast<char>((v >> 8U) & 255U)};
    out.write(b, 2);
}

void write_master(const std::string& base, const Scene& scene) {
    std::ofstream wav(base + ".master.wav", std::ios::binary);
    const auto channels = static_cast<std::uint32_t>(scene.objects.size());
    const std::uint32_t data = static_cast<std::uint32_t>(kSamples) * channels * 3;
    wav.write("RIFF", 4);
    put32(wav, 36 + data);
    wav.write("WAVEfmt ", 8);
    put32(wav, 16);
    put16(wav, 1);
    put16(wav, static_cast<std::uint16_t>(channels));
    put32(wav, 48000);
    put32(wav, 48000 * channels * 3);
    put16(wav, static_cast<std::uint16_t>(channels * 3));
    put16(wav, 24);
    wav.write("data", 4);
    put32(wav, data);
    for (std::size_t t = 0; t < kSamples; ++t) {
        for (const Object& object : scene.objects) {
            const auto v = static_cast<std::uint32_t>(object.pcm[t]);
            const char b[3] = {static_cast<char>(v & 255U), static_cast<char>((v >> 8U) & 255U),
                               static_cast<char>((v >> 16U) & 255U)};
            wav.write(b, 3);
        }
    }
    std::ofstream txt(base + ".master.txt");
    txt << "case " << scene.name << ": 48 kHz, 24 bit, " << kSamples << " samples, "
        << scene.elements << " elements, extension " << (scene.slopes ? "slopes" : "none") << "\n";
    for (std::size_t i = 0; i < scene.objects.size(); ++i) {
        const Object& o = scene.objects[i];
        txt << "object " << i << ": sine " << o.hz << " Hz, peak " << o.amplitude << ", "
            << (o.bed_row >= 0 ? "bed row " + std::to_string(o.bed_row) : std::string("dynamic"))
            << "\n";
        for (const CubeKey& k : o.keys) {
            txt << "  key sample " << k.sample << " pos [" << k.x << ", " << k.y << ", " << k.z
                << "]\n";
        }
    }
}

int run(const Scene& scene, const std::string& dir) {
    const std::string base = dir + "/" + scene.name;
    write_master(base, scene);
    std::vector<ClusterObject> objects;
    for (const Object& o : scene.objects) {
        ClusterObject c;
        c.pcm = o.pcm;
        c.bed_row = o.bed_row;
        for (const CubeKey& k : o.keys) {
            // the cube to the room: x 0 left to 1 right, y 0 front to 1 back
            c.keys.push_back(ClusterKey{k.sample, (k.x + 1.0) / 2.0, (1.0 - k.y) / 2.0, k.z, 1.0});
        }
        objects.push_back(std::move(c));
    }
    ClusterConfig config;
    config.elements = scene.elements;
    config.section_samples = 1920;
    auto clustered = cluster_objects(objects, config);
    if (!clustered) {
        std::fprintf(stderr, "%s: %s\n", scene.name.c_str(), clustered.error().detail.c_str());
        return 1;
    }
    const RenderPlan plan = make_render_plan(*clustered, PanLaw::kWall);

    EncoderConfig writer;
    writer.channels = 8;
    writer.object_channels = scene.elements;
    writer.object_layout = ObjectLayout::kBedMatrices;
    writer.object_meaning.dyn_object_only = true;
    writer.object_meaning.lfe_present = true;
    writer.object_paths = object_paths_of(plan);
    writer.object_extension = scene.slopes ? ObjectExtension::kSlopes : ObjectExtension::kNone;
    writer.verify = true;
    auto encoder = Encoder::create(writer);
    if (!encoder) {
        std::fprintf(stderr, "%s: %.*s\n", scene.name.c_str(),
                     static_cast<int>(encoder.error().detail.size()),
                     encoder.error().detail.data());
        return 1;
    }
    std::ofstream out(base + ".thd", std::ios::binary);
    std::uint64_t bytes = 0;
    const auto emit = [&](const std::vector<EncodedAccessUnit>& units) {
        for (const EncodedAccessUnit& unit : units) {
            out.write(reinterpret_cast<const char*>(unit.bytes.data()),
                      static_cast<std::streamsize>(unit.bytes.size()));
            bytes += unit.bytes.size();
        }
    };
    constexpr std::size_t kChunk = 4800;
    std::vector<std::int32_t> block;
    for (std::size_t at = 0; at < kSamples; at += kChunk) {
        const std::size_t count = std::min(kChunk, kSamples - at);
        block.assign(count * static_cast<std::size_t>(scene.elements), 0);
        for (std::size_t t = 0; t < count; ++t) {
            for (int e = 0; e < scene.elements; ++e) {
                block[t * static_cast<std::size_t>(scene.elements) + static_cast<std::size_t>(e)] =
                    clustered->signals[static_cast<std::size_t>(e)][at + t];
            }
        }
        auto units = encoder->encode(block);
        if (!units) {
            std::fprintf(stderr, "%s: %.*s\n", scene.name.c_str(),
                         static_cast<int>(units.error().detail.size()),
                         units.error().detail.data());
            return 1;
        }
        emit(*units);
    }
    auto tail = encoder->flush();
    if (!tail) {
        std::fprintf(stderr, "%s: %.*s\n", scene.name.c_str(),
                     static_cast<int>(tail.error().detail.size()), tail.error().detail.data());
        return 1;
    }
    emit(*tail);
    const EncoderStats stats = encoder->stats();
    std::printf("case=%s bytes=%llu units=%llu verified=%llu mix_reduced=%llu clipped=%llu\n",
                scene.name.c_str(), static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(stats.access_units),
                static_cast<unsigned long long>(stats.verified_units),
                static_cast<unsigned long long>(stats.object_mix_reduced_intervals),
                static_cast<unsigned long long>(clustered->clipped_samples));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s OUT_DIR [CASE ...]\n", argv[0]);
        return 2;
    }
    int status = 0;
    for (const Scene& scene : scenes()) {
        bool wanted = argc == 2;
        for (int i = 2; i < argc; ++i) {
            wanted = wanted || scene.name == argv[i];
        }
        if (wanted) {
            status |= run(scene, argv[1]);
        }
    }
    return status;
}
