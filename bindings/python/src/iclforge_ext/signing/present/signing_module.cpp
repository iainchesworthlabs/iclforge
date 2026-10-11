#include "optional_modules.hpp"

#include <expected>
#include <string>
#include <vector>

#include "iclforge/ac3/io/object_strip.hpp"
#include "iclforge/ac3/signing/emdf_atmos_signer.hpp"
#include "iclforge/base/crypto/signing_key.hpp"

#include "binding_support.hpp"

// The variant of the `ac3.signing` submodule compiled when iclforge::ac3::signing is in this build.
//
// This is the body that used to sit inside bindings.cpp's PYBIND11_MODULE
// behind `#ifdef ICLFORGE_PY_HAVE_SIGNING`, moved verbatim. See
// optional_modules.hpp for why it is a translation unit now, and
// bindings/python/CMakeLists.txt for the selection that picks this file over the
// absent/ one beside it.

namespace iclforge::python {

namespace py = pybind11;
using detail::to_bytes;
using detail::to_bytes_list;
using detail::KwargBinder;

void register_signing(py::module_& m) {
    // --- Object signing (Python bindings completeness) --------------------------------------
    auto signing = m.def_submodule(
        "signing",
        "EMDF object-layer signing - see docs/concepts/object-signing.md. Sign an encoded "
        "Atmos stream's frames, detect tags, verify with the matching key.");

    py::class_<iclforge::base::crypto::SigningKey>(signing, "SigningKey",
                                         "Owns the key bytes; zeroizes them on destruction.")
        .def(py::init([](const py::buffer& content) {
                 auto decoded = iclforge::base::crypto::decode_signing_key(to_bytes(content));
                 if (!decoded) {
                     throw py::value_error("empty signing key");
                 }
                 return *decoded;
             }),
             py::arg("content"),
             "Decode a key from bytes: base64 when the content is valid base64 (the "
             "CI/secret transport form), raw key bytes otherwise - the same single decode "
             "every other front end uses.")
        .def_property_readonly("empty", &iclforge::base::crypto::SigningKey::empty);

    signing.def(
        "load_signing_key",
        [](const std::string& explicit_path) {
            auto key = iclforge::base::crypto::load_signing_key(explicit_path);
            if (!key) {
                throw py::value_error(key.error().message);
            }
            return *key;
        },
        py::arg("path") = std::string{},
        "Resolve a key from `path` if given, else $ICLFORGE_SIGNING_KEY_FILE, else "
        "$ICLFORGE_SIGNING_KEY - the CLI's own resolution order. Raises ValueError with the "
        "loader's message when nothing usable was found.");

    signing.def(
        "sign_atmos_stream",
        [](const py::buffer& stream, const iclforge::base::crypto::SigningKey& key) {
            auto bytes = to_bytes(stream);
            int signed_count = 0;
            {
                py::gil_scoped_release release;
                signed_count = iclforge::ac3::signing::sign_atmos_stream(bytes, key);
            }
            return py::make_tuple(
                py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                signed_count);
        },
        py::arg("stream"), py::arg("key"),
        "Sign every Atmos frame in an elementary stream. Returns (signed_stream, "
        "frames_signed) - the input is not modified (Python bytes are immutable; the C++ "
        "in-place form signs a copy here).");

    signing.def(
        "has_authenticity_tag",
        [](const py::buffer& frame) {
            return iclforge::ac3::signing::has_authenticity_tag(to_bytes(frame));
        },
        py::arg("frame"),
        "Whether this frame carries an authenticity tag - answerable without any key.");

    py::class_<iclforge::ac3::signing::VerifySummary>(signing, "VerifySummary")
        .def_readonly("valid", &iclforge::ac3::signing::VerifySummary::valid)
        .def_readonly("mismatch", &iclforge::ac3::signing::VerifySummary::mismatch)
        .def_readonly("no_container", &iclforge::ac3::signing::VerifySummary::no_container)
        .def_property_readonly("all_valid", [](const iclforge::ac3::signing::VerifySummary& s) {
            return s.valid > 0 && s.mismatch == 0;
        });

    signing.def(
        "verify_atmos_stream",
        [](const py::buffer& stream, const iclforge::base::crypto::SigningKey& key) {
            const auto bytes = to_bytes(stream);
            py::gil_scoped_release release;
            return iclforge::ac3::signing::verify_atmos_stream(bytes, key);
        },
        py::arg("stream"), py::arg("key"),
        "Verify every frame's tag against `key`. See VerifySummary.");

    // --- Several keys, and the licensed gate ---------------------------------------------------
    py::class_<iclforge::ac3::signing::KeyringSummary>(
        signing, "KeyringSummary",
        "verify_atmos_stream_any's result: `totals` is the VerifySummary (`valid` meaning valid "
        "under some key), `per_key[i]` how many frames key i accepted - the first accepting key "
        "wins, so the entries add up to totals.valid.")
        .def_readonly("totals", &iclforge::ac3::signing::KeyringSummary::totals)
        .def_readonly("per_key", &iclforge::ac3::signing::KeyringSummary::per_key);

    signing.def(
        "verify_atmos_stream_any",
        [](const py::buffer& stream,
           const std::vector<iclforge::base::crypto::SigningKey>& keys) {
            const auto bytes = to_bytes(stream);
            py::gil_scoped_release release;
            return iclforge::ac3::signing::verify_atmos_stream_any(bytes, keys);
        },
        py::arg("stream"), py::arg("keys"),
        "Verify every frame's tag against a list of keys: a frame is valid when ANY of them "
        "reproduces its tag. An empty list, or one of empty keys, accepts nothing.");

    py::class_<iclforge::ac3::signing::GateSummary>(
        signing, "GateSummary",
        "gate_atmos_stream's counts: `passed` frames kept their object layer, `gated` frames had "
        "it removed (a container no key reproduced, unsigned included), `no_objects` had none to "
        "gate; `per_key` credits the passed frames as KeyringSummary does.")
        .def_readonly("passed", &iclforge::ac3::signing::GateSummary::passed)
        .def_readonly("gated", &iclforge::ac3::signing::GateSummary::gated)
        .def_readonly("no_objects", &iclforge::ac3::signing::GateSummary::no_objects)
        .def_readonly("per_key", &iclforge::ac3::signing::GateSummary::per_key);

    signing.def(
        "gate_atmos_stream",
        [](const py::buffer& stream,
           const std::vector<iclforge::base::crypto::SigningKey>& keys) {
            const auto bytes = to_bytes(stream);
            std::expected<iclforge::ac3::signing::GatedStream, iclforge::ac3::io::StripError> result;
            {
                py::gil_scoped_release release;
                result = iclforge::ac3::signing::gate_atmos_stream(bytes, keys);
            }
            if (!result) {
                throw py::value_error(std::string{iclforge::ac3::io::describe(result.error())});
            }
            return py::make_tuple(
                py::bytes(reinterpret_cast<const char*>(result->bytes.data()), result->bytes.size()),
                result->summary);
        },
        py::arg("stream"), py::arg("keys"),
        "The licensed decode policy: returns (stream, GateSummary) where every frame whose object "
        "layer verifies under some key is kept as it was and every other one has its object layer "
        "removed, so the result plays as objects where the tag was good and as the 5.1 bed "
        "elsewhere. Raises ValueError for a stream it cannot walk, or one carrying an object "
        "layer that can be neither verified nor removed - it fails closed.");
}

}  // namespace iclforge::python
