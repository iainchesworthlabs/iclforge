#!/usr/bin/env bash
#
# Builds the Qt side of the tree on a Linux machine that has no Qt: forge-gui, Crucible and Hearth,
# at the Qt version CI pins, outside the repository and without sudo for anything but apt.
#
# It does what .github/workflows/_ci-core.yml's coverage-qt job does with actions and vcpkg, for a
# machine where neither is available (a cloud sandbox whose proxy refuses GitHub's source archives,
# which is what vcpkg downloads, but serves release assets and raw.githubusercontent.com):
#
#   1. the apt packages Qt's prebuilt kit and the audio backends need;
#   2. aqtinstall at the commit .github/toolchain-versions.json pins, in a virtualenv, installing
#      Qt 6.12.0 with qttasktree, qtquick3d and qtshadertools (without qtshadertools Quick 3D is
#      found but unusable, CMake drops the Qt and font sections from Crucible's notices and one
#      Crucible test fails);
#   3. for Hearth, a private prefix: mbedTLS built from its release tarball, cpp-httplib's header
#      with the repository's patch applied, mdns.h, the CMake package files the project looks for,
#      the vcpkg-shaped licence and version files its notices read, and a toolchain file that does
#      nothing (so a configure that names vcpkg's variables does not need vcpkg).
#
# Every download is checked against a SHA-256 pinned below. Then it prints the configure line.
#
# Usage:
#   tools/dev/qt-sandbox-setup.sh [options] PREFIX
#
#   PREFIX          a directory outside the repository; created if absent. Holds kit/ (Qt), aqt-venv/
#                   and hearth/ (the private prefix).
#   --no-apt        do not run apt-get (the packages are installed already, or this is not root and
#                   has no sudo)
#   --no-qt         skip step 2
#   --no-hearth     skip step 3; the printed line then turns Hearth off
#   --no-mbedtls    in step 3, do not build mbedTLS (its tests need no compiler)
#   --jobs N        parallel jobs for the mbedTLS build (default: nproc)
#   --mirror DIR    take the pinned downloads from DIR instead of the network, under the names
#                   mbedtls-VERSION.tar.bz2, httplib.h, httplib-LICENSE, mdns.h and mdns-LICENSE;
#                   the checksums still apply
#   --print-only    print the configure line for PREFIX and do nothing else
#
# The tests of this script (tools/ci/test_qt_sandbox_setup.py) run the offline parts: the pins
# against .github/toolchain-versions.json and the repository's patch, and the package files
# --no-apt --no-qt --mirror writes.
#
# Exit status: 0 done, 1 a step failed, 2 bad usage.

set -euo pipefail

# Pinned downloads. The mbedTLS version is the one vcpkg's baseline (vcpkg.json builtin-baseline)
# resolves; httplib's comes from the overlay port, so it is read from there rather than repeated.
MBEDTLS_VERSION="3.6.5"
MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VERSION}/mbedtls-${MBEDTLS_VERSION}.tar.bz2"
MBEDTLS_SHA256="4a11f1777bb95bf4ad96721cac945a26e04bf19f57d905f241fe77ebeddf46d8"
MDNS_VERSION="1.4.3"
MDNS_URL="https://raw.githubusercontent.com/mjansson/mdns/${MDNS_VERSION}/mdns.h"
MDNS_SHA256="62ef311060b2db8e267e21d7a981e9c54d0698af781225c3c8e827cb84633a5d"
MDNS_LICENCE_URL="https://raw.githubusercontent.com/mjansson/mdns/${MDNS_VERSION}/LICENSE"
MDNS_LICENCE_SHA256="88d9b4eb60579c191ec391ca04c16130572d7eedc4a86daa58bf28c6e14c9bcd"
HTTPLIB_SHA256="1f99e51881c4c9d0649b27c611442c2f4d9bcfec5a22a14d5fcd1f8106f730b4"
HTTPLIB_LICENCE_SHA256="4b45cbe16d7b71b89ae6127e26e0d90a029198ca5e958ad8e3d0b8bbed364d8b"
TRIPLET="x64-linux-gcc"

APT_PACKAGES=(
    libegl1 libgl1 libxkbcommon0 libfontconfig1 libdbus-1-3 libglib2.0-0
    libgl-dev libegl-dev libxkbcommon-dev
    libasound2-dev libpipewire-0.3-dev libspa-0.2-dev
    libflac-dev libopus-dev
    ninja-build cmake build-essential curl bzip2 patch python3-venv
)

die() { echo "qt-sandbox-setup: $*" >&2; exit "${2:-1}"; }
step() { echo; echo "== $* =="; }

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
do_apt=1 do_qt=1 do_hearth=1 do_mbedtls=1 print_only=0 mirror="" jobs="$(nproc 2>/dev/null || echo 2)" prefix=""

while (($#)); do
    case "$1" in
        --no-apt) do_apt=0 ;;
        --no-qt) do_qt=0 ;;
        --no-hearth) do_hearth=0 ;;
        --no-mbedtls) do_mbedtls=0 ;;
        --print-only) print_only=1 ;;
        --jobs) shift; jobs="${1:?--jobs needs a number}" ;;
        --mirror) shift; mirror="${1:?--mirror needs a directory}" ;;
        -h | --help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//; /^set -euo/d'; exit 0 ;;
        -*) die "unknown option $1" 2 ;;
        *) [[ -z "$prefix" ]] || die "one PREFIX only" 2; prefix="$1" ;;
    esac
    shift
done
[[ -n "$prefix" ]] || die "usage: $0 [options] PREFIX" 2

# Resolved before anything is created, so a refused prefix leaves nothing behind. (cygpath exists
# only under Git Bash and MSYS, where the script's own checks are run by its tests.)
if command -v cygpath >/dev/null 2>&1; then prefix="$(cygpath -u -- "$prefix")"; fi
prefix="$(realpath -m -- "$prefix")"
case "$prefix" in
    "$repo_root" | "$repo_root"/*) die "PREFIX must be outside the repository ($repo_root)" 2 ;;
esac
mkdir -p "$prefix"

toolchain_versions="$repo_root/.github/toolchain-versions.json"
read_pin() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$toolchain_versions" "$1"; }
qt_version="$(read_pin qt)"
aqt_source="$(read_pin aqtsource)"
httplib_version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' \
    "$repo_root/cmake/vcpkg/ports/cpp-httplib/vcpkg.json")"
qt_kit="$prefix/kit/$qt_version/gcc_64"
hearth_prefix="$prefix/hearth"
vcpkg_installed="$hearth_prefix/vcpkg_installed"

print_configure_line() {
    local hearth=ON
    ((do_hearth)) || hearth=OFF
    local flags="-Wno-error=old-style-cast -Wno-error=array-bounds -Wno-error=stringop-overflow -Wno-error=maybe-uninitialized -Wno-error=null-dereference"
    local line="cmake -S $repo_root -B build-qt -G Ninja -DCMAKE_BUILD_TYPE=Release -DICLFORGE_QT_ROOT=$qt_kit"
    line+=" -DICLFORGE_BUILD_GUI=ON -DICLFORGE_BUILD_CRUCIBLE=ON -DICLFORGE_BUILD_HEARTH=$hearth"
    line+=" -DICLFORGE_WITH_ALSA=OFF -DICLFORGE_WITH_PIPEWIRE=ON"
    if ((do_hearth)); then
        line+=" -DCMAKE_PREFIX_PATH=$hearth_prefix -DICLFORGE_MDNS_INCLUDE_DIR=$hearth_prefix/include"
        line+=" -DVCPKG_INSTALLED_DIR=$vcpkg_installed -DVCPKG_TARGET_TRIPLET=$TRIPLET"
        line+=" -DCMAKE_TOOLCHAIN_FILE=$hearth_prefix/no-vcpkg-toolchain.cmake"
        flags+=" -isystem $hearth_prefix/include"
    fi
    echo "$line \"-DCMAKE_CXX_FLAGS=$flags\""
    echo
    echo "then, to test (offscreen, and PipeWire's client-config warnings off, since they break the"
    echo "forge tests that compare stderr exactly):"
    echo "  QT_QPA_PLATFORM=offscreen PIPEWIRE_DEBUG=0 ctest --test-dir build-qt --output-on-failure"
}

if ((print_only)); then
    print_configure_line
    exit 0
fi

# A pinned download: from the mirror when there is one, else the network, and in both cases only if
# the checksum matches. $1 file name under the mirror, $2 URL, $3 SHA-256, $4 destination.
fetch() {
    local name="$1" url="$2" sha="$3" dest="$4"
    if [[ -n "$mirror" ]]; then
        cp "$mirror/$name" "$dest" || die "$mirror/$name is not there"
    else
        curl --fail --silent --show-error --location --retry 3 --output "$dest" "$url" || die "could not download $url"
    fi
    echo "$sha  $dest" | sha256sum --check --status || die "$name does not match its pinned SHA-256 ($sha)"
}

install_apt() {
    step "apt packages"
    local sudo=""
    [[ "$(id -u)" == 0 ]] || sudo="sudo"
    export DEBIAN_FRONTEND=noninteractive
    $sudo apt-get update -qq
    $sudo apt-get install -y -qq --no-install-recommends "${APT_PACKAGES[@]}"
}

install_qt() {
    step "Qt $qt_version (aqtinstall at the pinned commit)"
    if [[ ! -x "$prefix/aqt-venv/bin/aqt" ]]; then
        python3 -m venv "$prefix/aqt-venv"
        # py7zr: the pinned aqtinstall needs 1.1.3 or later.
        "$prefix/aqt-venv/bin/pip" install --quiet "$aqt_source" "py7zr>=1.1.3"
    fi
    "$prefix/aqt-venv/bin/aqt" install-qt linux desktop "$qt_version" linux_gcc_64 \
        -O "$prefix/kit" -m qttasktree qtquick3d qtshadertools
    [[ -f "$qt_kit/lib/cmake/Qt6/Qt6Config.cmake" ]] || die "Qt $qt_version was not installed under $qt_kit"
    [[ -f "$qt_kit/lib/cmake/Qt6ShaderTools/Qt6ShaderToolsConfig.cmake" ]] ||
        die "qtshadertools is missing from $qt_kit: Quick 3D would be found and unusable"
}

# What vcpkg leaves for a port that the build reads back: share/<port>/copyright and a minimal
# vcpkg.spdx.json whose first package carries the version (notices/hearth/notices.cmake).
write_port_files() {
    local port="$1" version="$2" licence="$3"
    local dir="$vcpkg_installed/$TRIPLET/share/$port"
    mkdir -p "$dir"
    cp "$licence" "$dir/copyright"
    printf '{"packages":[{"name":"%s","versionInfo":"%s"}]}\n' "$port" "$version" >"$dir/vcpkg.spdx.json"
}

# The Debian copyright file of a package apt installed, or the first licence file the library's
# headers sit beside, as the texts for libflac and opus.
system_licence() {
    local package="$1" out="$2" f
    for f in "/usr/share/doc/$package/copyright"; do
        if [[ -f "$f" ]]; then cp "$f" "$out"; return 0; fi
    done
    # No Debian file (--no-apt on a minimal image): say where the text is rather than leave nothing.
    printf 'See the licence of the %s package installed on this machine.\n' "$package" >"$out"
}

write_package_files() {
    local cmake_dir="$hearth_prefix/lib/cmake"
    mkdir -p "$cmake_dir/httplib" "$cmake_dir/FLAC" "$cmake_dir/Opus"

    # cpp-httplib: header-only, and the project links httplib::httplib behind its own
    # iclforge_sendspin_httplib target (libs/sendspin/CMakeLists.txt).
    cat >"$cmake_dir/httplib/httplibConfig.cmake" <<EOF
if(NOT TARGET httplib::httplib)
    find_package(Threads REQUIRED)
    add_library(httplib::httplib INTERFACE IMPORTED)
    set_target_properties(httplib::httplib PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "$hearth_prefix/include")
    target_link_libraries(httplib::httplib INTERFACE Threads::Threads)
endif()
set(httplib_VERSION "$httplib_version")
EOF

    # FLAC and Opus: apt's libraries, in the shape of the targets vcpkg's packages define. The
    # headers are in the system include path, so no include directory is named.
    cat >"$cmake_dir/FLAC/FLACConfig.cmake" <<'EOF'
if(NOT TARGET FLAC::FLAC)
    find_library(_iclforge_flac_lib NAMES FLAC REQUIRED)
    add_library(FLAC::FLAC UNKNOWN IMPORTED)
    set_target_properties(FLAC::FLAC PROPERTIES IMPORTED_LOCATION "${_iclforge_flac_lib}")
endif()
EOF
    cat >"$cmake_dir/Opus/OpusConfig.cmake" <<'EOF'
if(NOT TARGET Opus::opus)
    find_library(_iclforge_opus_lib NAMES opus REQUIRED)
    find_path(_iclforge_opus_inc NAMES opus.h PATH_SUFFIXES opus REQUIRED)
    add_library(Opus::opus UNKNOWN IMPORTED)
    set_target_properties(Opus::opus PROPERTIES
        IMPORTED_LOCATION "${_iclforge_opus_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_iclforge_opus_inc}")
endif()
EOF

    # Nothing for vcpkg to do: the project's presets chainload a toolchain, and a plain configure
    # that names VCPKG_INSTALLED_DIR (for the notices) must not need one.
    cat >"$hearth_prefix/no-vcpkg-toolchain.cmake" <<'EOF'
# Deliberately empty: this prefix replaces vcpkg (tools/dev/qt-sandbox-setup.sh).
EOF
}

install_hearth_prefix() {
    step "Hearth's private prefix ($hearth_prefix)"
    mkdir -p "$hearth_prefix/include" "$hearth_prefix/lib" "$prefix/downloads"
    local dl="$prefix/downloads"

    if ((do_mbedtls)); then
        fetch "mbedtls-$MBEDTLS_VERSION.tar.bz2" "$MBEDTLS_URL" "$MBEDTLS_SHA256" "$dl/mbedtls.tar.bz2"
        rm -rf "$dl/mbedtls" && mkdir -p "$dl/mbedtls"
        tar -xjf "$dl/mbedtls.tar.bz2" -C "$dl/mbedtls" --strip-components=1
        # PSA Crypto is part of 3.6; the library is static, so the binary needs no loader path.
        cmake -S "$dl/mbedtls" -B "$dl/mbedtls-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$hearth_prefix" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
            -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null
        cmake --build "$dl/mbedtls-build" --parallel "$jobs" >/dev/null
        cmake --install "$dl/mbedtls-build" >/dev/null
        write_port_files mbedtls "$MBEDTLS_VERSION" "$dl/mbedtls/LICENSE"
    fi

    fetch httplib.h \
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/v$httplib_version/httplib.h" \
        "$HTTPLIB_SHA256" "$dl/httplib.h"
    fetch httplib-LICENSE \
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/v$httplib_version/LICENSE" \
        "$HTTPLIB_LICENCE_SHA256" "$dl/httplib-LICENSE"
    patch --silent -p1 "$dl/httplib.h" <"$repo_root/cmake/vcpkg/ports/cpp-httplib/wait-inside-a-frame.patch"
    cp "$dl/httplib.h" "$hearth_prefix/include/httplib.h"

    fetch mdns.h "$MDNS_URL" "$MDNS_SHA256" "$dl/mdns.h"
    fetch mdns-LICENSE "$MDNS_LICENCE_URL" "$MDNS_LICENCE_SHA256" "$dl/mdns-LICENSE"
    cp "$dl/mdns.h" "$hearth_prefix/include/mdns.h"

    write_package_files

    # Versions as installed by apt, for the notices; the licence texts are the distributions' own.
    local flac_version opus_version
    flac_version="$(dpkg-query -W -f='${Version}' libflac-dev 2>/dev/null | sed 's/[-+~].*//' || true)"
    opus_version="$(dpkg-query -W -f='${Version}' libopus-dev 2>/dev/null | sed 's/[-+~].*//' || true)"
    system_licence libflac-dev "$dl/flac-copyright"
    system_licence libopus-dev "$dl/opus-copyright"
    write_port_files cpp-httplib "$httplib_version" "$dl/httplib-LICENSE"
    write_port_files mdns "$MDNS_VERSION" "$dl/mdns-LICENSE"
    write_port_files libflac "${flac_version:-0}" "$dl/flac-copyright"
    write_port_files opus "${opus_version:-0}" "$dl/opus-copyright"
}

if ((do_apt)); then install_apt; fi
if ((do_qt)); then install_qt; fi
if ((do_hearth)); then install_hearth_prefix; fi

step "configure line"
print_configure_line
