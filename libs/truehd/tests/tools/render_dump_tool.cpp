// iclforge-truehd-render-dump: what a TrueHD Atmos stream holds, laid out for a check of the render
// rule against it (libs/truehd/notes/feature-truehd-measured-bed-render.md).
//
//   iclforge-truehd-render-dump STREAM OUT_PREFIX
//
// Writes OUT_PREFIX.bed.i32 (the 7.1 bed as substreams 0 to 2 decode it, 8 channels interleaved, one
// 32 bit little-endian value per sample), OUT_PREFIX.elem.i32 (the object substream's channels, the
// stream's elements, interleaved) and OUT_PREFIX.pos.csv (the object audio metadata of every
// evolution frame: access unit, block start, ramp and each dynamic object's place and gain). Reads
// the stream the way a player does; nothing is written back into it.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "iclforge/objects/oamd.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/evolution.hpp"

namespace {

using namespace iclforge::truehd;

void write_samples(std::ofstream& out, std::span<const std::int32_t> pcm) {
    out.write(reinterpret_cast<const char*>(pcm.data()),
              static_cast<std::streamsize>(pcm.size() * sizeof(std::int32_t)));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s STREAM OUT_PREFIX\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> stream(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        stream[i] = static_cast<std::byte>(raw[i]);
    }
    const std::string prefix = argv[2];
    std::ofstream bed_out(prefix + ".bed.i32", std::ios::binary);
    std::ofstream elem_out(prefix + ".elem.i32", std::ios::binary);
    std::ofstream pos(prefix + ".pos.csv");
    pos << "au,block_start,sample_offset,factor,ramp,object,x,y,z,gain_db,size_x,size_y,size_z\n";

    DecoderConfig bed_config;
    bed_config.presentation = Presentation::k8ch;
    DecoderConfig all_config;
    all_config.presentation = Presentation::kAll;
    Decoder bed_decoder(bed_config);
    Decoder all_decoder(all_config);
    ElementaryReader reader(stream);
    int substreams = 0;
    std::uint64_t au = 0;
    std::uint64_t bed_samples = 0;
    std::uint64_t elem_samples = 0;
    int elem_channels = 0;
    while (const auto unit = reader.next()) {
        const auto bed = bed_decoder.decode(*unit);
        if (bed && *bed) {
            write_samples(bed_out, (*bed)->pcm);
            bed_samples += static_cast<std::uint64_t>((*bed)->samples);
        }
        const auto all = all_decoder.decode(*unit);
        if (all && *all) {
            write_samples(elem_out, (*all)->pcm);
            elem_samples += static_cast<std::uint64_t>((*all)->samples);
            elem_channels = (*all)->channels;
        }
        const auto parsed = parse_access_unit(*unit, substreams);
        if (parsed) {
            if (parsed->major_sync) {
                substreams = parsed->num_substreams;
            }
            if (parsed->extra_data) {
                const EvolutionRead read = read_evolution(*parsed->extra_data);
                if (read.container) {
                    const auto payload = read.container->oamd();
                    if (!payload.empty()) {
                        if (const auto program = iclforge::objects::oba::parse_payload(payload)) {
                            for (const auto& block : program->blocks) {
                                const int start =
                                    block.sample_offset + 32 * block.block_offset_factor;
                                for (std::size_t o = 0; o < block.objects.size(); ++o) {
                                    const auto& d = block.objects[o];
                                    pos << au << ',' << start << ',' << block.sample_offset << ','
                                        << block.block_offset_factor << ',' << block.ramp_duration
                                        << ',' << o << ',' << d.position.x << ',' << d.position.y
                                        << ',' << d.position.z << ',' << d.gain_db << ','
                                        << d.size.width << ',' << d.size.depth << ','
                                        << d.size.height << '\n';
                                }
                            }
                        }
                    }
                }
            }
        }
        ++au;
    }
    std::printf("units %llu bed samples %llu element samples %llu channels %d\n",
                static_cast<unsigned long long>(au), static_cast<unsigned long long>(bed_samples),
                static_cast<unsigned long long>(elem_samples), elem_channels);
    return 0;
}
