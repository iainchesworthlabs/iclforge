// iclforge_truehd_* - see iclforge.h's TrueHD section and iclforge::truehd::Decoder
// (libs/truehd/include/iclforge/truehd/decoder/decoder.hpp).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "internal.hpp"
#include "internal_truehd.hpp"

using iclforge_c::from_cpp;
using iclforge_c::guard;

namespace {

std::span<const std::byte> bytes_of(const std::uint8_t* data, std::size_t size) {
    return std::as_bytes(std::span<const std::uint8_t>(data, size));
}

// A string_view of a string literal (iclforge::truehd::Error::detail, channel_name()) as a C string;
// the empty view's data() may be NULL, so it is normalised to "".
const char* c_string(std::string_view literal) { return literal.empty() ? "" : literal.data(); }

}  // namespace

extern "C" {

void iclforge_truehd_decoder_config_init(iclforge_truehd_decoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const iclforge::truehd::DecoderConfig defaults{};
    *config = iclforge_truehd_decoder_config_t{
        .presentation = static_cast<iclforge_truehd_presentation_t>(defaults.presentation),
        .lenient = defaults.strictness == iclforge::truehd::Strictness::kLenient ? 1 : 0};
}

iclforge_status_t iclforge_truehd_probe(const uint8_t* data, size_t size,
                                        iclforge_truehd_stream_info_t* info) {
    if (data == nullptr || info == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&data, &size, &info]() -> iclforge_status_t {
        const auto result = iclforge::truehd::probe_stream(bytes_of(data, size));
        if (!result.has_value()) {
            return from_cpp(result.error().kind);
        }
        const iclforge::truehd::StreamInfo& found = *result;
        *info = iclforge_truehd_stream_info_t{};
        info->is_mlp = found.major_sync.kind == iclforge::truehd::StreamKind::kMlp ? 1 : 0;
        info->sample_rate = found.sample_rate;
        info->substreams = found.num_substreams;
        for (std::size_t i = 0; i < found.channels.size(); ++i) {
            info->channels[i] = found.channels[i];
        }
        info->samples_per_access_unit = found.samples_per_access_unit;
        info->access_units = found.access_units;
        info->variable_rate = found.major_sync.is_vbr ? 1 : 0;
        info->peak_data_rate_field = found.major_sync.peak_data_rate;
        info->has_object_substream = found.has_object_substream ? 1 : 0;
        info->first_unit_offset = found.first_unit_offset;
        return ICLFORGE_OK;
    });
}

iclforge_status_t iclforge_truehd_scan(const uint8_t* data, size_t size,
                                       iclforge_truehd_units_t** out_units) {
    if (data == nullptr || out_units == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&data, &size, &out_units]() -> iclforge_status_t {
        const std::span<const std::byte> all = bytes_of(data, size);
        iclforge::truehd::ElementaryReader reader(all);
        auto owned = std::make_unique<iclforge_truehd_units>();
        while (const auto unit = reader.next()) {
            owned->units.push_back(iclforge_truehd_units::Unit{
                .offset = static_cast<std::size_t>(unit->data() - all.data()),
                .size = unit->size()});
        }
        if (owned->units.empty()) {
            return ICLFORGE_ERROR_TRUEHD_BAD_SYNC;
        }
        owned->skipped_bytes = reader.skipped_bytes();
        *out_units = owned.release();
        return ICLFORGE_OK;
    });
}

void iclforge_truehd_units_destroy(iclforge_truehd_units_t* units) { delete units; }

size_t iclforge_truehd_units_count(const iclforge_truehd_units_t* units) {
    return units == nullptr ? 0 : units->units.size();
}

size_t iclforge_truehd_units_offset(const iclforge_truehd_units_t* units, size_t index) {
    return units == nullptr || index >= units->units.size() ? 0 : units->units[index].offset;
}

size_t iclforge_truehd_units_size(const iclforge_truehd_units_t* units, size_t index) {
    return units == nullptr || index >= units->units.size() ? 0 : units->units[index].size;
}

size_t iclforge_truehd_units_skipped_bytes(const iclforge_truehd_units_t* units) {
    return units == nullptr ? 0 : units->skipped_bytes;
}

iclforge_status_t iclforge_truehd_decoder_create(const iclforge_truehd_decoder_config_t* config,
                                                 iclforge_truehd_decoder_t** out_decoder) {
    if (config == nullptr || out_decoder == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    const int presentation = iclforge_c::stored_value(config->presentation);
    if (presentation < ICLFORGE_TRUEHD_PRESENTATION_BEST ||
        presentation > ICLFORGE_TRUEHD_PRESENTATION_ALL) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&config, &out_decoder, presentation] {
        *out_decoder = new iclforge_truehd_decoder(iclforge::truehd::DecoderConfig{
            .presentation = static_cast<iclforge::truehd::Presentation>(presentation),
            .strictness = config->lenient != 0 ? iclforge::truehd::Strictness::kLenient
                                               : iclforge::truehd::Strictness::kStrict});
        return ICLFORGE_OK;
    });
}

void iclforge_truehd_decoder_destroy(iclforge_truehd_decoder_t* decoder) { delete decoder; }

void iclforge_truehd_decoder_reset(iclforge_truehd_decoder_t* decoder) {
    if (decoder == nullptr) {
        return;
    }
    decoder->impl.reset();
    decoder->last_detail = {};
}

iclforge_status_t iclforge_truehd_decoder_decode(iclforge_truehd_decoder_t* decoder,
                                                 const uint8_t* access_unit, size_t size,
                                                 iclforge_truehd_decoded_t** out_decoded) {
    if (decoder == nullptr || access_unit == nullptr || out_decoded == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    decoder->last_detail = {};
    return guard([&decoder, &access_unit, &size, &out_decoded]() -> iclforge_status_t {
        const auto result = decoder->impl.decode(bytes_of(access_unit, size));
        if (!result.has_value()) {
            decoder->last_detail = result.error().detail;
            return from_cpp(result.error().kind);
        }
        if (!result->has_value()) {
            *out_decoded = nullptr;
            return ICLFORGE_OK;
        }
        const iclforge::truehd::DecodedAccessUnit& unit = **result;
        auto owned = std::make_unique<iclforge_truehd_decoded>();
        owned->sample_rate = unit.sample_rate;
        owned->channels = unit.channels;
        owned->samples = unit.samples;
        owned->substream = unit.substream;
        owned->restarted = unit.restarted;
        owned->padding_samples = unit.padding_samples;
        owned->pcm.assign(unit.pcm.begin(), unit.pcm.end());
        owned->layout.assign(unit.layout.begin(), unit.layout.end());
        owned->extra_data.reserve(unit.extra_data.size());
        for (const std::byte b : unit.extra_data) {
            owned->extra_data.push_back(std::to_integer<std::uint8_t>(b));
        }
        *out_decoded = owned.release();
        return ICLFORGE_OK;
    });
}

const char* iclforge_truehd_decoder_last_error_detail(const iclforge_truehd_decoder_t* decoder) {
    return decoder == nullptr ? "" : c_string(decoder->last_detail);
}

void iclforge_truehd_decoder_counters(const iclforge_truehd_decoder_t* decoder,
                                      iclforge_truehd_counters_t* out) {
    if (decoder == nullptr || out == nullptr) {
        return;
    }
    const iclforge::truehd::DecoderCounters& counters = decoder->impl.counters();
    *out = iclforge_truehd_counters_t{.units = counters.units,
                                      .units_decoded = counters.units_decoded,
                                      .units_waiting = counters.units_waiting,
                                      .restarts = counters.restarts,
                                      .lossless_checks_ok = counters.lossless_checks_ok,
                                      .lossless_check_failures = counters.lossless_check_failures,
                                      .check_failures = counters.check_failures};
}

void iclforge_truehd_decoded_destroy(iclforge_truehd_decoded_t* decoded) { delete decoded; }

uint32_t iclforge_truehd_decoded_sample_rate(const iclforge_truehd_decoded_t* decoded) {
    return decoded == nullptr ? 0 : decoded->sample_rate;
}

int iclforge_truehd_decoded_channels(const iclforge_truehd_decoded_t* decoded) {
    return decoded == nullptr ? 0 : decoded->channels;
}

int iclforge_truehd_decoded_samples(const iclforge_truehd_decoded_t* decoded) {
    return decoded == nullptr ? 0 : decoded->samples;
}

int iclforge_truehd_decoded_substream(const iclforge_truehd_decoded_t* decoded) {
    return decoded == nullptr ? 0 : decoded->substream;
}

int iclforge_truehd_decoded_restarted(const iclforge_truehd_decoded_t* decoded) {
    return decoded != nullptr && decoded->restarted ? 1 : 0;
}

int iclforge_truehd_decoded_padding_samples(const iclforge_truehd_decoded_t* decoded) {
    return decoded == nullptr ? 0 : decoded->padding_samples;
}

const int32_t* iclforge_truehd_decoded_pcm(const iclforge_truehd_decoded_t* decoded) {
    return decoded == nullptr ? nullptr : decoded->pcm.data();
}

const uint8_t* iclforge_truehd_decoded_extra_data(const iclforge_truehd_decoded_t* decoded,
                                                  size_t* out_size) {
    if (out_size != nullptr) {
        *out_size = decoded == nullptr ? 0 : decoded->extra_data.size();
    }
    return decoded == nullptr || decoded->extra_data.empty() ? nullptr
                                                              : decoded->extra_data.data();
}

const char* iclforge_truehd_decoded_channel_name(const iclforge_truehd_decoded_t* decoded,
                                                 size_t index) {
    if (decoded == nullptr || index >= decoded->layout.size()) {
        return "";
    }
    return c_string(iclforge::truehd::channel_name(decoded->layout[index]));
}

}  // extern "C"
