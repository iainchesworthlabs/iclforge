#!/usr/bin/env bash
set -euo pipefail
#
# Copied verbatim from the org-shared runner fleet repo, iainchesworthlabs/ci-runners
# (scripts/linux/02-gcc-toolchain.sh), so the fleet's baked toolchain and this repo's
# CI stay pinned to the same GCC — if you change the fallback logic here, port it
# back there too.
# Consumed by .github/workflows/_build.yml, which runs it with sudo on the
# GitHub-hosted *and* self-hosted Ubuntu runner (both land on the same
# ubuntu:26.04 container image; nothing is baked into a runner image, see
# docs/ci-self-hosted-runners.md).
#

GCC_VERSION=16

# apt-get update, then apt-get install of any packages named, in up to three
# attempts 30 s apart. Each attempt re-runs the update, so a retry reads a fresh
# index and may reach another mirror node: Ubuntu's mirrors fail a single attempt
# outright now and then (a pool 404 for a version the index names, or an index
# caught mid-sync), and apt's own Acquire::Retries treats a 404 as final. The
# workflows' own apt steps run the same loop inline. A function in each script
# rather than one shared file, because the fleet's Packer build uploads and runs
# each of these scripts on its own.
apt_retry() {
    local attempt=1
    until apt-get update && { [ "$#" -eq 0 ] || apt-get install -y --no-install-recommends "$@"; }
    do
        echo "::warning title=apt-get::attempt $attempt of 3 failed"
        [ "$attempt" -lt 3 ] || return 1
        attempt=$((attempt + 1))
        sleep 30
    done
}

# Register the ubuntu-toolchain-r/test PPA by writing its apt source and its
# signing key straight into /etc/apt, instead of `add-apt-repository ppa:...`.
# That shortcut is resolved through Launchpad's API (lazr.restfulclient), a
# different service from the archive the packages come from
# (ppa.launchpadcontent.net), and the API answers 503 for minutes at a time now
# and then (2026-09-16: the AppImage leg, whose 22.04 base can only get GCC 16
# from this PPA, failed after a three-minute outage; a retry that waits about
# seven and a half minutes narrowed that without removing it). With the key
# pinned here nothing but the archive is needed.
#
# The key is the PPA's signing_key_fingerprint on Launchpad
# (https://api.launchpad.net/1.0/~ubuntu-toolchain-r/+archive/ubuntu/test):
#   C8EC 952E 2A0E 1FBD C509  0F6A 2C27 7A0A 3521 54E5
# RSA-4096, created 2024-05-02, no expiry, and it signs the archive's InRelease.
# Launchpad still also signs that file with the PPA's old 1024-bit key
# (60C3 1780 3A41 BA51 845E  371A 1E93 77A2 BA9E F27F); apt needs one good
# signature, so only the strong key is trusted here. If Launchpad rotates the
# key, `apt-get update` fails with NO_PUBKEY for the new id: take the new
# fingerprint from the API page above, then
#   gpg --keyserver keyserver.ubuntu.com --recv-keys <FPR>
#   gpg --export --export-options export-minimal --armor <FPR>
# and replace the block below (and its copy in ci-runners).
add_toolchain_ppa() {
    local codename keyring=/etc/apt/keyrings/ubuntu-toolchain-r-test.asc
    # shellcheck source=/dev/null
    codename=$(. /etc/os-release && printf '%s' "${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}")
    [ -n "$codename" ] || { echo "ERROR: no Ubuntu codename in /etc/os-release"; return 1; }

    install -d -m 0755 /etc/apt/keyrings
    cat > "$keyring" <<'KEY'
-----BEGIN PGP PUBLIC KEY BLOCK-----

mQINBGYzeoEBEADEVzm0fPTo1KfunTjKV5ZP0Mb86Z6izCXSYbeYt/iAaIfK7a8x
c13I/uAOxzjcuZHD4sQh7/22I74ignCSexoJD3PnytpZtAxNjyj3jkJpIrdFOVd4
Cmua5tnEI1Av+B21YLlYYftK2wR+Z6hpsz3NyQ8URR1SCX04BOV9AGxcWb7izkV9
RHu6xR4x8kAjuTObhhqw/ZR73+U9mig785qmrtapQkF/IanYj2OofqUJwosDo8YA
shi01735JP9s5do4nnsb6w6xcYQ0HlGn3AxR1k4WA2CK/90QybOmVbLz7IOF3Ou3
cPfoDzSxAiQemge2msJWPaQLSy/HPUivPB2uNPxkcNIHUVwbjgIig0yaN0EOsveg
o1m6QvS8sm1MEA2ySl+9+q8wdqTnXbSgA3hq/5OoG4rvHD9GUAd6oRxbz/wdOU3J
7BlAhEMAHgeQ9DyIzRP9lGeU3QnWNA+WFiiDrLljf+6xewe5iyqLxPqvCkFUDPCF
emp2oIqeWf6l04IMtM4SJz2AnU1EvytprEEvntMvHMXlBBinw1Q5r3ES/QlPfWr4
o5CUpu2P3GpZtflDe7jvccyJUMDtlqOVv7wBZPMZykjTAPQ16clGdtIqWdQSk3/b
UA96RSdpM4gMGXFBGLBoAd7CptafRvPjpRnbMxs2jIxicnASfwJALEFgbQARAQAB
tD9MYXVuY2hwYWQgUFBBIGZvciBQUEEgZm9yIFVidW50dSBUb29sY2hhaW4gVXBs
b2FkcyAocmVzdHJpY3RlZCmJAk4EEwEKADgWIQTI7JUuKg4fvcUJD2osJ3oKNSFU
5QUCZjN6gQIbAwULCQgHAgYVCgkICwIEFgIDAQIeAQIXgAAKCRAsJ3oKNSFU5TkT
EACpFJ+q0xqfAmRF1886cgtVw+ruJTqfsxdj7ms1os3/I5VXLhPMbUg56mtK9Okj
0Nf9rY5bnDagJobjtS2JgzMbYWXocPJuwK6ZvmMD5tFSsTov19R3SyIT2x3JAy7S
/LNWqVeZS41RtM8uSu56PhghE7aAP9PigFTI3CUargpx1DQh6xiHKRR57ZMrLcd4
l6tj8yb4Hl+OeFRQMcoim696lPMSqzVtJ0epP7yNsz2LFaRmkRP/hf+S4JlSs8uC
wzAl9eJR2h1v1WoBr0pAW2gjjd8VyKWhaKjneicJPqo5GDvhDl+lbrpqSwSHENlH
DVyC+XvAjvTf4JhTQE/M1L/sGrqqa65n+WF4ZqauiDvn1bUw0vnL0oaiK/hyH1YG
XW7khJ7JDlw/6LFNZ6Ih0UuVRTzVBBBz3Sipw5hgM9ztKEYLMI5zlwWGlhEjMnxD
X5WauIUWjPv3dKQp1qTthN7Fgv3/2J6j+2/tMAfWeWB1tL8H4lVPUCfcpQpkTP/E
HTnB3oaRiYy6GDuGKsNkhEUvlV2AnVAxO17IN5QQ2lMQ261zaPj9y9v1oCQlzGfm
UKRJWFGbsooapdm7DTrqrfO+zyRXWPe6GRnYlmeXTRWi8CEIXpCkhhf8rma8KNnW
S96jsTSU97Lo0hnIbL+4o3qf2BiYcfIwfoIXzUaj5ajGEA==
=CmYA
-----END PGP PUBLIC KEY BLOCK-----
KEY
    # apt reads the key as its _apt user: do not leave it at a restrictive umask.
    chmod 0644 "$keyring"

    cat > /etc/apt/sources.list.d/ubuntu-toolchain-r-test.sources <<EOF
Types: deb
URIs: https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu
Suites: ${codename}
Components: main
Signed-By: ${keyring}
EOF
    apt_retry
}

echo "==> Installing GCC ${GCC_VERSION} toolchain"

# Ubuntu 26.04 LTS (Resolute Raccoon) ships gcc-16/g++-16, but in the
# `universe` component rather than `main` (unlike gcc-15, which was in main
# outright) — and the official ubuntu:26.04 container image only enables
# `main` by default, so enable universe first and re-probe before falling
# back to the ubuntu-toolchain-r PPA, which exists for an older base whose
# own archive (main or universe) caps out below GCC 16 entirely.
if ! apt-cache show "gcc-${GCC_VERSION}" >/dev/null 2>&1; then
    echo "==> gcc-${GCC_VERSION} not in default repos, enabling the universe component"
    add-apt-repository -y universe
    apt_retry
fi

if ! apt-cache show "gcc-${GCC_VERSION}" >/dev/null 2>&1; then
    echo "==> gcc-${GCC_VERSION} still not found, adding ubuntu-toolchain-r PPA (pinned key, no Launchpad API)"
    add_toolchain_ppa
fi

apt_retry \
    "gcc-${GCC_VERSION}" \
    "g++-${GCC_VERSION}"

# Set up alternatives so gcc/g++/gcov all point to the correct version
update-alternatives --install /usr/bin/gcc gcc "/usr/bin/gcc-${GCC_VERSION}" "${GCC_VERSION}0" \
    --slave /usr/bin/g++ g++ "/usr/bin/g++-${GCC_VERSION}" \
    --slave /usr/bin/gcov gcov "/usr/bin/gcov-${GCC_VERSION}"
update-alternatives --set gcc "/usr/bin/gcc-${GCC_VERSION}"

# Verify
echo "==> GCC $(gcc --version | head -1) installed"
echo "==> gcov $(gcov --version | head -1) installed"
gcc --version | head -1 | grep -q "${GCC_VERSION}" || { echo "ERROR: gcc version mismatch"; exit 1; }
gcov --version | head -1 | grep -q "${GCC_VERSION}" || { echo "ERROR: gcov version mismatch"; exit 1; }
