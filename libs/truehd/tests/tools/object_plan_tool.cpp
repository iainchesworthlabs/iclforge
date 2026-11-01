// iclforge-truehd-object-plan-tool: the C++ side of the agreement check between the encoder's plan
// of moving objects and the Python reference writer's (tools/checks/verify_truehd_object_bed.py,
// tools/references/truehd_writer.py generate-moving).
//
//   iclforge-truehd-object-plan-tool plan  NAME.request NAME.objects
//   iclforge-truehd-object-plan-tool decode NAME.thd [ramp=none|end|start]
//
// `plan` reads a request (the objects, the matrices' parameters, the paths' knots, one line each:
// "key value", "knot CHANNEL SAMPLE GAIN x 8") and the objects as 32-bit little-endian samples, a
// frame after another, and prints what the encoder's planner makes of them for each restart
// interval: a line "section ABSOLUTE_SAMPLE SIZE BITS" for every block whose header sends a matrix
// section (the section's bits as 0 and 1, after the matrices-present bit), then "rows HEX" and
// "bypass HEX" (FNV-1a of the rows the decoder starts from and of the LSB bypass bits) and "pcm
// HEX" (FNV-1a of the objects). `decode` prints "pcm HEX" for what this library's decoder gives for
// substream 3 of a stream, with the lab settings for the ramp when asked. The Python side prints
// the same lines; the check compares them.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "encoder/object_bed.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/object_paths.hpp"

namespace {

using namespace iclforge::truehd;
namespace d = iclforge::truehd::detail;

constexpr std::uint64_t kFnvBasis = 0xCBF29CE484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001B3ULL;

void fnv(std::uint64_t& state, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        state = (state ^ bytes[i]) * kFnvPrime;
    }
}

void fnv_int32(std::uint64_t& state, std::int32_t value) {
    std::uint8_t bytes[4];
    const auto u = static_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) {
        bytes[i] = static_cast<std::uint8_t>(u >> (8 * i));
    }
    fnv(state, bytes, 4);
}

struct Request {
    d::ObjectTimelineRequest timeline;
    int interval_units = 8;
    int samples = 0;
};

bool read_request(const char* file, Request& out) {
    std::ifstream in(file);
    if (!in) {
        return false;
    }
    std::map<int, ObjectPath> by_channel;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string key;
        words >> key;
        long long value = 0;
        if (key == "knot") {
            int channel = 0;
            ObjectGainKnot knot;
            words >> channel >> knot.sample;
            for (std::int32_t& g : knot.gain) {
                words >> g;
            }
            by_channel[channel].object = channel;
            by_channel[channel].knots.push_back(knot);
        } else if (key == "ramp") {
            std::string mode;
            words >> mode;
            out.timeline.ramp = mode == "end"     ? ObjectRamp::kEnd
                                : mode == "start" ? ObjectRamp::kStart
                                                  : ObjectRamp::kNone;
        } else {
            words >> value;
            if (key == "objects") out.timeline.objects = static_cast<int>(value);
            if (key == "noise") out.timeline.noise_shift = static_cast<int>(value);
            if (key == "bypass_bits") out.timeline.bypass_bits = static_cast<int>(value);
            if (key == "mix_step") out.timeline.mix_step = static_cast<int>(value);
            if (key == "update_units") out.timeline.update_units = static_cast<int>(value);
            if (key == "drop_idle") out.timeline.drop_idle = value != 0;
            if (key == "split") out.timeline.split_mixes = value != 0;
            if (key == "nominal") out.timeline.nominal = static_cast<int>(value);
            if (key == "interval_units") out.interval_units = static_cast<int>(value);
            if (key == "samples") out.samples = static_cast<int>(value);
        }
    }
    for (auto& [channel, path] : by_channel) {
        out.timeline.paths.push_back(std::move(path));
    }
    return true;
}

// The bits of a block header that sends `section` and nothing else, less the bits around the
// section: the new-flags bit and the block-size bit before the matrices-present bit, and after the
// section the output-shift and quantiser-step bits and a bit for each coded channel.
std::string section_bits(const MatrixSet& section, int objects) {
    BlockContext context;
    context.min_chan = d::kBedChannels;
    context.max_chan = objects - 1;
    context.max_matrix_chan = objects - 1;
    context.object_matrices = true;
    BlockHeaderSpec header;
    header.matrices = section;
    iclforge::BitWriter writer;
    write_block_header(writer, context, header);
    const std::size_t count = writer.bit_count();
    const std::vector<std::byte> bytes = writer.take();
    std::string bits;
    for (std::size_t i = 0; i < count; ++i) {
        bits += ((static_cast<unsigned>(bytes[i / 8]) >> (7 - i % 8)) & 1U) != 0 ? '1' : '0';
    }
    const std::size_t head = 3;
    const std::size_t tail = 2 + static_cast<std::size_t>(objects - d::kBedChannels);
    return bits.substr(head, bits.size() - head - tail);
}

int plan(const char* request_path, const char* objects_path) {
    Request request;
    if (!read_request(request_path, request)) {
        std::fprintf(stderr, "cannot read %s\n", request_path);
        return 1;
    }
    std::ifstream in(objects_path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    const int objects = request.timeline.objects;
    const auto samples = static_cast<std::size_t>(request.samples);
    if (raw.size() != samples * static_cast<std::size_t>(objects) * 4) {
        std::fprintf(stderr, "%s is not %d frames of %d objects\n", objects_path, request.samples,
                     objects);
        return 1;
    }
    std::vector<d::ObjectPlane> all(static_cast<std::size_t>(objects), d::ObjectPlane(samples));
    for (std::size_t t = 0; t < samples; ++t) {
        for (std::size_t c = 0; c < static_cast<std::size_t>(objects); ++c) {
            std::int32_t v = 0;
            std::copy_n(raw.data() + (t * static_cast<std::size_t>(objects) + c) * 4, 4,
                        reinterpret_cast<char*>(&v));
            all[c][t] = v;
        }
    }
    const std::size_t interval = static_cast<std::size_t>(request.interval_units) *
                                 static_cast<std::size_t>(request.timeline.nominal);
    std::vector<d::ObjectPlane> start(all.size(), d::ObjectPlane(samples));
    std::uint64_t bypass_hash = kFnvBasis;
    for (std::size_t first = 0; first < samples; first += interval) {
        d::ObjectTimelineRequest one = request.timeline;
        one.first_sample = static_cast<std::int64_t>(first);
        one.blocks.emplace_back(0, 8);
        one.blocks.emplace_back(8, one.nominal - 8);
        for (int u = 1; u < request.interval_units; ++u) {
            one.blocks.emplace_back(
                static_cast<std::size_t>(u) * static_cast<std::size_t>(one.nominal), one.nominal);
        }
        const auto timeline = d::build_object_timeline(one);
        if (!timeline) {
            std::fprintf(stderr, "cannot plan: %.*s\n", static_cast<int>(timeline.error().size()),
                         timeline.error().data());
            return 1;
        }
        for (const d::ObjectBlock& block : timeline->blocks) {
            if (block.send) {
                std::printf("section %zu %d %s\n", first + block.start, block.size,
                            section_bits(block.section, objects).c_str());
            }
        }
        std::vector<d::ObjectPlane> rows(all.size());
        for (std::size_t c = 0; c < all.size(); ++c) {
            rows[c].assign(all[c].begin() + static_cast<std::ptrdiff_t>(first),
                           all[c].begin() + static_cast<std::ptrdiff_t>(first + interval));
        }
        d::ObjectChainShape shape;
        shape.units = request.interval_units;
        shape.nominal = request.timeline.nominal;
        d::ObjectBypass bypass;
        if (!d::invert_object_timeline(*timeline, shape, rows, bypass)) {
            std::fprintf(stderr, "the matrices cannot be run backwards\n");
            return 1;
        }
        for (std::size_t c = 0; c < all.size(); ++c) {
            std::copy(rows[c].begin(), rows[c].end(),
                      start[c].begin() + static_cast<std::ptrdiff_t>(first));
        }
        for (const d::ObjectBlock& block : timeline->blocks) {
            std::size_t slots = 0;
            for (const MatrixSpec& m : block.matrices) {
                slots += m.lsb_bypass ? 1U : 0U;
            }
            for (int i = 0; i < block.size; ++i) {
                for (std::size_t k = 0; k < slots; ++k) {
                    const std::uint8_t v = bypass[k][block.start + static_cast<std::size_t>(i)];
                    fnv(bypass_hash, &v, 1);
                }
            }
        }
    }
    std::uint64_t rows_hash = kFnvBasis;
    std::uint64_t pcm_hash = kFnvBasis;
    for (std::size_t t = 0; t < samples; ++t) {
        for (std::size_t c = 0; c < all.size(); ++c) {
            fnv_int32(rows_hash, start[c][t]);
            fnv_int32(pcm_hash, all[c][t]);
        }
    }
    std::printf(
        "rows %016llx\nbypass %016llx\npcm %016llx\n", static_cast<unsigned long long>(rows_hash),
        static_cast<unsigned long long>(bypass_hash), static_cast<unsigned long long>(pcm_hash));
    return 0;
}

int decode(const char* stream_path, const std::string& ramp) {
    std::ifstream in(stream_path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    DecoderConfig config;
    config.presentation = Presentation::kAll;
    if (ramp != "none") {
        config.lab.object_extension = ramp == "end" ? 4 : 5;
        config.lab.object_extension_fixed = 19;
    }
    Decoder decoder(config);
    // the stream is a run of access units, each led by its length in the low 12 bits of two bytes
    // (in words)
    std::uint64_t hash = kFnvBasis;
    std::size_t at = 0;
    while (at + 4 <= raw.size()) {
        const auto first = static_cast<unsigned>(static_cast<std::uint8_t>(raw[at]));
        const auto second = static_cast<unsigned>(static_cast<std::uint8_t>(raw[at + 1]));
        const std::size_t size = (((first & 0xFU) << 8U) | second) * 2U;
        if (size < 4 || at + size > raw.size()) {
            std::fprintf(stderr, "a damaged access unit at byte %zu\n", at);
            return 1;
        }
        const auto* unit = reinterpret_cast<const std::byte*>(raw.data() + at);
        auto decoded = decoder.decode(std::span<const std::byte>(unit, size));
        if (!decoded || !*decoded) {
            std::fprintf(stderr, "the decoder refused the access unit at byte %zu\n", at);
            return 1;
        }
        for (const std::int32_t sample : (**decoded).pcm) {
            fnv_int32(hash, sample);
        }
        at += size;
    }
    std::printf("pcm %016llx\n", static_cast<unsigned long long>(hash));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "plan") {
        return plan(argv[2], argv[3]);
    }
    if (argc >= 3 && std::string(argv[1]) == "decode") {
        std::string ramp = argc >= 4 ? argv[3] : "none";
        if (ramp.rfind("ramp=", 0) == 0) {
            ramp = ramp.substr(5);
        }
        return decode(argv[2], ramp);
    }
    std::fprintf(stderr,
                 "usage: %s plan NAME.request NAME.objects\n       %s decode NAME.thd "
                 "[ramp=none|end|start]\n",
                 argv[0], argv[0]);
    return 2;
}
