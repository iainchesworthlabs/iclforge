#include "associated_mix.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/decoder/associated_service.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "stream_playback.hpp"

namespace iclforge::apps {

namespace {

[[nodiscard]] std::size_t length_of(const iclforge::ac3::DecodedAccessUnit& unit) {
    return unit.channels.empty() ? 0 : unit.channels.front().size();
}

// The first `count` samples of every channel, split off the front of `unit`.
[[nodiscard]] iclforge::ac3::DecodedAccessUnit split_head(iclforge::ac3::DecodedAccessUnit& unit,
                                                          std::size_t count) {
    iclforge::ac3::DecodedAccessUnit head = unit;
    for (std::size_t ch = 0; ch < unit.channels.size(); ++ch) {
        head.channels[ch].resize(count);
        unit.channels[ch].erase(unit.channels[ch].begin(),
                                unit.channels[ch].begin() + static_cast<std::ptrdiff_t>(count));
    }
    return head;
}

}  // namespace

std::expected<AssociatedChoice, AssociatedRefusal> choose_associated(
    std::span<const std::byte> stream, int main, const AssociatedRequest& request) {
    const auto scanned = iclforge::ac3::io::scan(stream);
    if (!scanned.has_value()) {
        return std::unexpected(AssociatedRefusal{
            .reason = AssociatedRefusal::Reason::kUnreadable,
            .main = main,
            .detail = std::string{iclforge::ac3::io::describe(scanned.error())}});
    }
    const auto& programmes = scanned->programmes;
    const auto find = [&](int id) -> const iclforge::ac3::io::ScannedProgramme* {
        for (const auto& programme : programmes) {
            if (programme.substreamid == id) {
                return &programme;
            }
        }
        return nullptr;
    };
    if (const auto* lead = find(main);
        lead != nullptr && lead->acmod == iclforge::ac3::Acmod::kDualMono) {
        return std::unexpected(
            AssociatedRefusal{.reason = AssociatedRefusal::Reason::kDualMonoMain, .main = main});
    }
    const iclforge::ac3::io::ScannedProgramme* chosen = nullptr;
    if (request.programme.has_value()) {
        const int id = *request.programme;
        if (id == main) {
            return std::unexpected(AssociatedRefusal{
                .reason = AssociatedRefusal::Reason::kIsTheMain, .main = main, .requested = id});
        }
        chosen = find(id);
        if (chosen == nullptr) {
            return std::unexpected(AssociatedRefusal{
                .reason = AssociatedRefusal::Reason::kNotCarried, .main = main, .requested = id});
        }
    } else {
        for (const auto& programme : programmes) {
            if (programme.substreamid != main && request.bsmod.has_value() &&
                programme.bsmod == *request.bsmod) {
                chosen = &programme;
                break;
            }
        }
        if (chosen == nullptr) {
            AssociatedRefusal refusal{.reason = AssociatedRefusal::Reason::kNoSuchService,
                                      .main = main};
            for (const auto& programme : programmes) {
                refusal.carried.push_back({.id = programme.substreamid,
                                           .bsmod = programme.bsmod,
                                           .acmod = programme.acmod});
            }
            return std::unexpected(std::move(refusal));
        }
    }
    return AssociatedChoice{.id = chosen->substreamid,
                            .bsmod = chosen->bsmod,
                            .acmod = chosen->acmod,
                            .lfe = chosen->lfe};
}

void MixReport::observe(const iclforge::ac3::AssociatedServiceMixResult& result) {
    if (units == 0) {
        main_min = main_max = result.main_gain_db;
        associated_min = associated_max = result.associated_gain_db;
        panmean = result.panmean;
    } else {
        main_min = std::min(main_min, result.main_gain_db);
        main_max = std::max(main_max, result.main_gain_db);
        associated_min = std::min(associated_min, result.associated_gain_db);
        associated_max = std::max(associated_max, result.associated_gain_db);
    }
    ++units;
}

void UnitPairing::push_main(iclforge::ac3::DecodedAccessUnit unit) {
    main_.push_back(std::move(unit));
}

void UnitPairing::push_associated(iclforge::ac3::DecodedAccessUnit unit) {
    associated_.push_back(std::move(unit));
}

std::expected<std::optional<iclforge::ac3::DecodedAccessUnit>, AssociatedMixError>
UnitPairing::next() {
    // One mixed unit, or the wait for a partner, or the end of the queue.
    // Looped rather than recursed: a unit with nothing in it is dropped and
    // the next one tried.
    while (!main_.empty()) {
        if (associated_.empty()) {
            if (!associated_done_) {
                return std::optional<iclforge::ac3::DecodedAccessUnit>{};  // its next is still to come
            }
            // The service has ended; the rest of the programme is the main
            // alone.
            auto alone = std::move(main_.front());
            main_.pop_front();
            return std::optional<iclforge::ac3::DecodedAccessUnit>{std::move(alone)};
        }
        auto& main_unit = main_.front();
        auto& associated_unit = associated_.front();
        const auto main_length = length_of(main_unit);
        const auto associated_length = length_of(associated_unit);
        if (main_length == 0 || associated_length == 0) {
            // Nothing to mix (a unit with no channels): drop the empty one.
            (main_length == 0 ? main_ : associated_).pop_front();
            continue;
        }
        const auto mix = [&](iclforge::ac3::DecodedAccessUnit& into,
                             const iclforge::ac3::DecodedAccessUnit& from)
            -> std::expected<void, AssociatedMixError> {
            const auto mixed = mixer_.mix(into, from);
            if (!mixed.has_value()) {
                return std::unexpected(AssociatedMixError{
                    .stage = AssociatedMixError::Stage::kMix,
                    .associated = associated_programme_,
                    .main = main_programme_,
                    .reason = std::string{iclforge::ac3::describe(mixed.error())}});
            }
            report_.observe(*mixed);
            return {};
        };
        if (main_length == associated_length) {
            if (auto done = mix(main_unit, associated_unit); !done.has_value()) {
                return std::unexpected(std::move(done.error()));
            }
            auto out = std::move(main_unit);
            main_.pop_front();
            associated_.pop_front();
            return std::optional<iclforge::ac3::DecodedAccessUnit>{std::move(out)};
        }
        if (main_length > associated_length) {
            auto head = split_head(main_unit, associated_length);
            if (auto done = mix(head, associated_unit); !done.has_value()) {
                return std::unexpected(std::move(done.error()));
            }
            associated_.pop_front();
            return std::optional<iclforge::ac3::DecodedAccessUnit>{std::move(head)};
        }
        const auto head = split_head(associated_unit, main_length);
        if (auto done = mix(main_unit, head); !done.has_value()) {
            return std::unexpected(std::move(done.error()));
        }
        auto out = std::move(main_unit);
        main_.pop_front();
        return std::optional<iclforge::ac3::DecodedAccessUnit>{std::move(out)};
    }
    return std::optional<iclforge::ac3::DecodedAccessUnit>{};
}

iclforge::ac3::DecoderConfig AssociatedMix::service_config(
    const iclforge::ac3::DecoderConfig& main, int programme) {
    iclforge::ac3::DecoderConfig config;
    config.drc_scale = main.drc_scale;
    config.drc_boost_scale = main.drc_boost_scale;
    config.fast_imdct = main.fast_imdct;
    config.heavy_compression = main.heavy_compression;
    config.output = main.output;
    config.output.target = iclforge::ac3::DownmixTarget::kAsCoded;
    config.concealment = main.concealment;
    config.fast_mdct = main.fast_mdct;
    config.programme = programme;
    config.skip_object_reconstruction = true;
    return config;
}

AssociatedMix::AssociatedMix(const iclforge::ac3::DecoderConfig& main_config, int main_programme,
                             const AssociatedChoice& choice,
                             std::vector<std::span<const std::byte>> units, double trim_db)
    : decoder_(std::make_unique<iclforge::ac3::Eac3Decoder>(
          service_config(main_config, choice.id))),
      units_(std::move(units)),
      choice_(choice),
      main_programme_(main_programme),
      pairing_({.main_fold = main_config.output.target,
                .associated_fold = iclforge::ac3::DownmixTarget::kAsCoded,
                .associated_trim_db = trim_db},
               main_programme, choice.id) {}

std::expected<void, AssociatedMixError> AssociatedMix::advance() {
    if (done_) {
        return {};
    }
    if (next_unit_ >= units_.size()) {
        // The service ran out of access units before the main did.
        release_held();
        return {};
    }
    auto heard = decoder_->decode_access_unit(units_[next_unit_++]);
    if (!heard.has_value()) {
        return std::unexpected(
            AssociatedMixError{.stage = AssociatedMixError::Stage::kDecode,
                               .associated = choice_.id,
                               .main = main_programme_,
                               .reason = std::string{iclforge::ac3::describe(heard.error())}});
    }
    if (heard->has_value()) {
        if (!layout_.has_value()) {
            layout_ = (*heard)->layout;
        }
        pairing_.push_associated(std::move(**heard));
    }
    return {};
}

void AssociatedMix::end() {
    if (!done_) {
        release_held();
    }
}

void AssociatedMix::release_held() {
    // §3.7: what the decoder still holds once its units have ended is its last.
    // Rendered as coded, so never folded.
    auto flushed = decoder_->flush();
    if (!flushed.empty()) {
        auto held = held_back_unit(std::move(flushed), layout_, false);
        if (held.has_value()) {
            pairing_.push_associated(std::move(*held));
        }
    }
    pairing_.end_associated();
    done_ = true;
}

}  // namespace iclforge::apps
