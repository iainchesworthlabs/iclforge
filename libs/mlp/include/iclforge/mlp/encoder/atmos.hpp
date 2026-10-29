#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include "iclforge/mlp/core/error.hpp"
#include "iclforge/mlp/encoder/encoder.hpp"
#include "iclforge/mlp/export.hpp"
#include "iclforge/objects/oamd.hpp"

// TrueHD with Dolby Atmos (truehd-spec.md section 15): a 7.1 bed and up to eight dynamic objects
// in one lossless stream of four substreams, with the objects' metadata (OAMD) in evolution
// payloads.
//
// The layout (planning/truehd.md, phase 4): channels 0 to 7 are the bed in 13.1's 7.1 order
// (L R C LFE Ls Rs Lrs Rrs) and channels 8 onwards the objects, in the 16-channel substream
// (restart sync 0x31EC). The lower presentations are what a decoder without Atmos plays: the
// 8-channel one is the bed with every object rendered into it (render_gains(), from the object's
// position and gain at that access unit), the 6-channel and 2-channel ones its fold-downs. The
// 16-channel substream's matrices undo the render exactly, so the 16-channel presentation gives
// back the bed and the objects as they were coded.
//
// The OAMD payload is iclforge::objects' serialisation of ETSI TS 103 420 object audio metadata
// (section 15 gives no bit syntax and points there): one bed instance of the 7.1 bed, the dynamic
// objects, one update per payload, sent in the access unit its state starts at and again at
// every restart so that a decoder joining there has it. Section 15.4's constraints hold by
// construction: the OAMD's object count is the 16-channel presentation's channel count, one bed
// instance, no intermediate spatial format, a constant bed distribution, and no timestamps.

namespace iclforge::mlp {

struct AtmosConfig {
    std::uint32_t sample_rate = 48000;  // 15.5: Atmos is 48 kHz only
    int objects = 8;                    // dynamic objects, 1 to 8
    int restart_interval = 16;
    std::uint64_t peak_data_rate = 18'000'000;
    bool verify = true;
};

// The gains with which an object at its position folds into the 7.1 bed's channels, in 13.1's
// order: a distance-weighted, power-normalised pan over the seven full-range speakers (the LFE
// takes none), height folded to the floor, scaled by the object's gain. Deterministic: the
// encoder's fold and any check of it agree.
[[nodiscard]] ICLFORGE_MLP_EXPORT std::array<double, 8> render_gains(
    const objects::oba::DynamicObject& object);

// The OAMD program of a 7.1 bed with `objects` dynamic objects.
[[nodiscard]] ICLFORGE_MLP_EXPORT objects::oba::Program atmos_program(int objects);

class ICLFORGE_MLP_EXPORT AtmosEncoder {
   public:
    [[nodiscard]] static std::expected<AtmosEncoder, Error> create(const AtmosConfig& config);
    ~AtmosEncoder();
    AtmosEncoder(AtmosEncoder&&) noexcept;
    AtmosEncoder& operator=(AtmosEncoder&&) noexcept;
    AtmosEncoder(const AtmosEncoder&) = delete;
    AtmosEncoder& operator=(const AtmosEncoder&) = delete;

    // Codes frames of the bed (8 channels, interleaved) and the objects (config.objects
    // channels, interleaved), the same number of frames each. `state`, when not empty, is the
    // objects' metadata from the first of these frames on (config.objects entries): it is sent as
    // an OAMD payload and folds the objects into the bed from that access unit. Returns the access
    // units completed so far.
    [[nodiscard]] std::expected<std::vector<std::byte>, Error> encode(
        std::span<const std::int32_t> bed, std::span<const std::int32_t> objects,
        std::span<const objects::oba::DynamicObject> state);

    [[nodiscard]] std::expected<std::vector<std::byte>, Error> flush();
    [[nodiscard]] const EncoderStats& stats() const noexcept;

   private:
    struct State;
    explicit AtmosEncoder(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

// A whole Atmos stream decoded: the bed, the objects and the metadata, each OAMD payload with the
// sample it took effect at.
struct AtmosMetadataFrame {
    std::uint64_t first_sample = 0;
    objects::oba::DecodedProgram metadata;
};
struct AtmosStream {
    std::uint32_t sample_rate = 0;
    int objects = 0;
    std::vector<std::int32_t> bed;         // 8 channels, interleaved, 24-bit scale
    std::vector<std::int32_t> object_pcm;  // `objects` channels, interleaved
    std::vector<AtmosMetadataFrame> metadata;
};

// Decodes the 16-channel presentation and splits it; fails with kInvalidConfiguration when the
// stream has no 16-channel presentation, and kExtraDataLength when an OAMD payload does not
// parse or names a channel count other than the presentation's (15.4).
[[nodiscard]] ICLFORGE_MLP_EXPORT std::expected<AtmosStream, Error> decode_atmos(
    std::span<const std::byte> stream);

}  // namespace iclforge::mlp
