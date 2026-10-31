// Embind wrapper around iclforge::truehd's decode side (libs/truehd): the probe, the scan into
// access units, the scan of a whole stream for its length and its joining points, and the decoder.
// The JavaScript counterpart of bindings/python's iclforge.truehd and bindings/rust's
// iclforge::truehd (WP7); bindings/js/src/truehd.ts is the typed wrapper over it. The encoder, the
// Atmos layer and the metadata edit are not here.
//
// Four JS-visible things:
//   - probe(bytes): wraps iclforge::truehd::probe_stream().
//   - scanUnits(bytes): wraps iclforge::truehd::ElementaryReader, the access units of a bare stream.
//   - TruehdScan: wraps iclforge::truehd::scan_stream() and the ScannedStream it returns.
//   - TruehdDecoder: wraps iclforge::truehd::Decoder.
//
// One convention crosses the boundary for a call that can fail: the library's std::expected is
// not turned into a JavaScript exception here, where an Embind throw reaches the page as a bare
// pointer number. A free function returns, instead of its result, `{ error, message, detail }`:
// the ErrorKind's number (its declaration order in core/error.hpp, which can be 0, so a caller
// asks whether the property is there and not whether it is truthy), to_string()'s text for it, and
// the library's detail text. The result of a success never has an `error` property. TruehdScan is
// built before it can say it failed, so it has errorKind() (-1 when it did not fail), message()
// and detail() like Ac4Encoder's constructionError(), and the decoder's decode() returns the same
// `{ error, message, detail }`. bindings/js/src/truehd.ts turns either into a TruehdError.
//
// Every number is a JS number: the stream's sample and unit counts and byte offsets are exact to
// 2^53, which a stream that fits in a 32-bit WebAssembly heap never approaches. Every array that
// leaves is a copy (the `new Int32Array(view)` idiom of ac4_bindings.cpp), so what JS holds stays
// valid across the next call; the decoder's own PCM buffer is reused by its next decode().

#include <cstddef>
#include <cstdint>
#include <expected>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/scan.hpp"

namespace {

namespace truehd = iclforge::truehd;

// --- Small shared helpers ---------------------------------------------------

// The bytes of a JS Uint8Array, copied out of the JS heap. The vector is the one place a call's
// input is held, so the library's spans stay valid for the whole call.
std::vector<std::byte> bytes_from_js(const emscripten::val& array) {
    const auto length = array["length"].as<std::size_t>();
    std::vector<std::byte> bytes(length);
    if (length != 0) {
        const emscripten::val view(emscripten::typed_memory_view(
            length, reinterpret_cast<std::uint8_t*>(bytes.data())));
        view.call<void>("set", array);
    }
    return bytes;
}

emscripten::val int32_array(std::span<const std::int32_t> values) {
    const emscripten::val view(emscripten::typed_memory_view(values.size(), values.data()));
    return emscripten::val::global("Int32Array").new_(view);
}

emscripten::val uint8_array(std::span<const std::byte> values) {
    const emscripten::val view(emscripten::typed_memory_view(
        values.size(), reinterpret_cast<const std::uint8_t*>(values.data())));
    return emscripten::val::global("Uint8Array").new_(view);
}

emscripten::val failure(const truehd::Error& error) {
    auto out = emscripten::val::object();
    out.set("error", static_cast<int>(error.kind));
    out.set("message", std::string(truehd::to_string(error.kind)));
    out.set("detail", std::string(error.detail));
    return out;
}

emscripten::val restart_point(const truehd::RestartPoint& point) {
    auto out = emscripten::val::object();
    out.set("unit", static_cast<double>(point.unit));
    out.set("offset", static_cast<double>(point.offset));
    return out;
}

emscripten::val channel_counts(const std::array<int, truehd::kMaxSubstreams>& channels) {
    auto out = emscripten::val::array();
    for (std::size_t i = 0; i < channels.size(); ++i) {
        out.set(static_cast<unsigned>(i), channels[i]);
    }
    return out;
}

// --- probe and the access units -----------------------------------------------

emscripten::val probe(const emscripten::val& data) {
    const std::vector<std::byte> bytes = bytes_from_js(data);
    const auto found = truehd::probe_stream(bytes);
    if (!found) {
        return failure(found.error());
    }
    auto out = emscripten::val::object();
    out.set("isMlp", found->major_sync.kind == truehd::StreamKind::kMlp);
    out.set("sampleRate", found->sample_rate);
    out.set("substreams", found->num_substreams);
    out.set("channels", channel_counts(found->channels));
    out.set("samplesPerAccessUnit", found->samples_per_access_unit);
    out.set("accessUnits", static_cast<double>(found->access_units));
    out.set("variableRate", found->major_sync.is_vbr);
    out.set("peakDataRateField", found->major_sync.peak_data_rate);
    out.set("hasObjectSubstream", found->has_object_substream);
    out.set("firstUnitOffset", static_cast<double>(found->first_unit_offset));
    return out;
}

// Each access unit as its offset and size in the data given, and the bytes that belonged to no
// unit; the wrapper makes the subarrays, so no unit's bytes are copied.
emscripten::val scan_units(const emscripten::val& data) {
    const std::vector<std::byte> bytes = bytes_from_js(data);
    truehd::ElementaryReader reader(bytes);
    auto units = emscripten::val::array();
    unsigned count = 0;
    while (const auto unit = reader.next()) {
        auto entry = emscripten::val::object();
        entry.set("offset", static_cast<double>(unit->data() - bytes.data()));
        entry.set("size", static_cast<double>(unit->size()));
        units.set(count++, entry);
    }
    if (count == 0) {
        return failure(truehd::Error{.kind = truehd::ErrorKind::kBadSync, .detail = {}});
    }
    auto out = emscripten::val::object();
    out.set("units", units);
    out.set("skippedBytes", static_cast<double>(reader.skipped_bytes()));
    return out;
}

// --- iclforge::truehd::ScannedStream ------------------------------------------

class TruehdScan {
public:
    TruehdScan(const emscripten::val& data, double restart_spacing) {
        const std::vector<std::byte> bytes = bytes_from_js(data);
        // A negative or NaN spacing is taken as 0, which scan_stream() takes as 1.
        const auto spacing = restart_spacing >= 1.0 ? static_cast<std::uint64_t>(restart_spacing) : 0U;
        auto scanned = truehd::scan_stream(bytes, truehd::ScanOptions{.restart_spacing = spacing});
        if (scanned) {
            stream_ = std::move(*scanned);
        } else {
            error_ = static_cast<int>(scanned.error().kind);
            message_ = std::string(truehd::to_string(scanned.error().kind));
            detail_ = std::string(scanned.error().detail);
        }
    }

    // The ErrorKind's number when the scan failed, -1 when it did not.
    int errorKind() const { return error_; }
    std::string message() const { return message_; }
    std::string detail() const { return detail_; }

    emscripten::val info() const {
        auto out = emscripten::val::object();
        if (!stream_) {
            return out;
        }
        const truehd::ScannedStream& s = *stream_;
        out.set("isMlp", s.kind == truehd::StreamKind::kMlp);
        out.set("sampleRate", s.sample_rate);
        out.set("samplesPerUnit", s.samples_per_unit);
        out.set("substreams", s.num_substreams);
        out.set("channels", channel_counts(s.channels));
        out.set("variableRate", s.variable_rate);
        out.set("peakDataRateField", s.peak_data_rate);
        out.set("hasObjectSubstream", s.has_object_substream);
        out.set("units", static_cast<double>(s.units));
        out.set("unitBytes", static_cast<double>(s.unit_bytes));
        out.set("skippedBytes", static_cast<double>(s.skipped_bytes));
        out.set("leadingBytes", static_cast<double>(s.leading_bytes));
        out.set("majorSyncs", static_cast<double>(s.major_syncs));
        out.set("paddingSamples", s.padding_samples);
        out.set("samples", static_cast<double>(s.samples()));
        out.set("durationSeconds", s.duration_seconds());
        return out;
    }

    emscripten::val restarts() const {
        auto out = emscripten::val::array();
        if (stream_) {
            unsigned index = 0;
            for (const truehd::RestartPoint& point : stream_->restarts) {
                out.set(index++, restart_point(point));
            }
        }
        return out;
    }

    // The restart a decode must start at to reach `sample`; null for a sample past the end.
    emscripten::val restartForSample(double sample) const {
        if (!stream_ || !(sample >= 0.0)) {
            return emscripten::val::null();
        }
        const auto found = stream_->restart_for_sample(static_cast<std::uint64_t>(sample));
        return found ? restart_point(*found) : emscripten::val::null();
    }

    double sampleAtSeconds(double seconds) const {
        return stream_ ? static_cast<double>(stream_->sample_at_seconds(seconds)) : 0.0;
    }

private:
    std::optional<truehd::ScannedStream> stream_;
    int error_ = -1;
    std::string message_;
    std::string detail_;
};

// --- iclforge::truehd::Decoder -------------------------------------------------

class TruehdDecoder {
public:
    // `presentation` is iclforge::truehd::Presentation's own number (0 best, 1 2ch, 2 6ch, 3 8ch,
    // 4 all); a number outside them is the default, as the C API's config init does.
    TruehdDecoder(int presentation, bool lenient)
        : decoder_(truehd::DecoderConfig{
              .presentation = presentation >= 0 && presentation <= 4
                                  ? static_cast<truehd::Presentation>(presentation)
                                  : truehd::Presentation::kBest,
              .strictness = lenient ? truehd::Strictness::kLenient : truehd::Strictness::kStrict}) {}

    // One whole access unit. Null for a unit that gave no samples (no restart header has come yet);
    // `{ error, message, detail }` for one that failed; otherwise the decoded unit.
    emscripten::val decode(const emscripten::val& access_unit) {
        const std::vector<std::byte> bytes = bytes_from_js(access_unit);
        const auto result = decoder_.decode(bytes);
        if (!result) {
            return failure(result.error());
        }
        if (!result->has_value()) {
            return emscripten::val::null();
        }
        const truehd::DecodedAccessUnit& unit = **result;
        auto out = emscripten::val::object();
        out.set("sampleRate", unit.sample_rate);
        out.set("channels", unit.channels);
        out.set("samples", unit.samples);
        out.set("substream", unit.substream);
        out.set("restarted", unit.restarted);
        out.set("paddingSamples", unit.padding_samples);
        out.set("pcm", int32_array(unit.pcm));
        out.set("extraData", uint8_array(unit.extra_data));
        auto layout = emscripten::val::array();
        unsigned index = 0;
        for (const truehd::Channel channel : unit.layout) {
            layout.set(index++, std::string(truehd::channel_name(channel)));
        }
        out.set("layout", layout);
        return out;
    }

    void lostData() { decoder_.lost_data(); }

    void reset() { decoder_.reset(); }

    emscripten::val counters() const {
        const truehd::DecoderCounters& c = decoder_.counters();
        auto out = emscripten::val::object();
        out.set("units", static_cast<double>(c.units));
        out.set("unitsDecoded", static_cast<double>(c.units_decoded));
        out.set("unitsWaiting", static_cast<double>(c.units_waiting));
        out.set("restarts", static_cast<double>(c.restarts));
        out.set("losslessChecksOk", static_cast<double>(c.lossless_checks_ok));
        out.set("losslessCheckFailures", static_cast<double>(c.lossless_check_failures));
        out.set("checkFailures", static_cast<double>(c.check_failures));
        out.set("matrixHeaders", static_cast<double>(c.matrix_headers));
        out.set("lsbBypassMatrices", static_cast<double>(c.lsb_bypass_matrices));
        return out;
    }

private:
    truehd::Decoder decoder_;
};

}  // namespace

EMSCRIPTEN_BINDINGS(iclforge_wasm_truehd) {
    emscripten::function("probe", &probe);
    emscripten::function("scanUnits", &scan_units);

    emscripten::class_<TruehdScan>("TruehdScan")
        .constructor<emscripten::val, double>()
        .function("errorKind", &TruehdScan::errorKind)
        .function("message", &TruehdScan::message)
        .function("detail", &TruehdScan::detail)
        .function("info", &TruehdScan::info)
        .function("restarts", &TruehdScan::restarts)
        .function("restartForSample", &TruehdScan::restartForSample)
        .function("sampleAtSeconds", &TruehdScan::sampleAtSeconds);

    emscripten::class_<TruehdDecoder>("TruehdDecoder")
        .constructor<int, bool>()
        .function("decode", &TruehdDecoder::decode)
        .function("lostData", &TruehdDecoder::lostData)
        .function("reset", &TruehdDecoder::reset)
        .function("counters", &TruehdDecoder::counters);
}
