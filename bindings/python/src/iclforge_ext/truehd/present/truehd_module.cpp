#include "optional_modules.hpp"

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/scan.hpp"

#include "binding_support.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The variant of the `iclforge.truehd` submodule compiled when iclforge::truehd is in this build.
//
// pybind11-direct on iclforge::truehd's Decoder, ElementaryReader and scan_stream, the same policy
// as the rest of this extension (see bindings.cpp's own header comment): no intermediate C API. The
// surface is the one the C API has for TrueHD and MLP (docs/library/c-api.md) in Python's shape:
//
//   probe(data)                 what the first major sync and the restart headers after it say
//   access_units(data)          the units of a bare stream, as bytes, and what was skipped
//   scan(data, restart_spacing) the stream's length, timing and the units a decoder can join at
//   decode(data, ...)           a whole bare stream to PCM
//   Decoder(...)                a unit at a time, for a caller that has the units already
//
// The codec is lossless, so PCM is integers: int32 arrays of shape (samples, channels), each value
// 24 bits wide and sign-extended (a 16-bit stream's values are multiples of 256). That is the
// decoder's own interleaved order, and the channel order of the stream, which `speakers` names.
//
// Every failure raises TruehdError, a ValueError (as Ac4Error is), carrying the library's error
// kind as `.kind`.
//
// See optional_modules.hpp for why this is a translation unit rather than an #ifdef, and
// bindings/python/CMakeLists.txt for the selection that picks this file over the absent/ one
// beside it.

namespace {

namespace py = pybind11;
namespace th = iclforge::truehd;

// Thrown where a std::expected's error branch is, and turned into TruehdError (with `.kind` set)
// by the translator register_truehd() installs.
struct TruehdFailure : std::runtime_error {
    explicit TruehdFailure(const th::Error& error)
        : std::runtime_error(std::string(th::to_string(error.kind)) +
                             (error.detail.empty() ? std::string{} : ": " + std::string(error.detail))),
          kind(error.kind) {}
    th::ErrorKind kind;
};

template <typename T>
T value_or_throw(std::expected<T, th::Error> result) {
    if (!result) {
        throw TruehdFailure(result.error());
    }
    return std::move(*result);
}

[[nodiscard]] std::vector<std::byte> bytes_of(const py::buffer& buffer) {
    return iclforge::python::detail::to_bytes(buffer);
}

[[nodiscard]] std::string kind_name(th::StreamKind kind) {
    return kind == th::StreamKind::kMlp ? "mlp" : "truehd";
}

// Interleaved PCM as an owning (samples, channels) array: the decode's views end at the decoder's
// next call, so a Python object keeps a copy.
[[nodiscard]] py::array_t<std::int32_t> pcm_array(const std::vector<std::int32_t>& pcm,
                                                  std::size_t channels) {
    const std::size_t frames = channels == 0 ? 0 : pcm.size() / channels;
    py::array_t<std::int32_t> out({static_cast<py::ssize_t>(frames), static_cast<py::ssize_t>(channels)});
    if (!pcm.empty()) {
        std::memcpy(out.mutable_data(), pcm.data(), pcm.size() * sizeof(std::int32_t));
    }
    return out;
}

[[nodiscard]] std::vector<std::string> speaker_names(std::span<const th::Channel> layout) {
    std::vector<std::string> names;
    names.reserve(layout.size());
    for (const th::Channel channel : layout) {
        names.emplace_back(th::channel_name(channel));
    }
    return names;
}

struct StreamInfoView {
    std::string kind;
    std::uint32_t sample_rate = 0;
    int substreams = 0;
    std::vector<int> channels;
    int samples_per_access_unit = 0;
    std::size_t access_units = 0;
    bool variable_rate = false;
    int peak_data_rate_field = 0;
    bool has_object_substream = false;
    std::size_t first_unit_offset = 0;
};

struct DecodedUnitView {
    std::uint32_t sample_rate = 0;
    int channels = 0;
    int samples = 0;
    int substream = 0;
    bool restarted = false;
    int padding_samples = 0;
    std::vector<std::int32_t> pcm;
    std::vector<std::string> speakers;
};

struct DecodedStreamView {
    std::uint32_t sample_rate = 0;
    int channels = 0;
    std::vector<std::int32_t> pcm;
    std::vector<std::string> speakers;
    std::size_t units = 0;
    std::size_t skipped_bytes = 0;
    std::size_t leading_bytes = 0;
};

th::DecoderConfig make_config(th::Presentation presentation, bool lenient) {
    th::DecoderConfig config;
    config.presentation = presentation;
    config.strictness = lenient ? th::Strictness::kLenient : th::Strictness::kStrict;
    return config;
}

}  // namespace

namespace iclforge::python {

void register_truehd(py::module_& parent) {
    py::module_ m = parent.def_submodule(
        "truehd", "Dolby TrueHD and MLP: probe, scan, seek and lossless decode (iclforge::truehd).");

    // Defined on the extension module itself, beside Ac3Error and Ac4Error (iclforge/__init__.py
    // re-exports it), and reachable as iclforge.truehd.TruehdError too.
    static py::exception<std::runtime_error> truehd_error(parent, "TruehdError", PyExc_ValueError);
    m.attr("TruehdError") = truehd_error;
    py::register_exception_translator([](std::exception_ptr p) {
        if (!p) {
            return;
        }
        try {
            std::rethrow_exception(p);
        } catch (const TruehdFailure& e) {
            py::object exc = py::reinterpret_steal<py::object>(
                PyObject_CallFunction(truehd_error.ptr(), "s", e.what()));
            exc.attr("kind") = py::cast(e.kind);
            PyErr_SetObject(truehd_error.ptr(), exc.ptr());
        }
    });

    py::enum_<th::ErrorKind>(m, "ErrorKind", "Why a read or a decode stopped; TruehdError.kind.")
        .value("kTruncated", th::ErrorKind::kTruncated)
        .value("kBadSync", th::ErrorKind::kBadSync)
        .value("kBadCheckNibble", th::ErrorKind::kBadCheckNibble)
        .value("kBadLength", th::ErrorKind::kBadLength)
        .value("kBadSignature", th::ErrorKind::kBadSignature)
        .value("kBadMajorSyncCrc", th::ErrorKind::kBadMajorSyncCrc)
        .value("kBadRestartSync", th::ErrorKind::kBadRestartSync)
        .value("kBadRestartCrc", th::ErrorKind::kBadRestartCrc)
        .value("kBadParity", th::ErrorKind::kBadParity)
        .value("kBadSubstreamCrc", th::ErrorKind::kBadSubstreamCrc)
        .value("kBadParameter", th::ErrorKind::kBadParameter)
        .value("kBadBlockData", th::ErrorKind::kBadBlockData)
        .value("kBadLosslessCheck", th::ErrorKind::kBadLosslessCheck)
        .value("kNeedsRestart", th::ErrorKind::kNeedsRestart)
        .value("kUnsupported", th::ErrorKind::kUnsupported);

    py::enum_<th::Presentation>(m, "Presentation",
                                "Which substreams a decode reads; the PCM is the last one's.")
        .value("kBest", th::Presentation::kBest, "the highest of substreams 0 to 2 the stream has")
        .value("k2ch", th::Presentation::k2ch, "substream 0")
        .value("k6ch", th::Presentation::k6ch, "substreams 0 and 1")
        .value("k8ch", th::Presentation::k8ch, "substreams 0 to 2")
        .value("kAll", th::Presentation::kAll, "every substream, the sixteen-channel one last");

    py::class_<StreamInfoView>(m, "StreamInfo",
                               "What the first major sync and the restart headers after it say.")
        .def_readonly("kind", &StreamInfoView::kind, "'truehd' or 'mlp'")
        .def_readonly("sample_rate", &StreamInfoView::sample_rate)
        .def_readonly("substreams", &StreamInfoView::substreams)
        .def_readonly("channels", &StreamInfoView::channels,
                      "per substream: the channels a decode of it delivers, 0 if not reached")
        .def_readonly("samples_per_access_unit", &StreamInfoView::samples_per_access_unit)
        .def_readonly("access_units", &StreamInfoView::access_units,
                      "counted in the data given")
        .def_readonly("variable_rate", &StreamInfoView::variable_rate)
        .def_readonly("peak_data_rate_field", &StreamInfoView::peak_data_rate_field)
        .def_readonly("has_object_substream", &StreamInfoView::has_object_substream)
        .def_readonly("first_unit_offset", &StreamInfoView::first_unit_offset);

    m.def(
        "probe",
        [](const py::buffer& data) {
            const std::vector<std::byte> bytes = bytes_of(data);
            const th::StreamInfo found = value_or_throw(th::probe_stream(bytes));
            StreamInfoView out;
            out.kind = kind_name(found.major_sync.kind);
            out.sample_rate = found.sample_rate;
            out.substreams = found.num_substreams;
            out.channels.assign(found.channels.begin(), found.channels.end());
            out.samples_per_access_unit = found.samples_per_access_unit;
            out.access_units = found.access_units;
            out.variable_rate = found.major_sync.is_vbr;
            out.peak_data_rate_field = found.major_sync.peak_data_rate;
            out.has_object_substream = found.has_object_substream;
            out.first_unit_offset = found.first_unit_offset;
            return out;
        },
        py::arg("data"),
        "Reads the first major sync in `data` (it may start up to 64 KiB in) and the restart "
        "headers after it. Raises TruehdError (kind 'kBadSync') when there is none.");

    m.def(
        "access_units",
        [](const py::buffer& data) {
            const std::vector<std::byte> bytes = bytes_of(data);
            th::ElementaryReader reader(bytes);
            std::vector<py::bytes> units;
            while (const auto unit = reader.next()) {
                units.emplace_back(reinterpret_cast<const char*>(unit->data()), unit->size());
            }
            if (units.empty()) {
                throw TruehdFailure(th::Error{th::ErrorKind::kBadSync, "no access unit"});
            }
            return py::make_tuple(std::move(units), reader.skipped_bytes());
        },
        py::arg("data"),
        "Splits a bare stream into its access units: (units, skipped_bytes). Bytes between units "
        "that do not read as one are skipped and counted, those ahead of the first unit included.");

    py::class_<th::RestartPoint>(m, "RestartPoint",
                                 "A unit a decoder can start at: its number and where it begins.")
        .def_readonly("unit", &th::RestartPoint::unit)
        .def_readonly("offset", &th::RestartPoint::offset)
        .def("__repr__", [](const th::RestartPoint& r) {
            return "RestartPoint(unit=" + std::to_string(r.unit) +
                   ", offset=" + std::to_string(r.offset) + ")";
        });

    py::class_<th::ScannedStream>(m, "ScannedStream",
                                  "A stream's length, timing and the units a decoder can join at.")
        .def_property_readonly("kind", [](const th::ScannedStream& s) { return kind_name(s.kind); })
        .def_readonly("sample_rate", &th::ScannedStream::sample_rate)
        .def_readonly("samples_per_unit", &th::ScannedStream::samples_per_unit)
        .def_readonly("substreams", &th::ScannedStream::num_substreams)
        .def_property_readonly(
            "channels", [](const th::ScannedStream& s) {
                return std::vector<int>(s.channels.begin(), s.channels.end());
            })
        .def_readonly("variable_rate", &th::ScannedStream::variable_rate)
        .def_readonly("has_object_substream", &th::ScannedStream::has_object_substream)
        .def_readonly("units", &th::ScannedStream::units, "units that read")
        .def_readonly("unit_bytes", &th::ScannedStream::unit_bytes)
        .def_readonly("skipped_bytes", &th::ScannedStream::skipped_bytes,
                      "bytes not part of a unit that read, those ahead of the first unit included")
        .def_readonly("leading_bytes", &th::ScannedStream::leading_bytes,
                      "the part of skipped_bytes that came before the first unit")
        .def_readonly("major_syncs", &th::ScannedStream::major_syncs)
        .def_readonly("padding_samples", &th::ScannedStream::padding_samples)
        .def_readonly("restarts", &th::ScannedStream::restarts,
                      "the units a decoder can start at, the first among them")
        .def_property_readonly("samples", &th::ScannedStream::samples,
                               "every sample per channel, the last unit's padding left out")
        .def_property_readonly("duration_seconds", &th::ScannedStream::duration_seconds)
        .def("unit_start_sample", &th::ScannedStream::unit_start_sample, py::arg("unit"))
        .def("unit_at_sample", &th::ScannedStream::unit_at_sample, py::arg("sample"),
             "The unit that holds `sample`, or None past the end.")
        .def("sample_at_seconds", &th::ScannedStream::sample_at_seconds, py::arg("seconds"),
             "The sample at a time, rounded down; a negative time is sample 0.")
        .def(
            "restart_for_sample",
            [](const th::ScannedStream& s, std::uint64_t sample) {
                return s.restart_for_sample(sample);
            },
            py::arg("sample"),
            "Where to start decoding to reach `sample`: the last restart at or before its unit, "
            "or None past the end. Decode on from its offset; the first sample the decoder gives "
            "is unit_start_sample(restart.unit).");

    m.def(
        "scan",
        [](const py::buffer& data, std::uint64_t restart_spacing) {
            const std::vector<std::byte> bytes = bytes_of(data);
            return value_or_throw(
                th::scan_stream(bytes, th::ScanOptions{.restart_spacing = restart_spacing}));
        },
        py::arg("data"), py::arg("restart_spacing") = th::kDefaultRestartSpacing,
        "Reads a whole stream once: its length and timing, and the units with a major sync (where "
        "a decoder can start), kept at least `restart_spacing` apart (0 or 1 keeps every one). "
        "Raises TruehdError (kind 'kBadSync') for data with no unit that reads.");

    py::class_<DecodedUnitView>(m, "DecodedUnit", "One decoded access unit.")
        .def_readonly("sample_rate", &DecodedUnitView::sample_rate)
        .def_readonly("channels", &DecodedUnitView::channels)
        .def_readonly("samples", &DecodedUnitView::samples, "per channel")
        .def_readonly("substream", &DecodedUnitView::substream)
        .def_readonly("restarted", &DecodedUnitView::restarted)
        .def_readonly("padding_samples", &DecodedUnitView::padding_samples,
                      "samples at the end that are padding; not in pcm")
        .def_readonly("speakers", &DecodedUnitView::speakers,
                      "the speakers of the channels in order, when the major sync names them")
        .def_property_readonly(
            "pcm",
            [](const DecodedUnitView& u) {
                return pcm_array(u.pcm, static_cast<std::size_t>(u.channels));
            },
            "int32 array (samples, channels); each value 24 bits, sign-extended");

    py::class_<DecodedStreamView>(m, "DecodedStream", "A whole bare stream, decoded.")
        .def_readonly("sample_rate", &DecodedStreamView::sample_rate)
        .def_readonly("channels", &DecodedStreamView::channels)
        .def_readonly("speakers", &DecodedStreamView::speakers)
        .def_readonly("units", &DecodedStreamView::units)
        .def_readonly("skipped_bytes", &DecodedStreamView::skipped_bytes)
        .def_readonly("leading_bytes", &DecodedStreamView::leading_bytes)
        .def_property_readonly(
            "pcm",
            [](const DecodedStreamView& s) {
                return pcm_array(s.pcm, static_cast<std::size_t>(s.channels));
            },
            "int32 array (samples, channels); each value 24 bits, sign-extended");

    m.def(
        "decode",
        [](const py::buffer& data, th::Presentation presentation, bool lenient) {
            const std::vector<std::byte> bytes = bytes_of(data);
            DecodedStreamView out;
            std::optional<th::Error> failure;
            {
                py::gil_scoped_release release;
                th::Decoder decoder(make_config(presentation, lenient));
                th::ElementaryReader reader(bytes);
                while (const auto unit = reader.next()) {
                    auto decoded = decoder.decode(*unit);
                    if (!decoded) {
                        failure = decoded.error();
                        break;
                    }
                    ++out.units;
                    if (!*decoded) {
                        continue;
                    }
                    const th::DecodedAccessUnit& d = **decoded;
                    out.sample_rate = d.sample_rate;
                    out.channels = d.channels;
                    if (out.speakers.empty()) {
                        out.speakers = speaker_names(d.layout);
                    }
                    out.pcm.insert(out.pcm.end(), d.pcm.begin(), d.pcm.end());
                }
                out.skipped_bytes = reader.skipped_bytes();
                out.leading_bytes = reader.leading_bytes();
            }
            if (failure) {
                throw TruehdFailure(*failure);
            }
            if (out.units == 0) {
                throw TruehdFailure(th::Error{th::ErrorKind::kBadSync, "no access unit"});
            }
            return out;
        },
        py::arg("data"), py::arg("presentation") = th::Presentation::kBest,
        py::arg("lenient") = false,
        "Decodes a whole bare stream. With the default strict checks a unit that fails one raises "
        "TruehdError; with lenient=True the failure is counted and the unit decoded as far as it "
        "reads (its samples can then be wrong).");

    py::class_<th::Decoder>(m, "Decoder",
                            "A decoder for one stream, a whole access unit at a time. It holds the "
                            "state a unit continues, so units go in stream order.")
        .def(py::init([](th::Presentation presentation, bool lenient) {
                 return th::Decoder(make_config(presentation, lenient));
             }),
             py::arg("presentation") = th::Presentation::kBest, py::arg("lenient") = false)
        .def(
            "decode",
            [](th::Decoder& decoder, const py::buffer& unit) -> std::optional<DecodedUnitView> {
                const std::vector<std::byte> bytes = bytes_of(unit);
                auto decoded = value_or_throw(decoder.decode(bytes));
                if (!decoded) {
                    return std::nullopt;
                }
                DecodedUnitView out;
                out.sample_rate = decoded->sample_rate;
                out.channels = decoded->channels;
                out.samples = decoded->samples;
                out.substream = decoded->substream;
                out.restarted = decoded->restarted;
                out.padding_samples = decoded->padding_samples;
                out.pcm.assign(decoded->pcm.begin(), decoded->pcm.end());
                out.speakers = speaker_names(decoded->layout);
                return out;
            },
            py::arg("unit"),
            "Decodes one whole access unit (from its 4-byte header). None for a unit that cannot "
            "give samples yet: one before the first major sync, or before a substream's first "
            "restart header. Raises TruehdError for a unit that fails a check (strict) or does "
            "not read; the decoder stays usable and waits for the next restart header.")
        .def("lost_data", &th::Decoder::lost_data,
             "Call when input was dropped between the last unit decoded and the next: every "
             "substream waits for its next restart header.")
        .def("reset", &th::Decoder::reset, "Forgets the stream: the next unit has to carry a major sync.")
        .def_property_readonly("units", [](const th::Decoder& d) { return d.counters().units; })
        .def_property_readonly("units_decoded",
                               [](const th::Decoder& d) { return d.counters().units_decoded; })
        .def_property_readonly("check_failures",
                               [](const th::Decoder& d) { return d.counters().check_failures; })
        .def_property_readonly("lossless_check_failures", [](const th::Decoder& d) {
            return d.counters().lossless_check_failures;
        });
}

}  // namespace iclforge::python
