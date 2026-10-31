get_filename_component(ICLFORGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(TRUEHD "${ICLFORGE_ROOT}/libs/truehd")
set(BASE "${ICLFORGE_ROOT}/libs/base")
set(FIXTURES "${ICLFORGE_ROOT}/testdata/truehd")

set(TRUEHD_SOURCES
    "${TRUEHD}/src/core/crc.cpp"
    "${TRUEHD}/src/core/format.cpp"
    "${TRUEHD}/src/core/syntax.cpp"
    "${TRUEHD}/src/decoder/decoder.cpp"
    "${TRUEHD}/src/io/access_unit.cpp"
    "${TRUEHD}/src/io/major_sync.cpp")

# The streams, embedded in the image: the symbol of each is _binary_<file>_start, with the dots
# as underscores (fixtures.inc names them).
set(FIXTURE_FILES
    "${FIXTURES}/truehd_mono48_s24.thd"
    "${FIXTURES}/truehd_st48_s16.thd"
    "${FIXTURES}/truehd_st48_s24.thd"
    "${FIXTURES}/truehd_st44_s24.thd"
    "${FIXTURES}/truehd_st96_s24.thd"
    "${FIXTURES}/truehd_st192_s24.thd"
    "${FIXTURES}/truehd_m51_s16.thd"
    "${FIXTURES}/truehd_m51_s24.thd")

idf_component_register(
    SRCS main.cpp ${TRUEHD_SOURCES}
    INCLUDE_DIRS "" "${CMAKE_CURRENT_SOURCE_DIR}/../shim" "${TRUEHD}/include" "${BASE}/include"
                 "${BASE}/variants/profiling-tracy_disabled"
    PRIV_INCLUDE_DIRS "${TRUEHD}/src"
    EMBED_FILES ${FIXTURE_FILES}
    REQUIRES esp_timer esp_system freertos heap esp_hw_support esp_rom)

# The decoder's optimisation: -O2, as the AC-3 and AC-4 decoders' hot files are in the real image
# (libs/ac4/minimal.cmake); TRUEHD_PROBE_OPT=Os measures the size-optimised build.
set(TRUEHD_PROBE_OPT "O2" CACHE STRING "-O2 or -Os for the decoder's sources")
if(CONFIG_TRUEHD_PROBE_TRACE)
    set_source_files_properties(${TRUEHD_SOURCES} PROPERTIES COMPILE_OPTIONS "-${TRUEHD_PROBE_OPT};-finstrument-functions")
else()
    set_source_files_properties(${TRUEHD_SOURCES} PROPERTIES COMPILE_OPTIONS "-${TRUEHD_PROBE_OPT}")
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-psabi -Wno-error=deprecated-declarations)
if(CONFIG_TRUEHD_PROBE_TRACE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=__cxa_allocate_exception")
endif()
