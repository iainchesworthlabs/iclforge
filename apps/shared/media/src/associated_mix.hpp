#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/decoder/associated_service.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"

// §E3.10's programme mixing for a player: which programme of a stream is the
// associated service a listener asked for, and how its decoded units are paired
// with the main's and mixed. iclforge::ac3::AssociatedServiceMixer is the
// mixer; what it does not own is everything around it, which 'forge decode'
// wrote first and 'forge monitor' needs in the same form:
//
//   * choose_associated() resolves associated= - a substream number or a
//     service name - against what the stream carries;
//   * AssociatedMix runs the associated programme's own decoder in step with
//     the main's, holds each side's units until the other has one, and mixes
//     them.
//
// Device-free and free of any command's options, so a test can hold it without
// a render device and each command prints in its own wording. Compiled straight
// into forge and iclforge-app-media-tests, as stream_playback.cpp beside it is
// (see recording_sink.hpp for why apps/shared/media/src has no library target).

namespace iclforge::apps {

// The programme associated= names, and what the stream says it is.
struct AssociatedChoice {
    int id = 0;
    int bsmod = 0;
    iclforge::ac3::Acmod acmod = iclforge::ac3::Acmod::k2_0;
    bool lfe = false;
};

// What the listener asked for. A number names the independent substream; a
// service name arrives as the bsmod that labels it: A/52 Table 5.7 numbers the
// services as ETSI TS 103 190-1 Table 91 numbers its content classifiers, so
// the code a name resolves to for AC-4 is the bsmod to look for here. When both
// are set the substream wins.
struct AssociatedRequest {
    std::optional<int> programme = std::nullopt;
    std::optional<int> bsmod = std::nullopt;
};

// One programme of the stream, as a refusal lists what there was to choose from.
struct CarriedProgramme {
    int id = 0;
    int bsmod = 0;
    iclforge::ac3::Acmod acmod = iclforge::ac3::Acmod::k2_0;
};

// Why choose_associated chose nothing.
struct AssociatedRefusal {
    enum class Reason : std::uint8_t {
        kUnreadable,    // the stream does not scan: `detail` says why
        kDualMonoMain,  // 1+1 is two programmes with no soundfield to mix a third into
        kIsTheMain,     // associated= names the programme being played
        kNotCarried,    // a substream number the stream has no programme for
        kNoSuchService  // no programme other than the main carries that bsmod
    };
    Reason reason = Reason::kUnreadable;
    int main = 0;
    // The substream number asked for (kIsTheMain, kNotCarried).
    int requested = 0;
    // Every programme the stream has, for kNoSuchService to list.
    std::vector<CarriedProgramme> carried = {};
    std::string detail = {};
};

// §E3.10: the programme `request` names beside `main`, or the reason there is
// none. The main is never its own associated service, and a 1+1 main is two
// programmes with no soundfield to mix a third into. Headers only: nothing is
// decoded.
[[nodiscard]] std::expected<AssociatedChoice, AssociatedRefusal> choose_associated(
    std::span<const std::byte> stream, int main, const AssociatedRequest& request);

// What the mixing applied over a stream, for a status report: the range each
// gain took and where a mono service sat.
struct MixReport {
    std::size_t units = 0;
    double main_min = 0.0;
    double main_max = 0.0;
    double associated_min = 0.0;
    double associated_max = 0.0;
    std::optional<int> panmean = std::nullopt;

    void observe(const iclforge::ac3::AssociatedServiceMixResult& result);
};

// What stopped the mix.
struct AssociatedMixError {
    enum class Stage : std::uint8_t {
        kDecode,  // the associated programme's decoder refused a unit
        kMix      // AssociatedServiceMixer refused the pair
    };
    Stage stage = Stage::kDecode;
    // The associated programme's substream number, and the main's.
    int associated = 0;
    int main = 0;
    // iclforge::ac3::describe() of the decoder's or the mixer's own error.
    std::string reason = {};
};

// The pairing: a main programme's units wait in one queue and an associated
// service's in another, and `next` mixes the heads once both sides have one.
//
// Two decoders running in step still do not release units in step. A decoder
// holding frames back for transient pre-noise (§3.7) releases its first a
// frame or more late, and the two need not hold the same number back, so each
// side's units are queued until its partner has one. Where the two differ in
// length - the flushed tail of a stream that holds several short frames back -
// the longer is cut to the shorter. The mixer sees equal-length units only.
// When the service has ended, what is left of the main goes out as it is.
class UnitPairing {
   public:
    // `main` and `associated` are the programmes' substream numbers, for the
    // error a refused mix names them in.
    UnitPairing(const iclforge::ac3::AssociatedServiceMixConfig& config, int main, int associated)
        : mixer_(config), main_programme_(main), associated_programme_(associated) {}

    void push_main(iclforge::ac3::DecodedAccessUnit unit);
    void push_associated(iclforge::ac3::DecodedAccessUnit unit);
    // The associated side has released every unit it ever will: a main unit
    // that finds no partner is now played alone instead of waiting for one.
    void end_associated() { associated_done_ = true; }

    // The next main unit with the service mixed in - or, once the service has
    // ended, without - or std::nullopt when the head of the main's queue is
    // waiting for the service's next unit (or there is none). The units are
    // dropped from their queues as they are used.
    [[nodiscard]] std::expected<std::optional<iclforge::ac3::DecodedAccessUnit>, AssociatedMixError>
    next();

    [[nodiscard]] const MixReport& report() const { return report_; }

   private:
    iclforge::ac3::AssociatedServiceMixer mixer_;
    int main_programme_ = 0;
    int associated_programme_ = 0;
    std::deque<iclforge::ac3::DecodedAccessUnit> main_;
    std::deque<iclforge::ac3::DecodedAccessUnit> associated_;
    bool associated_done_ = false;
    MixReport report_;
};

// The associated service as a second decoder, run in step with the caller's
// decoder of the main programme and feeding a UnitPairing.
//
// The caller decodes the main itself - it has its own configuration for it
// (the census trace, the object layer, the fold) and its own end-of-stream
// handling - and calls this one unit for unit:
//
//   for each main access unit:
//       decoded = main_decoder.decode_access_unit(unit)
//       mix.advance()                      // always, released or held back
//       if released: mix.push_main(...), then next() until it has none
//   at the end:
//       mix.end(); mix.push_main(held-back unit, if any); next() until none
//
// The service renders as coded whatever fold the main was asked for: a mono
// description folded to Lo/Ro would arrive as two channels with the centre
// already spread over both, and the mixer could no longer place it by its pan.
// Its objects are never wanted.
class AssociatedMix {
   public:
    // `main` is the DecoderConfig the main's own decoder runs with: what the
    // listener asked of it - drc_scale, heavy compression, the output level,
    // concealment - applies to the service too, and its fold is the one the
    // mixer is told the main arrives in. `units` are the service's access
    // units in order (iclforge::apps::select_programme or
    // iclforge::ac3::split_access_units for its substream), and point into a
    // stream that outlives this. `trim_db` is the listener's own level for the
    // service, on top of the stream's gains.
    AssociatedMix(const iclforge::ac3::DecoderConfig& main_config, int main_programme,
                  const AssociatedChoice& choice,
                  std::vector<std::span<const std::byte>> units, double trim_db);

    // The service's decoder's configuration for a main configured with `main`:
    // what the listener asked of the main, except what means nothing for a
    // service - the fold, the traces, the object layer. Exposed for a test.
    [[nodiscard]] static iclforge::ac3::DecoderConfig service_config(
        const iclforge::ac3::DecoderConfig& main, int programme);

    // One main access unit was given to the main decoder: decode the service's
    // next, or - the service having run out of units before the main - flush
    // what its decoder held back and end it. Whether or not the main decoder
    // released a unit for it.
    [[nodiscard]] std::expected<void, AssociatedMixError> advance();

    // The main decoder released a unit (or, at the end, flush()'s).
    void push_main(iclforge::ac3::DecodedAccessUnit unit) { pairing_.push_main(std::move(unit)); }

    // The end of the main's units: the service's own held-back unit, if it had
    // not already ended. Call before the last push_main and the final next()s.
    void end();

    [[nodiscard]] std::expected<std::optional<iclforge::ac3::DecodedAccessUnit>, AssociatedMixError>
    next() {
        return pairing_.next();
    }

    [[nodiscard]] const AssociatedChoice& choice() const { return choice_; }
    [[nodiscard]] int main_programme() const { return main_programme_; }
    [[nodiscard]] const MixReport& report() const { return pairing_.report(); }

   private:
    void release_held();

    // On the heap: Eac3Decoder's per-block scratch is several KB, which a
    // caller's own stack frame would otherwise carry (PREfast's C6262).
    std::unique_ptr<iclforge::ac3::Eac3Decoder> decoder_;
    std::vector<std::span<const std::byte>> units_;
    std::size_t next_unit_ = 0;
    AssociatedChoice choice_;
    int main_programme_ = 0;
    UnitPairing pairing_;
    // The service's own layout, from its first unit: its held-back unit is
    // laid out against it (held_back_unit's own doc comment).
    std::optional<iclforge::ac3::eac3::chanmap::Layout> layout_;
    bool done_ = false;
};

}  // namespace iclforge::apps
