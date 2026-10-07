# ---------------------------------------------------------------------------
# InstallLibrary.cmake
#
# install() rules + package config for distributing iclforge::ac3, iclforge::containers and
# the other libraries below independently, consumable via find_package(iclforge). iclforge::audio
# (src/audio/) is deliberately NOT installed/exported here - it is a CLI/GUI implementation
# detail, not part of the distributed package; see docs/library/index.md.
#
# include()'d from the root CMakeLists.txt after add_subdirectory(src/ac3), src/containers and each
# optional component's own guarded add_subdirectory(), before include(Packaging) - CPack's own
# library component (cmake/Packaging.cmake) packages exactly what gets install()'d here.
#
# iclforge::containers is installed always, with the parts its options selected
# (ICLFORGE_BUILD_MATROSKA, ICLFORGE_BUILD_MP4, ICLFORGE_BUILD_MPEGTS, ICLFORGE_BUILD_IAMF): a part
# that is off installs no headers. iclforge::c, the AC-4 library and iclforge::iab have their own
# ICLFORGE_BUILD_<NAME> option, guarded add_subdirectory() and guarded block below. Each option maps
# 1:1 onto its own vcpkg feature (packaging/vcpkg-port/iclforge/vcpkg.json's
# "matroska"/"mp4"/"mpegts"/"capi"/"ac4"/"iab"/"iamf", wired through portfile.cmake's
# vcpkg_check_features()) and its own Conan option (packaging/conan/conanfile.py), so a vcpkg or
# Conan install only gets the ones its feature selection actually asked for.
#
# Every install() rule below carries COMPONENT library: without one, CPack
# files it under its own "Unspecified" component, inconsistent once
# component-based packaging is on (see cmake/Packaging.cmake) - same reason
# apps/cli/CMakeLists.txt's forge install() carries COMPONENT runtime.
#
# The LIBRARY DESTINATION rules below additionally carry NAMELINK_COMPONENT
# library, splitting them from COMPONENT libruntime. On Unix, a versioned
# shared library install produces two files - the real
# libiclforge_ac3.so.<version> and an unversioned libiclforge_ac3.so symlink (the
# "namelink") a linker resolves -l against - and NAMELINK_COMPONENT is CMake's
# own mechanism for filing those two files under different CPack components:
# COMPONENT names the real .so, NAMELINK_COMPONENT names the symlink. Confirmed
# empirically (see cmake/Packaging.cmake's DEB/RPM comment) that today's
# monolithic .deb bundles forge together with the full SDK - headers, static
# archives, CMake package config, .so and symlink alike - because CPack's DEB/
# RPM generators ignore CPACK_COMPONENTS_ALL entirely unless *_COMPONENT_INSTALL
# is explicitly turned on for them. This split is what makes a real
# runtime/-dev separation possible there: libruntime becomes a small
# "just the .so a linked binary needs at runtime" package, while library
# keeps everything only a builder needs (headers, static archives, CMake
# config, and the symlink you link against, -l style). RUNTIME/ARCHIVE (the
# Windows .dll/.lib pair, and the static archives on every platform) stay
# under COMPONENT library throughout: NAMELINK_COMPONENT only ever affects the
# LIBRARY DESTINATION install, i.e. Unix .so installs - Windows has no
# namelink concept at all, so this is a no-op there and the Windows dev ZIP is
# unaffected.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(PkgConfig)

# OFF is what a vcpkg port needs: vcpkg's per-triplet linkage policy (and its post-build lint)
# expects a port to ship only the variant matching that triplet's VCPKG_LIBRARY_LINKAGE, not
# both. ON (the default) keeps today's direct-build/CPack SDK behaviour unchanged - both
# variants installed and exported, same as before this option existed. iclforge_ac3_static/iclforge_ac3_shared
# and their matroska/mp4/mpegts equivalents still get *built* either way - only what gets
# install()'d/exported is filtered by this option, so nothing above this point in the tree
# needs touching for it to take effect.
option(ICLFORGE_INSTALL_BOTH_LINKAGES "Install/export both static and shared library variants (OFF installs only the BUILD_SHARED_LIBS-selected one)" ON)

if(ICLFORGE_INSTALL_BOTH_LINKAGES)
    set(_iclforge_forge_install_targets iclforge_ac3_objects iclforge_ac3_static iclforge_ac3_shared)
elseif(BUILD_SHARED_LIBS)
    # iclforge::c (src/capi/CMakeLists.txt) statically embeds iclforge::ac3_static PRIVATE
    # unconditionally, regardless of BUILD_SHARED_LIBS - see that file's header comment for why
    # (a self-contained C ABI, not one that depends on a separately-shipped forge shared
    # library). iclforge_ac3_static used to have to be in an export set here: iclforge_capi_objects is an
    # OBJECT library, so the PRIVATE dependency ended up in its own INTERFACE_LINK_LIBRARIES
    # (OBJECT libraries have no link step of their own to hide it behind), and since
    # iclforge_capi_objects is itself part of capiTargets whenever ICLFORGE_BUILD_CAPI is ON,
    # install(EXPORT capiTargets) failed with "requires target iclforge_ac3_static that is not in any
    # export set." That dependency now sits on iclforge_capi_static and iclforge_capi_shared instead, and a
    # shared library's PRIVATE dependencies are not exported, so nothing in capiTargets names
    # iclforge_ac3_static in this branch any more. It is still installed, so that a package built this
    # way keeps shipping the archive it always has. iclforge_ac3_shared never needed the same treatment.
    if(ICLFORGE_BUILD_CAPI)
        set(_iclforge_forge_install_targets iclforge_ac3_objects iclforge_ac3_static iclforge_ac3_shared)
    else()
        set(_iclforge_forge_install_targets iclforge_ac3_objects iclforge_ac3_shared)
    endif()
else()
    set(_iclforge_forge_install_targets iclforge_ac3_objects iclforge_ac3_static)
endif()

# iclforge_ac3_simd_avx2 (runtime SIMD dispatch, x86_64 only,
# ICLFORGE_AVX2) is PUBLIC-linked into both iclforge_ac3_static and iclforge_ac3_shared
# unconditionally (src/ac3/CMakeLists.txt), so it needs the same
# export-set membership iclforge_ac3_objects gets just above and for the same
# reason: install(EXPORT) cannot resolve a usage-requirement dependency
# that is not itself part of an export set, regardless of which branch
# above put iclforge_ac3_static/iclforge_ac3_shared in the target list. Does not exist
# at all when ICLFORGE_AVX2=OFF or the target is not x86_64.
if(TARGET iclforge_ac3_simd_avx2)
    list(APPEND _iclforge_forge_install_targets iclforge_ac3_simd_avx2)
endif()

# Two separate EXPORT sets, not the one combined set an earlier draft of this
# plan sketched: install(EXPORT ... NAMESPACE X) applies X uniformly to
# every target in that export set, and iclforge::ac3_static/iclforge::ac3_shared
# need a different namespace from iclforge::containers_static/
# iclforge::containers_shared. Both still land in the one iclforgeConfig.cmake
# a consumer's find_package(iclforge) resolves - see iclforgeConfig.cmake.in,
# which include()s both generated *Targets.cmake files.
#
# forgeTargets (not iclforgeTargets): every other export set here is named
# after its own ICLFORGE_BUILD_<NAME> component switch (matroskaTargets,
# mp4Targets, mpegtsTargets, capiTargets) - forge has no such switch, since
# it's the one mandatory, always-built component, but it still gets named
# after its own component identity ("forge", matching its raw target names
# iclforge_ac3_static/iclforge_ac3_shared) rather than after the overall package, for the
# same consistency reason.
# The _objects OBJECT library has to be in the same export set as the
# _static/_shared targets that PUBLIC-link it, even though nothing about it
# needs installing on its own (its compiled code is already embedded in the
# installed .lib/.dll) - install(EXPORT) otherwise refuses to generate,
# since it can't resolve a usage-requirement dependency that isn't itself
# part of any export set.
# Source headers, from iclforge::ac3's include/ tree, as the file set iclforge_header_set()
# (cmake/IclforgeLibrary.cmake) gives every library.
iclforge_header_set(iclforge_ac3_objects "${PROJECT_SOURCE_DIR}/src/ac3/include")
install(TARGETS ${_iclforge_forge_install_targets}
    EXPORT ac3Targets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library
    FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT library)

# The generated header - the generate_export_header() output (the family's version is
# iclforge::base's, installed with it) - lives in the library's own binary dir, not
# its source tree (see src/ac3/CMakeLists.txt), so the header file set above never
# sees it. A consumer's #include
# <iclforge/ac3/export.hpp> needs it installed at the same relative path the
# in-tree BUILD_INTERFACE include dirs already use.
install(FILES
        "${CMAKE_BINARY_DIR}/src/ac3/generated/iclforge/ac3/export.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/ac3"
    COMPONENT library)

iclforge_pkgconfig_libname(_iclforge_forge_pc_libname iclforge_ac3_shared iclforge_ac3 iclforge_ac3_static
    "${_iclforge_forge_install_targets}")
iclforge_install_pkgconfig(
    NAME iclforge-ac3
    DESCRIPTION "Clean-room AC-3 (ATSC A/52) and E-AC-3 encoder and decoder with a spatial object layer"
    LIBNAME "${_iclforge_forge_pc_libname}"
    REQUIRES iclforge-base iclforge-dsp iclforge-objects iclforge-render)

# The codec-blind libraries iclforge::ac3 links (src/base, dsp, objects, render), and the
# containers: each is a mandatory component, installed and exported like the codec.
# Fixed32 and the scalar arithmetic (iclforge/base/arithmetic/) are in-tree build plumbing, never
# installed: iclforge::base_headers (src/base/CMakeLists.txt).
iclforge_install_library(base
    DESCRIPTION "The family's version, the bit reader and writer, the speaker vocabulary, the CPU feature probe and the signing key, SHA-256 and HMAC-SHA-256 the iclforge libraries build on"
    GENERATED_HEADERS iclforge/base/version.hpp
    EXCLUDE arithmetic)
iclforge_install_library(dsp
    DESCRIPTION "The FFT, the QMF bank, the sample-rate converter and the biquad sections the iclforge codecs share")
iclforge_install_library(objects
    DESCRIPTION "The object-audio model, its scene readers and the Object Audio Metadata payload of ETSI TS 103 420")
iclforge_install_library(render
    DESCRIPTION "Speaker layouts, routing, the bed and object renderer and the panner")
iclforge_install_library(containers
    DESCRIPTION "IEC 61937 burst packing, and the Matroska, MP4/ISOBMFF (with fMP4/CMAF, HLS and DASH), MPEG-2 TS and IAMF writers and readers this build selected"
    EXCLUDE ${ICLFORGE_CONTAINERS_NOT_BUILT})

# The optional components below (iclforge::iab, the AC-4 library, adm, and iclforge::c) each
# have their own ICLFORGE_BUILD_<NAME> option (root
# CMakeLists.txt) and their own guarded block here, and the vcpkg port's and the Conan recipe's
# features of the same names switch them (packaging/). Their targets, headers and export sets only
# exist to install when the component was actually built; iclforgeConfig.cmake.in includes each
# *Targets.cmake only if(EXISTS).
# A reader rather than a writer; the vcpkg port's "iab" feature and the Conan recipe's "iab"
# option switch it, off unless asked for (packaging/).
if(ICLFORGE_BUILD_IAB)
    iclforge_install_library(iab
        DESCRIPTION "Standalone SMPTE ST 2098-2 Immersive Audio Bitstream reader")
endif()

# iclforge::adm (the parser and the bridge, ICLFORGE_BUILD_ADM), unlike every other component in
# this file, installs and exports its SHARED variant only, regardless of
# ICLFORGE_INSTALL_BOTH_LINKAGES and BUILD_SHARED_LIBS: it embeds the third-party libbw64 and libadm
# (never installed or exported by this project in their own right), which only a self-contained
# shared library can absorb without either re-exporting them or leaving a static archive with
# unresolved symbols (src/adm/CMakeLists.txt's header comment). Its .pc names the object model and
# the IAB reader, which the bridge's headers name. No vcpkg or Conan feature: iclforge::adm needs
# Boost (docs/library/index.md).
if(ICLFORGE_BUILD_ADM)
    iclforge_install_library(adm SHARED_ONLY
        DESCRIPTION "Standalone BW64/RF64 + Audio Definition Model (ADM) parser, and its bridge onto the object model iclforge::ac3's Atmos encoder takes"
        REQUIRES iclforge-objects iclforge-iab)
endif()

# The AC-4 codec (src/ac4, iclforge::ac4: the inspector, the decoder and the encoder, one library)
# is an optional component, ICLFORGE_BUILD_AC4 (see the root CMakeLists.txt). The vcpkg port's
# "ac4" feature and the Conan recipe's "ac4" option switch it, off unless asked for (packaging/): it
# adds public targets, which a curated vcpkg port's default features may not. The tables and the
# transforms the decoder and the encoder share are inside it, with hidden symbols and private
# headers, so nothing else is installed for them.
if(ICLFORGE_BUILD_AC4)
    iclforge_install_library(ac4
        DESCRIPTION "AC-4 (ETSI TS 103 190-1 and TS 103 190-2) inspector, decoder and encoder"
        REQUIRES iclforge-base)
endif()

# iclforge::c is an optional component (ICLFORGE_BUILD_CAPI, see the root CMakeLists.txt). Roadmap
# item F1's whole point is a stable C-callable surface for OTHER toolchains, so its header
# (iclforge_c/iclforge.h) installs to its own include/iclforge_c/ subdirectory - a C or non-C++
# consumer has no reason to see (or accidentally #include) any C++ header this package ships.
# iclforge.h includes the generated export.h and version.h, which are installed beside it.
#
# STATIC_REQUIRES iclforge-ac3: libiclforge_c_static.a calls into libiclforge_ac3_static.a
# (capiTargets carries $<LINK_ONLY:iclforge::ac3_static> for it), and only a static-only install
# names the archive here. iclforge_install_pkgconfig() says why that goes to Requires.private:
# libiclforge_c.so embeds the codec and needs no libiclforge_ac3.so beside it. iclforge.h declares
# its AC-4 section either way; with ICLFORGE_BUILD_AC4 off those functions return
# ICLFORGE_ERROR_UNSUPPORTED (src/capi/src/ac4_absent.cpp). With it on, the archive also calls into
# the decoder's and the encoder's (capiTargets carries their $<LINK_ONLY:...> archives too), and the
# .pc of each brings the inspector and the core.
if(ICLFORGE_BUILD_CAPI)
    if(ICLFORGE_BUILD_AC4)
        set(_iclforge_capi_pc_description
            "Stable C11 API over the AC-3, E-AC-3 and AC-4 encoders and decoders")
        set(_iclforge_capi_pc_static_requires iclforge-ac3 iclforge-ac4)
    else()
        set(_iclforge_capi_pc_description
            "Stable C11 API over the AC-3 and E-AC-3 encoders and decoders")
        set(_iclforge_capi_pc_static_requires iclforge-ac3)
    endif()
    iclforge_install_library(capi
        DESCRIPTION "${_iclforge_capi_pc_description}"
        STATIC_REQUIRES ${_iclforge_capi_pc_static_requires}
        GENERATED_HEADERS iclforge_c/version.h)
endif()

# The config file find_package(iclforge) actually loads. No find_dependency()
# calls needed in iclforgeConfig.cmake.in: the platform-audio code is
# physically in a separate, non-exported target (iclforge::audio), and {fmt}, the
# one third-party library iclforge::ac3 and iclforge::containers use, is compiled into their
# object files as a private copy (iclforge::fmt_private, cmake/Fmt.cmake) instead of linked. A
# shared library absorbs a linked {fmt} at its own link step; a static archive
# cannot, so a linked {fmt} would leave its consumers an undefined fmt:: symbol
# that this package names nowhere. tools/checks/check_install_consumer.sh links
# every installed archive whole, so a symbol that neither the package nor the
# C/C++ runtime supplies fails there.
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/iclforgeConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/iclforgeConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge")

# SameMajorVersion, not exact: pre-1.0, there is no ABI-compatibility promise
# across any two releases (see src/ac3/CMakeLists.txt's SOVERSION comment for
# the full reasoning), but SameMajorVersion is the conventional default and
# is what actually governs here - find_package()'s own version matching
# against a requested `find_package(iclforge X.Y.Z)`, not the .so's SONAME
# (which is set separately, to the full version, precisely because 0.x has
# no narrower compatible range to express).
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/iclforgeConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/iclforgeConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/iclforgeConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
    COMPONENT library)

install(EXPORT ac3Targets
    FILE ac3Targets.cmake
    NAMESPACE iclforge::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
    COMPONENT library)
