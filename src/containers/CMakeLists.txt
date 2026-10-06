# ---------------------------------------------------------------------------
# iclforge::containers - the codec-blind carriage, one library in five parts, each in namespace
# iclforge::containers::<part> with its headers under iclforge/containers/<part>/:
#
#   iec61937   IEC 61937 burst packing and unpacking: an access unit of AC-3, E-AC-3 or AC-4
#              disguised as PCM for an S/PDIF or HDMI sink, and the detector that finds one in a
#              capture. Always built.
#   matroska   a Matroska (.mkv) writer and reader (ICLFORGE_BUILD_MATROSKA).
#   mp4        an MP4 (ISOBMFF) writer and reader, with fragmented MP4/CMAF segmenting and HLS/DASH
#              signaling (ICLFORGE_BUILD_MP4).
#   mpegts     an MPEG-2 Transport Stream writer and reader (ICLFORGE_BUILD_MPEGTS).
#   iamf       AOM's IAMF v2.0.0: the OBU syntax, the raw OBU stream and the ISO-BMFF encapsulation
#              (ICLFORGE_BUILD_IAMF).
#
# Each part takes the codec's frames and configuration boxes as opaque bytes, so the library links
# no codec and the codecs never reach for it: front ends link both. A vcpkg feature and a Conan
# option of each part's name switch its option; a part that is off compiles none of its sources and
# installs none of its headers (cmake/InstallLibrary.cmake). The five were libraries of their own
# until planning/consolidation.md's C3.
# ---------------------------------------------------------------------------
set(_containers_sources src/iec61937/iec61937.cpp)
set(_containers_not_built "")
if(ICLFORGE_BUILD_MATROSKA)
    list(APPEND _containers_sources src/matroska/matroska.cpp src/matroska/reader.cpp)
else()
    list(APPEND _containers_not_built matroska)
endif()
if(ICLFORGE_BUILD_MP4)
    list(APPEND _containers_sources
        src/mp4/mp4.cpp
        src/mp4/reader.cpp
        src/mp4/fragment.cpp
        src/mp4/hls.cpp
        src/mp4/dash.cpp)
else()
    list(APPEND _containers_not_built mp4)
endif()
if(ICLFORGE_BUILD_MPEGTS)
    list(APPEND _containers_sources src/mpegts/mpegts.cpp src/mpegts/reader.cpp)
else()
    list(APPEND _containers_not_built mpegts)
endif()
if(ICLFORGE_BUILD_IAMF)
    list(APPEND _containers_sources
        src/iamf/container.cpp
        src/iamf/iamf.cpp
        src/iamf/sequence_common.cpp
        src/iamf/sequence_read.cpp
        src/iamf/sequence_write.cpp)
else()
    list(APPEND _containers_not_built iamf)
endif()

# fmt_private: the MP4 part's HLS and DASH writers format their manifests with {fmt}. src/ is
# where the parts' shared private headers are (isobmff_writer.hpp).
iclforge_add_library(containers
    SOURCES ${_containers_sources}
    DEPENDS base
    PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/src"
    LINK_PRIVATE
        "$<BUILD_INTERFACE:iclforge::tracy>"
        "$<BUILD_INTERFACE:iclforge::fmt_private>")

# The parts that are off, whose headers the install leaves out.
set(ICLFORGE_CONTAINERS_NOT_BUILT "${_containers_not_built}" PARENT_SCOPE)
