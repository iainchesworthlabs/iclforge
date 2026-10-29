# ---------------------------------------------------------------------------
# iclforge::mlp - the TrueHD (MLP) codec, written from the clean-room specification
# truehd-spec.md (planning/truehd.md): the bitstream layer, the decoder and the lossless
# encoder, with the evolution (extra data) carriage that holds Atmos object metadata.
#
#   core/     what the decoder and the encoder share: the structures and their one syntax
#             description (syntax.hpp, read and written through syntax_io.hpp), the access unit
#             framing, the CRCs and parity, the Huffman codebooks, prediction and matrixing
#   io/       the elementary stream (scan, probe) and the extra data container
#   decoder/  substream decode, presentations, conformance checks as typed errors
#   encoder/  analysis (matrices, filters), coding (Huffman, quantiser), rate control, writing
#   atmos/    object audio metadata to and from evolution payloads, through iclforge::objects
#
# A peer of iclforge::ac3 and iclforge::ac4 that links neither. The core's headers are private
# (src/core); the public headers are iclforge/mlp/core/, io/, decoder/ and encoder/.
# ---------------------------------------------------------------------------

set(_mlp_core_sources
    src/core/access_unit.cpp
    src/core/stream_info.cpp)

set(_mlp_decoder_sources
    src/decoder/decoder.cpp)

set(_mlp_encoder_sources
    src/encoder/analysis.cpp
    src/encoder/coder.cpp
    src/encoder/encoder.cpp
    src/encoder/plan.cpp
    src/encoder/rate_control.cpp)

set(_mlp_io_sources
    src/io/elementary.cpp
    src/io/extra_data.cpp)

# Atmos: the 16-channel substream's objects, their render into the bed, and their metadata in
# evolution payloads through iclforge::objects' OAMD serialiser.
set(_mlp_atmos_sources
    src/atmos/atmos.cpp)

# base: the bit reader's syntax trace; objects: the OAMD model the Atmos headers name.
iclforge_add_library(mlp
    DEPENDS base objects
    SOURCES
        ${_mlp_core_sources}
        ${_mlp_decoder_sources}
        ${_mlp_encoder_sources}
        ${_mlp_io_sources}
        ${_mlp_atmos_sources}
    PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/src"
    # The bit reader and writer are header-only; base's public headers carry them.
    LINK_PRIVATE "$<BUILD_INTERFACE:iclforge::base_headers>")
