#!/usr/bin/env bash
# The vcpkg TOOL binary (not the ports it builds): where setup-vcpkg gets it.
#
# bootstrap-vcpkg.{sh,bat} downloads it from
#   https://github.com/microsoft/vcpkg-tool/releases/download/<tag>/<asset>
# on every run, with a retry that gives up after about eight seconds, and
# nothing caches it (the vcpkg download cache is not even configured yet when
# the bootstrap runs). A 504 from that one URL therefore failed every job that
# uses the action at once - twenty-odd of them on 2026-11-09 - for something no
# commit had changed. This script is the two halves of the fix:
#
#   plan     reads the tool release tag (and, where the checkout has one, the
#            SHA-512 of this platform's binary) from scripts/vcpkg-tool-metadata.txt
#            in the vcpkg checkout, and prints KEY=VALUE lines for
#            $GITHUB_OUTPUT, among them the actions/cache key. The key is the
#            OS, the architecture and the tag - nothing else - so there is ONE
#            ~10 MB entry per platform per tool release, and a vcpkg pin bump
#            that keeps the tag adds nothing.
#   install  puts the tool in the checkout: from the cache when it is there and
#            verifies, otherwise through the stock bootstrap script run under a
#            retry with a real back-off (minutes, not seconds). It then copies
#            a freshly downloaded binary into the cache directory and says so
#            (fetched=true), so the workflow saves the entry right then rather
#            than in a post-job step a failed job would skip.
#
# A cached binary is never trusted on its say-so. Where the metadata pins a
# SHA-512 for the platform (Linux and macOS) the file must match it - exactly the
# check bootstrap-vcpkg.sh makes of a download. The Windows script
# (scripts/bootstrap.ps1) pins nothing, so there the file must match the SHA-512
# recorded beside it when this job saved it, which catches a truncated or
# damaged entry but is no defence against a poisoned one; that rests on GitHub's
# cache scoping, the same as every other cache in this repository. Either way
# the binary must then run (`vcpkg version`), and any failure falls back to the
# bootstrap, never to a build that carries on with a bad tool.
#
# Environment (all set by action.yml):
#   VCPKG_DIR         the vcpkg checkout
#   TOOL_CACHE_DIR    the directory actions/cache restores and saves
#   TOOL_TAG, TOOL_SHA512, TOOL_CACHE_HIT      (install only) from plan / the restore
#   VCPKG_TOOL_RETRY_DELAYS   seconds slept between bootstrap tries; the default
#                     is six tries over about four and a half minutes
set -euo pipefail

: "${VCPKG_DIR:?}"
: "${TOOL_CACHE_DIR:?}"

if [ "${RUNNER_OS:-}" = "Windows" ]; then
  exe="vcpkg.exe"
else
  exe="vcpkg"
fi
cached_file="$TOOL_CACHE_DIR/$exe"
cached_sha="$TOOL_CACHE_DIR/$exe.sha512"

# Outputs go straight to $GITHUB_OUTPUT (stdout when run by hand), so nothing the
# bootstrap prints can be mistaken for one.
emit() {
  echo "$1" >>"${GITHUB_OUTPUT:-/dev/stdout}"
}

sha512_of() {
  if command -v sha512sum >/dev/null 2>&1; then
    sha512sum "$1" | cut -d' ' -f1
  else
    shasum -a 512 "$1" | cut -d' ' -f1
  fi
}

metadata_value() {
  # Not `.`-sourced: this file comes out of a checkout, and a parse that only
  # ever sees one KEY=VALUE line cannot run anything else it contains.
  sed -n "s/^$1=//p" "$VCPKG_DIR/scripts/vcpkg-tool-metadata.txt" | tr -d '\r' | head -n 1
}

# The metadata variable naming this platform's binary, mirroring the branches
# of bootstrap-vcpkg.sh (glibc only: the musl and from-source paths are not
# ones a GitHub runner takes). Empty where the metadata pins nothing.
platform_sha_name() {
  case "${RUNNER_OS:-}" in
    macOS) echo VCPKG_MACOS_SHA ;;
    Linux)
      case "$(uname -m)" in
        x86_64) echo VCPKG_GLIBC_SHA ;;
        aarch64 | arm64) echo VCPKG_GLIBC_ARM64_SHA ;;
      esac
      ;;
  esac
}

plan() {
  local tag name sha=""
  tag="$(metadata_value VCPKG_TOOL_RELEASE_TAG)"
  if ! printf '%s' "$tag" | grep -Eq '^[0-9A-Za-z._-]+$'; then
    echo "::error::scripts/vcpkg-tool-metadata.txt has no usable VCPKG_TOOL_RELEASE_TAG (got '$tag')" >&2
    exit 1
  fi
  name="$(platform_sha_name)"
  if [ -n "$name" ]; then
    sha="$(metadata_value "$name")"
    if ! printf '%s' "$sha" | grep -Eq '^[0-9a-f]{128}$'; then
      echo "::error::scripts/vcpkg-tool-metadata.txt has no usable $name (got '$sha')" >&2
      exit 1
    fi
  fi
  emit "tag=$tag"
  emit "sha512=$sha"
  emit "key=vcpkg-tool-${RUNNER_OS:?}-${RUNNER_ARCH:?}-$tag"
}

# Succeeds only for a cached binary that matches its pin and runs.
use_cached() {
  local want actual
  [ -f "$cached_file" ] || return 1
  if [ -n "${TOOL_SHA512:-}" ]; then
    want="$TOOL_SHA512"
  elif [ -f "$cached_sha" ]; then
    want="$(tr -d ' \r\n' <"$cached_sha")"
  else
    echo "vcpkg tool cache: no SHA-512 to hold $exe to; not using it"
    return 1
  fi
  actual="$(sha512_of "$cached_file")"
  if [ "$actual" != "$want" ]; then
    echo "::warning title=vcpkg tool cache::cached $exe has SHA-512 $actual, expected $want; downloading it again"
    return 1
  fi
  cp "$cached_file" "$VCPKG_DIR/$exe"
  chmod +x "$VCPKG_DIR/$exe"
  if ! "$VCPKG_DIR/$exe" version --disable-metrics; then
    echo "::warning title=vcpkg tool cache::cached $exe does not run; downloading it again"
    rm -f "$VCPKG_DIR/$exe"
    return 1
  fi
  : >"$VCPKG_DIR/vcpkg.disable-metrics"
  return 0
}

bootstrap_with_retries() {
  local script delays delay attempt=1
  if [ "${RUNNER_OS:-}" = "Windows" ]; then
    script="$VCPKG_DIR/bootstrap-vcpkg.bat"
  else
    script="$VCPKG_DIR/bootstrap-vcpkg.sh"
  fi
  delays="${VCPKG_TOOL_RETRY_DELAYS:-10 20 40 80 120}"
  for delay in $delays ""; do
    rm -f "$VCPKG_DIR/$exe" "$VCPKG_DIR/$exe.part"
    echo "vcpkg tool: bootstrap attempt $attempt"
    if "$script" -disableMetrics; then
      return 0
    fi
    if [ -z "$delay" ]; then
      break
    fi
    echo "::warning title=vcpkg tool::bootstrap attempt $attempt failed (usually github.com answering 5xx for the release download); again in ${delay} s"
    sleep "$delay"
    attempt=$((attempt + 1))
  done
  echo "::error::bootstrapping vcpkg failed after $attempt attempts" >&2
  return 1
}

install() {
  : "${TOOL_TAG:?}"
  if [ "${TOOL_CACHE_HIT:-}" = "true" ] && use_cached; then
    echo "vcpkg tool: $TOOL_TAG restored from the cache"
    emit "fetched=false"
    return 0
  fi
  bootstrap_with_retries
  # Only a binary that was just downloaded and runs goes into the cache.
  rm -rf "$TOOL_CACHE_DIR"
  mkdir -p "$TOOL_CACHE_DIR"
  cp "$VCPKG_DIR/$exe" "$cached_file"
  sha512_of "$cached_file" >"$cached_sha"
  emit "fetched=true"
}

case "${1:-}" in
  plan) plan ;;
  install) install ;;
  *)
    echo "usage: vcpkg-tool.sh plan|install" >&2
    exit 2
    ;;
esac
