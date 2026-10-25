#pragma once

#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/base/dynamic_range.hpp"

// What a listener's choice of dynamic range (iclforge/base/dynamic_range.hpp) means to the AC-3 and
// E-AC-3 decoders, written into a DecoderConfig. A player that lets its listener choose calls this
// on the configuration it built, last, so that the choice wins over what the build or a server said
// about the same things.
//
// kStandard leaves `config` as it is: the operating mode the build chose and every setting a server
// sent are still in force, and the decode is what it was before the choice existed.
//
// kLateNight is line mode's reference with §7.7.2's heavy compression on top, and nothing of RF mode:
//
//   - Dialogue is normalised to the -31 dBFS reference (§5.4.2.8), as line mode does, so a switch
//     from standard to late night moves no stream's level by its dialnorm. This is the trap a
//     switch to OperatingMode::kRf falls into: RF mode applies every compr word with 11 dB
//     (meta::kRfModeGainDb) that put dialogue at -20 dBFS for a set-top box, 11 dB up from where
//     line mode leaves it (planning/hearth-late-night.md has the measurements).
//   - Every syncframe that carries a compr word is decoded with that word's bare §7.7.2 gain, which
//     an encoder writes to bring the loud passages under a ceiling and the quiet ones up toward
//     dialogue: the stronger of the two compressions a stream can carry.
//   - A syncframe with no compr word falls back on dynrng, in full, as §7.7.2.1 requires - which is
//     what line mode applies to it. So a stream that carries no compr word at all decodes exactly as
//     standard does: late night can only be as strong as the stream's own words, and the library has
//     no scale that makes a coded word stronger than it was written.
//
// The object layer of an E-AC-3 stream needs nothing more: the gain is applied to the bed's
// coefficients before the transform (the program's last dependent substream's word for every
// substream, §E3.8.5), and the objects are reconstructed from the bed it scaled.

namespace iclforge::ac3 {

// The choice itself is codec-neutral and lives in iclforge/base/dynamic_range.hpp; the names are
// re-exported here, as iclforge/base/karaoke.hpp's are, so that the board's settings and routes (a
// project the layering table lets use iclforge::ac3 and not iclforge::base) name the same type.
using base::DynamicRange;
using base::dynamic_range_from_stored;
using base::dynamic_range_name;
using base::kDefaultDynamicRange;
using base::parse_dynamic_range;

inline void apply_dynamic_range(DynamicRange range, DecoderConfig& config) {
    if (range != DynamicRange::kLateNight) {
        return;
    }
    // kCustom, so that the heavy_compression switch is the bare word and not RF mode's; the
    // normalisation is the explicit switch kLine and kRf imply.
    config.output.mode = OperatingMode::kCustom;
    config.output.apply_dialnorm = true;
    config.drc_scale = 1.0;
    config.drc_boost_scale.reset();
    config.heavy_compression = true;
}

}  // namespace iclforge::ac3
