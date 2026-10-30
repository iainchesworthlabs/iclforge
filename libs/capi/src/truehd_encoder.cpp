// iclforge_truehd_encoder_* - see iclforge.h's TrueHD section and iclforge::truehd::Encoder
// (libs/truehd/include/iclforge/truehd/encoder/encoder.hpp).

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#include "internal.hpp"
#include "internal_truehd.hpp"

using iclforge_c::c_string;
using iclforge_c::from_cpp;
using iclforge_c::guard;

namespace {

// The enumerator is read as the int a C caller stored (internal.hpp's stored_value): the C++ enum
// is not asked to hold a value it does not name.
bool known_downmix(const iclforge_truehd_encoder_config_t& config) {
    const int mode = iclforge_c::stored_value(config.downmix);
    return mode == ICLFORGE_TRUEHD_DOWNMIX_NONE || mode == ICLFORGE_TRUEHD_DOWNMIX_STANDARD;
}

iclforge::truehd::EncoderConfig to_cpp(const iclforge_truehd_encoder_config_t& config) {
    iclforge::truehd::EncoderConfig out;
    out.sample_rate = config.sample_rate;
    out.channels = config.channels;
    out.bits = config.bits;
    out.channel_assignment = config.channel_assignment;
    out.restart_interval_units = config.restart_interval_units;
    out.peak_data_rate_bps = config.peak_data_rate_bps;
    out.allow_rate_violations = config.allow_rate_violations != 0;
    out.downmix = iclforge_c::stored_value(config.downmix) == ICLFORGE_TRUEHD_DOWNMIX_STANDARD
                      ? iclforge::truehd::DownmixMode::kStandard
                      : iclforge::truehd::DownmixMode::kNone;
    out.downmix_gains =
        iclforge::truehd::DownmixGains{.centre = config.downmix_centre,
                                       .surround = config.downmix_surround,
                                       .lfe = config.downmix_lfe,
                                       .rear_to_surround = config.downmix_rear_to_surround};
    out.stereo_mode = config.stereo_mode;
    out.max_fir_order = config.max_fir_order;
    out.matrix_search = config.matrix_search != 0;
    out.verify = config.verify != 0;
    return out;
}

// The access units of one call as a handle, or none when it completed none.
iclforge_status_t hand_over(std::vector<iclforge::truehd::EncodedAccessUnit>&& units,
                            iclforge_truehd_encoded_t** out_encoded) {
    if (units.empty()) {
        *out_encoded = nullptr;
        return ICLFORGE_OK;
    }
    auto owned = std::make_unique<iclforge_truehd_encoded>();
    owned->units = std::move(units);
    *out_encoded = owned.release();
    return ICLFORGE_OK;
}

const iclforge::truehd::EncodedAccessUnit* unit_at(const iclforge_truehd_encoded_t* encoded,
                                                   size_t index) {
    return encoded == nullptr || index >= encoded->units.size() ? nullptr : &encoded->units[index];
}

}  // namespace

extern "C" {

void iclforge_truehd_encoder_config_init(iclforge_truehd_encoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const iclforge::truehd::EncoderConfig defaults{};
    *config = iclforge_truehd_encoder_config_t{
        .sample_rate = defaults.sample_rate,
        .channels = defaults.channels,
        .bits = defaults.bits,
        .channel_assignment = defaults.channel_assignment,
        .restart_interval_units = defaults.restart_interval_units,
        .peak_data_rate_bps = defaults.peak_data_rate_bps,
        .allow_rate_violations = defaults.allow_rate_violations ? 1 : 0,
        .downmix = static_cast<iclforge_truehd_downmix_t>(defaults.downmix),
        .downmix_centre = defaults.downmix_gains.centre,
        .downmix_surround = defaults.downmix_gains.surround,
        .downmix_lfe = defaults.downmix_gains.lfe,
        .downmix_rear_to_surround = defaults.downmix_gains.rear_to_surround,
        .stereo_mode = defaults.stereo_mode,
        .max_fir_order = defaults.max_fir_order,
        .matrix_search = defaults.matrix_search ? 1 : 0,
        .verify = defaults.verify ? 1 : 0};
}

uint32_t iclforge_truehd_default_channel_assignment(int channels) {
    return iclforge::truehd::default_channel_assignment(channels);
}

const char* iclforge_truehd_encoder_refusal_reason(const iclforge_truehd_encoder_config_t* config) {
    if (config == nullptr) {
        return "";
    }
    if (!known_downmix(*config)) {
        return "the downmix is not one of ICLFORGE_TRUEHD_DOWNMIX_NONE and _STANDARD";
    }
    return c_string(iclforge::truehd::Encoder::refusal_reason(to_cpp(*config)));
}

iclforge_status_t iclforge_truehd_encoder_create(const iclforge_truehd_encoder_config_t* config,
                                                 iclforge_truehd_encoder_t** out_encoder) {
    if (config == nullptr || out_encoder == nullptr || !known_downmix(*config)) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&config, &out_encoder]() -> iclforge_status_t {
        auto encoder = iclforge::truehd::Encoder::create(to_cpp(*config));
        if (!encoder.has_value()) {
            return from_cpp(encoder.error().kind);
        }
        auto owned =
            std::make_unique<iclforge_truehd_encoder>(std::move(*encoder), config->channels);
        *out_encoder = owned.release();
        return ICLFORGE_OK;
    });
}

void iclforge_truehd_encoder_destroy(iclforge_truehd_encoder_t* encoder) {
    delete encoder;
}

iclforge_status_t iclforge_truehd_encoder_encode(iclforge_truehd_encoder_t* encoder,
                                                 const int32_t* pcm, size_t frames,
                                                 iclforge_truehd_encoded_t** out_encoded) {
    if (encoder == nullptr || out_encoded == nullptr || (pcm == nullptr && frames != 0)) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    const auto channels = static_cast<size_t>(encoder->channels);
    if (channels != 0 && frames > std::numeric_limits<size_t>::max() / channels) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    encoder->last_detail = {};
    return guard([&encoder, &pcm, &frames, &channels, &out_encoded]() -> iclforge_status_t {
        auto result = encoder->impl.encode(std::span<const std::int32_t>(pcm, frames * channels));
        if (!result.has_value()) {
            encoder->last_detail = result.error().detail;
            return from_cpp(result.error().kind);
        }
        return hand_over(std::move(*result), out_encoded);
    });
}

iclforge_status_t iclforge_truehd_encoder_flush(iclforge_truehd_encoder_t* encoder,
                                                iclforge_truehd_encoded_t** out_encoded) {
    if (encoder == nullptr || out_encoded == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    encoder->last_detail = {};
    return guard([&encoder, &out_encoded]() -> iclforge_status_t {
        auto result = encoder->impl.flush();
        if (!result.has_value()) {
            encoder->last_detail = result.error().detail;
            return from_cpp(result.error().kind);
        }
        return hand_over(std::move(*result), out_encoded);
    });
}

const char* iclforge_truehd_encoder_last_error_detail(const iclforge_truehd_encoder_t* encoder) {
    return encoder == nullptr ? "" : c_string(encoder->last_detail);
}

int iclforge_truehd_encoder_samples_per_access_unit(const iclforge_truehd_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.samples_per_access_unit();
}

int iclforge_truehd_encoder_substreams(const iclforge_truehd_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.substreams();
}

void iclforge_truehd_encoder_stats(const iclforge_truehd_encoder_t* encoder,
                                   iclforge_truehd_encoder_stats_t* out) {
    if (encoder == nullptr || out == nullptr) {
        return;
    }
    const iclforge::truehd::EncoderStats& stats = encoder->impl.stats();
    *out = iclforge_truehd_encoder_stats_t{
        .access_units = stats.access_units,
        .restart_intervals = stats.restart_intervals,
        .input_samples = stats.input_samples,
        .bytes = stats.bytes,
        .rate_violations = stats.rate_violations,
        .max_unit_bytes = stats.max_unit_bytes,
        .max_unit_rate_bps = stats.max_unit_rate_bps,
        .matrices_used = stats.matrices_used,
        .downmix_reduced_intervals = stats.downmix_reduced_intervals,
        .verified_units = stats.verified_units};
}

void iclforge_truehd_encoded_destroy(iclforge_truehd_encoded_t* encoded) {
    delete encoded;
}

size_t iclforge_truehd_encoded_count(const iclforge_truehd_encoded_t* encoded) {
    return encoded == nullptr ? 0 : encoded->units.size();
}

const uint8_t* iclforge_truehd_encoded_data(const iclforge_truehd_encoded_t* encoded,
                                            size_t index) {
    const auto* unit = unit_at(encoded, index);
    return unit == nullptr ? nullptr : reinterpret_cast<const uint8_t*>(unit->bytes.data());
}

size_t iclforge_truehd_encoded_size(const iclforge_truehd_encoded_t* encoded, size_t index) {
    const auto* unit = unit_at(encoded, index);
    return unit == nullptr ? 0 : unit->bytes.size();
}

int iclforge_truehd_encoded_samples(const iclforge_truehd_encoded_t* encoded, size_t index) {
    const auto* unit = unit_at(encoded, index);
    return unit == nullptr ? 0 : unit->samples;
}

int iclforge_truehd_encoded_padding_samples(const iclforge_truehd_encoded_t* encoded,
                                            size_t index) {
    const auto* unit = unit_at(encoded, index);
    return unit == nullptr ? 0 : unit->padding_samples;
}

int iclforge_truehd_encoded_has_major_sync(const iclforge_truehd_encoded_t* encoded, size_t index) {
    const auto* unit = unit_at(encoded, index);
    return unit != nullptr && unit->has_major_sync ? 1 : 0;
}

}  // extern "C"
