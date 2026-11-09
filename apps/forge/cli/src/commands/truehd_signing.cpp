#include "truehd_signing.hpp"

#include <fmt/base.h>
#include <fmt/format.h>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../support.hpp"
#include "iclforge/base/crypto/signing_key.hpp"
#include "iclforge/truehd/signing/evolution_signer.hpp"

namespace forge_cli::commands {

namespace {

namespace sg = iclforge::truehd::signing;

// "key 1 (a.key) 40, key 2 (b.key) 10": which of several keys the units were signed under. Empty
// for one key, where there is nothing to tell apart.
std::string per_key_breakdown(const Options& meta, std::span<const int> per_key) {
    if (per_key.size() < 2) {
        return {};
    }
    std::string out;
    for (std::size_t i = 0; i < per_key.size(); ++i) {
        const std::string_view name = i < meta.signing_keys.size()
                                          ? std::string_view{meta.signing_keys[i]}
                                          : std::string_view{};
        out += fmt::format("{}key {}{} {}", out.empty() ? "" : ", ", i + 1,
                           name.empty() ? std::string{} : fmt::format(" ({})", name), per_key[i]);
    }
    return out;
}

}  // namespace

std::optional<int> apply_truehd_object_signing(std::span<std::byte> stream, const Options& meta,
                                               FILE* status) {
    if (!meta.sign_objects) {
        return 0;
    }
    // Verifying takes a keyring; signing is one key, and naming two would leave it unsaid which of
    // them wrote the tag.
    if (meta.signing_keys.size() > 1) {
        fmt::println(stderr,
                     "error: sign-objects signs with one key, and signing-key= was given {} times",
                     meta.signing_keys.size());
        return std::nullopt;
    }
    const auto key = iclforge::base::crypto::load_signing_key(
        meta.signing_keys.empty() ? std::string_view{}
                                  : std::string_view{meta.signing_keys.front()});
    if (!key.has_value()) {
        if (key.error().kind == iclforge::base::crypto::KeyErrorKind::kAbsent) {
            fmt::println(stderr,
                         "error: sign-objects needs a key - pass signing-key=<path>, or set "
                         "ICLFORGE_SIGNING_KEY_FILE / ICLFORGE_SIGNING_KEY");
        } else {
            fmt::println(stderr, "error: {}", key.error().message);
        }
        return std::nullopt;
    }
    const int signed_count = sg::sign_evolution_stream(stream, *key);
    status_println(status, "  object signature: {} evolution frame(s) signed", signed_count);
    if (signed_count == 0) {
        fmt::println(stderr,
                     "warning: sign-objects found no evolution frame to sign: this stream has no "
                     "object layer, or none that the tag can be written into");
    }
    return signed_count;
}

std::optional<sg::VerifySummary> apply_truehd_object_verification(std::span<const std::byte> stream,
                                                                  const Options& meta,
                                                                  FILE* status) {
    if (!meta.verify_objects) {
        return sg::VerifySummary{};
    }
    const auto keys = load_object_keyring(meta, "verify-objects");
    if (!keys.has_value()) {
        return std::nullopt;
    }
    const sg::KeyringSummary result = sg::verify_evolution_stream_any(stream, *keys);
    const sg::VerifySummary& summary = result.totals;
    const std::string breakdown = per_key_breakdown(meta, result.per_key);
    status_println(status, "  object signature: {} valid{}, {} mismatched, {} unit(s) with none",
                   summary.valid, breakdown.empty() ? std::string{} : " (" + breakdown + ")",
                   summary.mismatch, summary.no_container);
    if (summary.mismatch > 0) {
        fmt::println(stderr,
                     "error: object signature verification failed ({} of {} evolution frames did "
                     "not match {})",
                     summary.mismatch, summary.valid + summary.mismatch,
                     keys->size() == 1 ? "the supplied key" : "any of the supplied keys");
        return std::nullopt;
    }
    return summary;
}

std::optional<TruehdGate> apply_truehd_object_gate(std::span<const std::byte> stream,
                                                   const Options& meta, FILE* status) {
    const auto keys = load_object_keyring(meta, "gate-objects");
    if (!keys.has_value()) {
        return std::nullopt;
    }
    auto gated = sg::gate_evolution_stream(stream, *keys);
    if (!gated.has_value()) {
        fmt::println(stderr,
                     "error: gate-objects: {} - refusing to play an object layer that could be "
                     "neither verified nor removed",
                     gated.error().kind == sg::GateErrorKind::kNoStream
                         ? std::string{"no access unit reads as TrueHD"}
                         : fmt::format("{} byte(s) between or after the access units do not read "
                                       "as one",
                                       gated.error().skipped_bytes));
        return std::nullopt;
    }
    const sg::GateSummary& summary = gated->summary;
    const std::string breakdown = per_key_breakdown(meta, summary.per_key);
    status_println(status,
                   "  object gate: {} unit(s) kept their objects{}, {} played as the bed, {} had "
                   "none",
                   summary.passed, breakdown.empty() ? std::string{} : " (" + breakdown + ")",
                   summary.gated, summary.no_objects);
    return TruehdGate{std::move(gated->bytes), summary.gated};
}

}  // namespace forge_cli::commands
