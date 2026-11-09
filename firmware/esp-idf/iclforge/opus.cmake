# ---------------------------------------------------------------------------
# libopus, for the Sendspin player's Opus streams (CONFIG_ICLFORGE_SENDSPIN_OPUS).
#
# Included by this component's CMakeLists.txt, below the registration and only when the option is
# on, so a firmware that does not play Opus builds nothing of it. What is built is the decoder's
# half of libopus 1.6.1 in FIXED POINT: no float API, no encoder's analysis, no DNN, since the part
# a player runs on has no FPU and the decoder is all a player is asked for. The whole of CELT and
# SILK is listed and the linker keeps what the decoder reaches.
#
# WHERE THE SOURCES COME FROM. The release tarball, from xiph.org, checked against the SHA-256
# below and extracted into the build directory the first time; or, to build offline or from a tree
# already on the disk (vcpkg's, say), an extracted libopus 1.6.1 at ICLFORGE_OPUS_SOURCE_DIR. A
# tree given that way is not checked: it is the builder's own.
#
# THE TARBALL, in this order. (1) A copy already on the disk: opus-<version>.tar.gz in the directory
# ICLFORGE_OPUS_TARBALL_DIR names (a cache variable, or the environment variable of that name). It
# is used only when its SHA-256 is the pinned one; any other file there is refused with a warning
# and deleted, never used. (2) Otherwise a download, from each of ICLFORGE_OPUS_URLS in turn
# (downloads.xiph.org, which redirects to OSU Open Source Lab's mirror; that mirror's own URL; and
# Gentoo's distfiles mirror, which is a different host and serves the same bytes), the whole list
# tried ICLFORGE_OPUS_ATTEMPTS times (default 3) with a pause between rounds (5, then 15 s). Each
# transfer has a timeout and is held to the same SHA-256, which is what makes a mirror safe to
# list: a host that serves other bytes is just a failed try. The 2026-11-09 merge-queue runs of the
# release failed twice in a row on "SSL connect error" to the one host there was. A download that
# passes is also copied into the directory of (1) when that is set, so that a CI cache of it can be
# saved. Nothing here relaxes the pin: no path reaches the build with a tarball that did not hash to it.
#
# Run as a script - `cmake -DICLFORGE_OPUS_TARBALL_DIR=<dir> -P opus.cmake` - it does only (1) and
# (2) for that directory and stops. .github/actions/opus-tarball uses that to restore and fill the
# CI cache before the first configure.
#
# MEASURED, on an ESP32-C6 (docs/platforms/bare-metal/esp32-c6.md, "Opus from a server"): 7.5 ms to
# decode 20 ms of 128 kbit/s stereo (37% of the core), 27,060 bytes of decoder state, about 16.4 KB
# of stack.
# ---------------------------------------------------------------------------
set(ICLFORGE_OPUS_VERSION "1.6.1")
set(ICLFORGE_OPUS_SHA256 "6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1")
# The URLs, the number of rounds and the pauses between them may be overridden (ICLFORGE_OPUS_URL
# names one URL alone: a mirror, or a file:// URL and no pauses in a test); the SHA-256 above may not.
if(NOT ICLFORGE_OPUS_URLS)
    if(ICLFORGE_OPUS_URL)
        set(ICLFORGE_OPUS_URLS "${ICLFORGE_OPUS_URL}")
    else()
        set(ICLFORGE_OPUS_URLS
            "https://downloads.xiph.org/releases/opus/opus-${ICLFORGE_OPUS_VERSION}.tar.gz"
            "https://ftp.osuosl.org/pub/xiph/releases/opus/opus-${ICLFORGE_OPUS_VERSION}.tar.gz"
            "https://distfiles.gentoo.org/distfiles/cb/opus-${ICLFORGE_OPUS_VERSION}.tar.gz")
    endif()
endif()
if(NOT ICLFORGE_OPUS_ATTEMPTS)
    set(ICLFORGE_OPUS_ATTEMPTS 3)
endif()
if(NOT DEFINED ICLFORGE_OPUS_RETRY_DELAYS)
    set(ICLFORGE_OPUS_RETRY_DELAYS "5;15")
endif()
set(ICLFORGE_OPUS_SOURCE_DIR "" CACHE PATH
    "An extracted libopus ${ICLFORGE_OPUS_VERSION} source tree to build from, in place of the release tarball")
set(ICLFORGE_OPUS_TARBALL_DIR "" CACHE PATH
    "A directory that holds (or is to receive) opus-${ICLFORGE_OPUS_VERSION}.tar.gz; default: the environment variable of the same name")
if(ICLFORGE_OPUS_TARBALL_DIR STREQUAL "" AND DEFINED ENV{ICLFORGE_OPUS_TARBALL_DIR})
    set(ICLFORGE_OPUS_TARBALL_DIR "$ENV{ICLFORGE_OPUS_TARBALL_DIR}")
endif()

# Puts the pinned libopus tarball at <dest>, or stops with the reason. Afterwards <dest> is a file
# whose SHA-256 is ICLFORGE_OPUS_SHA256: a download goes to <dest>.part and is renamed only once
# it has passed.
function(iclforge_opus_obtain dest)
    set(seed "")
    if(NOT ICLFORGE_OPUS_TARBALL_DIR STREQUAL "")
        set(seed "${ICLFORGE_OPUS_TARBALL_DIR}/opus-${ICLFORGE_OPUS_VERSION}.tar.gz")
    endif()
    # A copy that is already there: the seed first, then whatever an earlier configure left.
    foreach(candidate IN ITEMS "${seed}" "${dest}")
        if(NOT candidate STREQUAL "" AND EXISTS "${candidate}")
            file(SHA256 "${candidate}" have)
            if(have STREQUAL ICLFORGE_OPUS_SHA256)
                if(NOT candidate STREQUAL dest)
                    file(COPY_FILE "${candidate}" "${dest}")
                endif()
                message(STATUS "iclforge: libopus ${ICLFORGE_OPUS_VERSION} tarball taken from ${candidate}")
                return()
            endif()
            message(WARNING "iclforge: ${candidate} is not libopus ${ICLFORGE_OPUS_VERSION} "
                            "(SHA-256 ${have}, expected ${ICLFORGE_OPUS_SHA256}); it is not used.")
            file(REMOVE "${candidate}")
        endif()
    endforeach()
    get_filename_component(dest_dir "${dest}" DIRECTORY)
    file(MAKE_DIRECTORY "${dest_dir}")
    list(LENGTH ICLFORGE_OPUS_RETRY_DELAYS delays)
    set(round 1)
    set(fetched FALSE)
    while(NOT fetched)
        set(failures "")
        foreach(url IN LISTS ICLFORGE_OPUS_URLS)
            file(REMOVE "${dest}.part")
            # No EXPECTED_HASH here: file(DOWNLOAD) turns a failed transfer into a hard CMake error
            # when it is given one ("cannot compute hash on failed download", which is what failed
            # the 2026-11-09 configure and cannot be retried), and STATUS does not soften that. The
            # hash is checked below, on a transfer that completed, and a mismatch counts as a
            # failed try like any other.
            file(DOWNLOAD "${url}" "${dest}.part" TIMEOUT 120 INACTIVITY_TIMEOUT 30 STATUS opus_status)
            list(GET opus_status 0 opus_code)
            if(opus_code EQUAL 0)
                file(SHA256 "${dest}.part" have)
                if(have STREQUAL ICLFORGE_OPUS_SHA256)
                    file(RENAME "${dest}.part" "${dest}")
                    set(fetched TRUE)
                    set(from "${url}")
                    break()
                endif()
                set(opus_status "${opus_code};SHA-256 ${have} is not the pinned ${ICLFORGE_OPUS_SHA256}")
            endif()
            file(REMOVE "${dest}.part")
            string(APPEND failures "
  ${url}: ${opus_status}")
            message(STATUS "iclforge: libopus download from ${url} failed (${opus_status})")
        endforeach()
        if(fetched)
            break()
        endif()
        if(round GREATER_EQUAL ICLFORGE_OPUS_ATTEMPTS)
            message(FATAL_ERROR
                "iclforge: libopus ${ICLFORGE_OPUS_VERSION} could not be fetched from "
                "${ICLFORGE_OPUS_URLS} after ${round} rounds (last status of each:${failures}). "
                "Extract it somewhere and set "
                "ICLFORGE_OPUS_SOURCE_DIR to the directory that holds include/opus.h.")
        endif()
        # The pause after round N is the Nth delay, or the last one when the list is shorter.
        set(pause 0)
        if(delays GREATER 0)
            math(EXPR idx "${round} - 1")
            if(idx GREATER_EQUAL delays)
                math(EXPR idx "${delays} - 1")
            endif()
            list(GET ICLFORGE_OPUS_RETRY_DELAYS ${idx} pause)
        endif()
        message(STATUS "iclforge: libopus round ${round} of ${ICLFORGE_OPUS_ATTEMPTS} failed on every "
                       "URL; trying again in ${pause} s")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${pause})
        math(EXPR round "${round} + 1")
    endwhile()
    message(STATUS "iclforge: libopus ${ICLFORGE_OPUS_VERSION} tarball downloaded from ${from} (round ${round})")
    # Leave a verified copy where a cache can pick it up.
    if(NOT seed STREQUAL "" AND NOT seed STREQUAL dest)
        file(MAKE_DIRECTORY "${ICLFORGE_OPUS_TARBALL_DIR}")
        file(COPY_FILE "${dest}" "${seed}")
    endif()
endfunction()

# Script mode (`cmake -P opus.cmake`): fill ICLFORGE_OPUS_TARBALL_DIR and stop.
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if(ICLFORGE_OPUS_TARBALL_DIR STREQUAL "")
        message(FATAL_ERROR "iclforge: script mode needs -DICLFORGE_OPUS_TARBALL_DIR=<directory>.")
    endif()
    iclforge_opus_obtain("${ICLFORGE_OPUS_TARBALL_DIR}/opus-${ICLFORGE_OPUS_VERSION}.tar.gz")
    return()
endif()

if(ICLFORGE_OPUS_SOURCE_DIR STREQUAL "")
    set(_opus_dl "${CMAKE_BINARY_DIR}/iclforge_opus")
    set(_opus_tree "${_opus_dl}/opus-${ICLFORGE_OPUS_VERSION}")
    if(NOT EXISTS "${_opus_tree}/include/opus.h")
        file(MAKE_DIRECTORY "${_opus_dl}")
        iclforge_opus_obtain("${_opus_dl}/opus.tar.gz")
        file(ARCHIVE_EXTRACT INPUT "${_opus_dl}/opus.tar.gz" DESTINATION "${_opus_dl}"
             PATTERNS "opus-${ICLFORGE_OPUS_VERSION}/celt/*" "opus-${ICLFORGE_OPUS_VERSION}/silk/*"
                      "opus-${ICLFORGE_OPUS_VERSION}/src/*" "opus-${ICLFORGE_OPUS_VERSION}/include/*"
                      "opus-${ICLFORGE_OPUS_VERSION}/COPYING")
    endif()
    set(_opus_root "${_opus_tree}")
else()
    set(_opus_root "${ICLFORGE_OPUS_SOURCE_DIR}")
endif()
if(NOT EXISTS "${_opus_root}/include/opus.h")
    message(FATAL_ERROR "iclforge: no libopus at '${_opus_root}': include/opus.h is not there.")
endif()

set(ICLFORGE_OPUS_CELT
    celt/bands.c
    celt/celt.c
    celt/celt_encoder.c
    celt/celt_decoder.c
    celt/cwrs.c
    celt/entcode.c
    celt/entdec.c
    celt/entenc.c
    celt/kiss_fft.c
    celt/laplace.c
    celt/mathops.c
    celt/mdct.c
    celt/modes.c
    celt/pitch.c
    celt/celt_lpc.c
    celt/quant_bands.c
    celt/rate.c
    celt/vq.c
)

set(ICLFORGE_OPUS_SILK
    silk/CNG.c
    silk/code_signs.c
    silk/init_decoder.c
    silk/decode_core.c
    silk/decode_frame.c
    silk/decode_parameters.c
    silk/decode_indices.c
    silk/decode_pulses.c
    silk/decoder_set_fs.c
    silk/dec_API.c
    silk/enc_API.c
    silk/encode_indices.c
    silk/encode_pulses.c
    silk/gain_quant.c
    silk/interpolate.c
    silk/LP_variable_cutoff.c
    silk/NLSF_decode.c
    silk/NSQ.c
    silk/NSQ_del_dec.c
    silk/PLC.c
    silk/shell_coder.c
    silk/tables_gain.c
    silk/tables_LTP.c
    silk/tables_NLSF_CB_NB_MB.c
    silk/tables_NLSF_CB_WB.c
    silk/tables_other.c
    silk/tables_pitch_lag.c
    silk/tables_pulses_per_block.c
    silk/VAD.c
    silk/control_audio_bandwidth.c
    silk/quant_LTP_gains.c
    silk/VQ_WMat_EC.c
    silk/HP_variable_cutoff.c
    silk/NLSF_encode.c
    silk/NLSF_VQ.c
    silk/NLSF_unpack.c
    silk/NLSF_del_dec_quant.c
    silk/process_NLSFs.c
    silk/stereo_LR_to_MS.c
    silk/stereo_MS_to_LR.c
    silk/check_control_input.c
    silk/control_SNR.c
    silk/init_encoder.c
    silk/control_codec.c
    silk/A2NLSF.c
    silk/ana_filt_bank_1.c
    silk/biquad_alt.c
    silk/bwexpander_32.c
    silk/bwexpander.c
    silk/debug.c
    silk/decode_pitch.c
    silk/inner_prod_aligned.c
    silk/lin2log.c
    silk/log2lin.c
    silk/LPC_analysis_filter.c
    silk/LPC_inv_pred_gain.c
    silk/table_LSF_cos.c
    silk/NLSF2A.c
    silk/NLSF_stabilize.c
    silk/NLSF_VQ_weights_laroia.c
    silk/pitch_est_tables.c
    silk/resampler.c
    silk/resampler_down2_3.c
    silk/resampler_down2.c
    silk/resampler_private_AR2.c
    silk/resampler_private_down_FIR.c
    silk/resampler_private_IIR_FIR.c
    silk/resampler_private_up2_HQ.c
    silk/resampler_rom.c
    silk/sigm_Q15.c
    silk/sort.c
    silk/sum_sqr_shift.c
    silk/stereo_decode_pred.c
    silk/stereo_encode_pred.c
    silk/stereo_find_predictor.c
    silk/stereo_quant_pred.c
    silk/LPC_fit.c
)

set(ICLFORGE_OPUS_SILK_FIXED
    silk/fixed/LTP_analysis_filter_FIX.c
    silk/fixed/LTP_scale_ctrl_FIX.c
    silk/fixed/corrMatrix_FIX.c
    silk/fixed/encode_frame_FIX.c
    silk/fixed/find_LPC_FIX.c
    silk/fixed/find_LTP_FIX.c
    silk/fixed/find_pitch_lags_FIX.c
    silk/fixed/find_pred_coefs_FIX.c
    silk/fixed/noise_shape_analysis_FIX.c
    silk/fixed/process_gains_FIX.c
    silk/fixed/regularize_correlations_FIX.c
    silk/fixed/residual_energy16_FIX.c
    silk/fixed/residual_energy_FIX.c
    silk/fixed/warped_autocorrelation_FIX.c
    silk/fixed/apply_sine_window_FIX.c
    silk/fixed/autocorr_FIX.c
    silk/fixed/burg_modified_FIX.c
    silk/fixed/k2a_FIX.c
    silk/fixed/k2a_Q16_FIX.c
    silk/fixed/pitch_analysis_core_FIX.c
    silk/fixed/vector_ops_FIX.c
    silk/fixed/schur64_FIX.c
    silk/fixed/schur_FIX.c
)

set(ICLFORGE_OPUS_API
    src/opus.c
    src/opus_decoder.c
    src/opus_encoder.c
    src/extensions.c
    src/repacketizer.c
)

set(_opus_files "")
foreach(_f IN LISTS ICLFORGE_OPUS_CELT ICLFORGE_OPUS_SILK ICLFORGE_OPUS_SILK_FIXED ICLFORGE_OPUS_API)
    list(APPEND _opus_files "${_opus_root}/${_f}")
endforeach()

target_sources(${COMPONENT_LIB} PRIVATE ${_opus_files})
# Per source, so that nothing of libopus's configuration reaches this component's own code: the
# definitions it builds with, -O2 as the decoder is built elsewhere in this component, and no
# warnings from code that is not ours to fix.
set_source_files_properties(${_opus_files} PROPERTIES
    COMPILE_DEFINITIONS "OPUS_BUILD;FIXED_POINT=1;VAR_ARRAYS;DISABLE_FLOAT_API;PACKAGE_VERSION=\"${ICLFORGE_OPUS_VERSION}\""
    COMPILE_OPTIONS "-O2;-w"
    INCLUDE_DIRECTORIES "${_opus_root};${_opus_root}/include;${_opus_root}/celt;${_opus_root}/silk;${_opus_root}/silk/fixed;${_opus_root}/src")
# The one header this component's own opus_stream.cpp includes.
target_include_directories(${COMPONENT_LIB} PRIVATE "${_opus_root}/include")
