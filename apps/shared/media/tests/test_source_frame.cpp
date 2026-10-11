#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <vector>

#include "source_frame.hpp"

// apps/shared/media/src/source_frame.hpp: what a source supplies to an encoder
// past the end of its audio, and ahead of its start. forge encode and the Forge
// GUI both fill every frame they hand an encoder through it, so these cases are
// the one place the policy is held: the end of a source holds its last real
// sample and is not silence (commit e89f5f6c7 measured why: a drop to zero at
// the end of a file is a transient the encoder spends a block switch on, and the
// last frame's SNR fell from 67 to 88 dB to 23 to 28 dB).

using iclforge::apps::fill_source_frame;

namespace {

constexpr std::array<float, 5> kSource{0.1F, -0.2F, 0.3F, -0.4F, 0.5F};

std::vector<float> frame_of(std::size_t offset, std::size_t start, std::size_t length,
                            const std::vector<float>& channel = {kSource.begin(), kSource.end()}) {
    std::vector<float> out(length, -9.0F);
    fill_source_frame(channel, offset, start, out);
    return out;
}

}  // namespace

TEST_CASE("source frame: a frame inside the source is the source's own samples", "[source-frame]") {
    CHECK(frame_of(0, 0, 3) == std::vector<float>{0.1F, -0.2F, 0.3F});
    CHECK(frame_of(0, 2, 3) == std::vector<float>{0.3F, -0.4F, 0.5F});
}

TEST_CASE("source frame: past the end the last real sample is held, not silence", "[source-frame]") {
    // The frame that ends mid-buffer: two real samples, then the last one repeated.
    CHECK(frame_of(0, 3, 5) == std::vector<float>{-0.4F, 0.5F, 0.5F, 0.5F, 0.5F});
    // A frame wholly past the end is all of it.
    CHECK(frame_of(0, 8, 4) == std::vector<float>{0.5F, 0.5F, 0.5F, 0.5F});
}

TEST_CASE("source frame: ahead of an offset is silence, and the hold starts at the source's own end",
          "[source-frame]") {
    // offset 2: programme samples 0 and 1 are silence, 2 to 6 are the source, 7 on holds.
    CHECK(frame_of(2, 0, 9) ==
          std::vector<float>{0.0F, 0.0F, 0.1F, -0.2F, 0.3F, -0.4F, 0.5F, 0.5F, 0.5F});
    // A frame that starts inside the silence and one that is wholly inside it.
    CHECK(frame_of(4, 1, 4) == std::vector<float>{0.0F, 0.0F, 0.0F, 0.1F});
    CHECK(frame_of(4, 0, 3) == std::vector<float>{0.0F, 0.0F, 0.0F});
}

TEST_CASE("source frame: a source whose last sample is silence holds silence", "[source-frame]") {
    const std::vector<float> fade{0.4F, 0.2F, 0.0F};
    CHECK(frame_of(0, 1, 4, fade) == std::vector<float>{0.2F, 0.0F, 0.0F, 0.0F});
}

TEST_CASE("source frame: an empty source supplies silence", "[source-frame]") {
    const std::vector<float> none;
    CHECK(frame_of(0, 0, 4, none) == std::vector<float>(4, 0.0F));
    CHECK(frame_of(3, 0, 4, none) == std::vector<float>(4, 0.0F));
}

TEST_CASE("source frame: successive frames tile the programme without a seam", "[source-frame]") {
    // Three frames of four read one after another are the same twelve samples as one frame of twelve.
    std::vector<float> joined;
    for (std::size_t start = 0; start < 12; start += 4) {
        const auto part = frame_of(1, start, 4);
        joined.insert(joined.end(), part.begin(), part.end());
    }
    CHECK(joined == frame_of(1, 0, 12));
}
