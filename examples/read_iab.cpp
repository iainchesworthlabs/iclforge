// Parses the same Immersive Audio Bitstream (SMPTE ST 2098-2:2022) content two ways: once as a
// bare elementary `.iab` file (iclforge::iab::parse_iabitstream) and once wrapped as an IMF IAB
// Track File (iclforge::iab::write_mxf_iab, then iclforge::iab::parse_mxf_iab), printing what each
// found to show the two agree - the point being that SMPTE ST 2067-201 clip-wraps the whole
// IABitstream as a single Generic Container KLV Value, so a Track File's essence really is the
// identical byte sequence an elementary `.iab` file already has (see src/iab/src/mxf_reader.cpp's
// own header comment for the full citation trail).
//
// iclforge::iab is codec-blind - this program does not either, it only proves both parsed graphs
// are navigable and agree. A real IAB Track File is a production Dolby Atmos cinema/IMF master
// this project has no license to embed, so - like examples/read_adm.cpp for its own container -
// this writes its own tiny-but-valid fixtures to temp files first. The elementary stream is
// laid out byte by byte from the syntax tables; the Track File is written by write_mxf_iab()
// from the frames that stream parses to, with the bit depth set to the 24 bits ST 2067-201 5.6.2
// requires.

#include <fmt/printf.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/iab/ac3iab.hpp"
#include "iclforge/iab/mxf.hpp"
#include "iclforge/iab/writer.hpp"

namespace {

std::string scratch_path(std::string_view name) {
    static const std::string run = std::to_string(
        (static_cast<std::uint64_t>(std::random_device{}()) << 32) ^
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string leaf = "iclforge_" + run + "_" + std::string(name);
    return (std::filesystem::temp_directory_path() / leaf).string();
}

void put_u8(std::vector<std::byte>& out, std::uint8_t v) {
    out.push_back(static_cast<std::byte>(v));
}

// SMPTE ST 2098-2:2022 §7 Table 2 / §8: a two-frame elementary IABitstream - each frame a
// Preamble (empty) plus one IAFrame segment wrapping the smallest legal IaFrame (§10.2: Version 1,
// 48 kHz, 16-bit, FrameRate code 0x3 [48 fps], no Beds/Objects/essence). Two frames, not one, so
// this program's own output shows a real multi-frame count rather than the degenerate case of 1.
std::vector<std::byte> build_elementary_iabitstream(unsigned frame_count) {
    std::vector<std::byte> out;
    for (unsigned i = 0; i < frame_count; ++i) {
        put_u8(out, 0x01);  // PreambleTag
        put_u8(out, 0x00);  // PreambleLength BE32 = 0
        put_u8(out, 0x00);
        put_u8(out, 0x00);
        put_u8(out, 0x00);
        put_u8(out, 0x02);  // IAFrameTag
        put_u8(out, 0x00);  // IAFrameLength BE32 = 6 (ElementID+ElementSize+4-byte payload)
        put_u8(out, 0x00);
        put_u8(out, 0x00);
        put_u8(out, 0x06);
        put_u8(out, 0x08);  // ElementID Plex(8) = 0x08 (IA_FRAME, §10.1.1 Table 14)
        put_u8(out, 0x04);  // ElementSize Plex(8) = 4
        put_u8(out, 0x01);  // Version = 1
        put_u8(out, 0x03);  // SampleRate=0 (48 kHz), BitDepth=0 (16-bit), FrameRate=0x3 (48 fps)
        put_u8(out, 0x00);  // MaxRendered Plex(8) = 0
        put_u8(out, 0x00);  // SubElementCount Plex(8) = 0
    }
    return out;
}

bool write_file(const std::string& path, const std::vector<std::byte>& bytes) {
    // path is always a scratch_path() result (never a caller-chosen path), and the system temp
    // directory is shared/world-writable - noreplace fails instead of writing through a symlink
    // another local user pre-planted at this exact (astronomically unlikely to guess) name.
    std::ofstream out(path, std::ios::binary | std::ios::noreplace);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

}  // namespace

int main() {
    const auto iabitstream = build_elementary_iabitstream(2);

    const auto elementary_path = scratch_path("read_iab_elementary.iab");
    const auto mxf_path = scratch_path("read_iab_wrapped.mxf");
    if (!write_file(elementary_path, iabitstream)) {
        fmt::printf("could not write the elementary fixture\n");
        return 1;
    }

    const auto elementary = iclforge::iab::parse_iabitstream(elementary_path);
    std::filesystem::remove(elementary_path);
    if (!elementary) {
        fmt::printf("parse_iabitstream failed: %.*s\n",
                    static_cast<int>(iclforge::iab::describe(elementary.error()).size()),
                    iclforge::iab::describe(elementary.error()).data());
        return 1;
    }

    // The Track File carries the same frames at 24 bits (ST 2067-201 5.6.2). write_mxf_iab()
    // refuses a bitstream the standard forbids, so this is checked, not assumed.
    auto track_file_frames = *elementary;
    for (auto& frame : track_file_frames) {
        frame.frame.bit_depth = 24;
    }
    if (const auto written = iclforge::iab::write_mxf_iab(mxf_path, track_file_frames); !written) {
        fmt::printf("write_mxf_iab failed: %.*s\n",
                    static_cast<int>(iclforge::iab::describe(written.error()).size()),
                    iclforge::iab::describe(written.error()).data());
        return 1;
    }
    const auto mxf = iclforge::iab::parse_mxf_iab(mxf_path);
    std::filesystem::remove(mxf_path);

    if (!mxf) {
        fmt::printf("parse_mxf_iab failed: %.*s\n",
                    static_cast<int>(iclforge::iab::describe(mxf.error()).size()),
                    iclforge::iab::describe(mxf.error()).data());
        return 1;
    }

    fmt::printf("elementary .iab: %zu frame(s)\n", elementary->size());
    fmt::printf("MXF track file:  %zu frame(s)\n", mxf->size());
    if (elementary->size() != mxf->size()) {
        fmt::printf("frame counts disagree\n");
        return 1;
    }

    for (std::size_t i = 0; i < elementary->size(); ++i) {
        const auto& a = (*elementary)[i].frame;
        const auto& b = (*mxf)[i].frame;
        fmt::printf("  frame %zu: elementary %u Hz/%u-bit, MXF %u Hz/%u-bit\n", i, a.sample_rate,
                    a.bit_depth, b.sample_rate, b.bit_depth);
        if (a.sample_rate != b.sample_rate || b.bit_depth != 24) {
            fmt::printf("frame %zu disagrees between the two containers\n", i);
            return 1;
        }
    }

    fmt::printf("both containers agree\n");
    return 0;
}
