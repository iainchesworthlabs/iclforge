#include "iclforge/ac3/io/dec3.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/types.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/meta/bsi.hpp"

namespace iclforge::ac3::io {

namespace {

// Annex F's fscod has no fscod2 counterpart - E-AC-3's reduced-rate
// extension (§E2.3.1.3) postdates ETSI TS 102 366 Annex F, so a reduced-rate
// stream has no exact 2-bit code to report here. Annex F's fscod is
// fundamentally a SAMPLE RATE FAMILY selector (Table 5.6's three families:
// 48/44.1/32 kHz, each with its reduced-rate half), and fscod_family() is
// exactly that mapping (tables.hpp), so this reports the family and leaves
// the exact rate to the sample entry's own samplerate field
// (iclforge::containers::mp4::AudioTrack::sample_rate, set from the same ScannedStream) - the box
// exists for capability signalling (E-AC-3? how many channels? Atmos?), not
// as the sample rate's source of truth.
[[nodiscard]] std::uint32_t box_fscod(SampleRate sr) {
    return static_cast<std::uint32_t>(fscod_family(sr));
}

// Table F.6.1's chan_loc: nine locations "beyond the standard 5.1 channels",
// bit 0 in the least significant position and LFE2 in the most. The names are
// Table E2.5's own, so this renames bit positions rather than translating -
// A/52's chanmap word counts from the other end (Left is its most significant
// bit) and has one location, the Lts/Rts pair, that Annex F's table has no bit
// for. A location the table cannot name is left out, as F.6.2.13 leaves out a
// replacement channel: the field says which extra locations the dependents
// add, and a player opens the track with the channel count the sample entry
// carries either way.
[[nodiscard]] std::uint32_t chan_loc_from(std::uint16_t channel_map) {
    using namespace eac3::chanmap;
    constexpr std::array<std::uint16_t, 9> kLocations = {
        kLcRcBit, kLrsRrsBit, kCsBit, kTsBit, kLsdRsdBit, kLwRwBit, kVhlVhrBit, kVhcBit, kLfe2Bit};
    std::uint32_t loc = 0;
    for (std::size_t bit = 0; bit < kLocations.size(); ++bit) {
        if ((channel_map & kLocations[bit]) != 0) {
            loc |= 1U << bit;
        }
    }
    return loc;
}

}  // namespace

std::vector<std::byte> build_codec_config_box(const ScannedStream& stream,
                                              BoxProgrammes programmes) {
    BitWriter w;

    // §E2.3.1.2's legacy core - an AC-3 syncframe with Annex E dependents
    // behind it - takes the EC3SpecificBox below. An AC3SpecificBox describes
    // one AC-3 syncframe and has no field for the dependents, but the E-AC-3
    // box does not need the independent substream to be Annex E syntax:
    // §E2.3.1.2 makes the AC-3 frame "an independent substream assigned
    // substream ID 0" of the E-AC-3 stream, and ETSI TS 102 366 F.6.2.5 sets
    // bsid to "the same value as the bsid field in the independent substream",
    // which for the core is its own 6 or 8. The stream's scalar fields already
    // describe the core (ScannedStream's own comment), so it falls through.
    if (stream.kind == StreamKind::kAc3) {
        // ETSI TS 102 366 Annex F §F.4 AC3SpecificBox: fscod(2) + bsid(5) +
        // bsmod(3) + acmod(3) + lfeon(1) + bit_rate_code(5) + reserved(5) =
        // 24 bits, byte-aligned by construction.
        w.put(box_fscod(stream.sample_rate), 2);                    // fscod
        w.put(static_cast<std::uint32_t>(stream.bsid), 5);          // bsid
        w.put(static_cast<std::uint32_t>(stream.bsmod), 3);         // bsmod
        w.put(static_cast<std::uint32_t>(stream.acmod), 3);         // acmod
        w.put(stream.lfe ? 1U : 0U, 1);                             // lfeon
        w.put(static_cast<std::uint32_t>(stream.bit_rate_code), 5); // bit_rate_code
        w.put(0, 5);                                                // reserved
        return w.take();
    }

    // ETSI TS 102 366 Annex F §F.6 EC3SpecificBox. Field layout cross-checked
    // against Dolby's own Digital Plus Online Delivery Kit documentation -
    // the EC3SpecificBox derivation guide for data_rate's semantics, and
    // https://ott.dolby.com/OnDelKits/DDP/Dolby_Digital_Plus_Online_Delivery_Kit_v1.4.1/Documentation/Playback/SDM/help_files/topics/c_id_ddp_atmos_isobmff.html
    // for the exact bit layout of the trailing Atmos extension (see the
    // addbsi block below) - both fetched and read directly, not recalled.
    //
    // data_rate(13) + num_ind_sub(3) = 16 bits, byte-aligned. data_rate's own
    // semantics ("the data rate of the ... bitstream, or the maximum data
    // rate if VBR" - Dolby's own EC3SpecificBox derivation guide) are exactly
    // what iclforge::ac3::eac3::frame_words() fixes per bitrate for this project's CBR
    // encoder, so the first access unit's own size is the exact rate, not an
    // estimate: kbps = bytes * 8 * sample_rate / samples_per_frame / 1000.
    //
    // F.6.2.2 calls it the rate "of the entire bitstream ... the sum of the
    // data rates of all the substreams", so for a track holding every
    // programme (kAll) each one's first unit counts and not the lead's alone.
    // A ScannedStream without a programme list (one built by hand) is the lead
    // programme and nothing else.
    const bool all = programmes == BoxProgrammes::kAll;
    std::size_t first_unit_bytes = 0;
    if (all && stream.programmes.size() > 1) {
        for (const auto& programme : stream.programmes) {
            if (!programme.access_units.empty()) {
                first_unit_bytes += programme.access_units.front().size();
            }
        }
    } else if (!stream.access_units.empty()) {
        first_unit_bytes = stream.access_units.front().size();
    }
    const std::uint64_t data_rate_bps = static_cast<std::uint64_t>(first_unit_bytes) * 8 *
                                        sample_rate_hz(stream.sample_rate);
    constexpr std::uint64_t kDenominator = static_cast<std::uint64_t>(kSamplesPerFrame) * 1000;
    const std::uint64_t data_rate_kbps =
        first_unit_bytes == 0 ? 0 : (data_rate_bps + kDenominator / 2) / kDenominator;
    // §F.6's data_rate is 13 bits (max 8191); one programme at E-AC-3's own
    // ceiling (§E1.3.1.5, 6144 kbps) fits, eight of them would not, so this
    // clamp is a backstop rather than a case anyone meets.
    constexpr std::uint32_t kMaxDataRate = (1U << 13) - 1;
    w.put(static_cast<std::uint32_t>(std::min<std::uint64_t>(data_rate_kbps, kMaxDataRate)), 13);

    // One block per independent substream (§F.6.1's loop over num_ind_sub + 1).
    // The lead programme's block comes from the stream's own scalar summary, as
    // it always did, and each further programme's from its ScannedProgramme.
    // Programmes are listed in ascending substreamid order and §E2.3.1.2
    // numbers them sequentially, which makes the count of blocks the same
    // number F.6.2.3 calls "the substreamID value of the last independent
    // substream". A box declaring programmes the track does not hold would be
    // worse than one describing what it does, so kLead - the pairing with
    // ScannedStream::access_units - declares one, and kAll is for a track
    // built from all_programme_access_units().
    const std::size_t independent =
        all ? std::clamp<std::size_t>(stream.programmes.size(), std::size_t{1}, std::size_t{8})
            : std::size_t{1};
    w.put(static_cast<std::uint32_t>(independent - 1), 3);  // num_ind_sub

    for (std::size_t i = 0; i < independent; ++i) {
        const ScannedProgramme absent{};
        const ScannedProgramme& programme =
            i < stream.programmes.size() ? stream.programmes[i] : absent;
        const bool lead = i == 0;
        const auto bsid = static_cast<std::uint32_t>(lead ? stream.bsid : programme.bsid);
        const auto bsmod = static_cast<std::uint32_t>(lead ? stream.bsmod : programme.bsmod);
        const auto acmod = static_cast<std::uint32_t>(lead ? stream.acmod : programme.acmod);
        const bool lfe = lead ? stream.lfe : programme.lfe;
        const std::size_t units = lead ? stream.substreams_per_unit : programme.substreams_per_unit;
        const std::uint16_t map = lead ? stream.channel_map : programme.channel_map;

        w.put(box_fscod(stream.sample_rate), 2);  // fscod
        // F.6.2.5: "the same value as the bsid field in the independent
        // substream" - whatever that substream is, which for a legacy core
        // is the AC-3 frame's own 6 or 8. Nothing in F.6 limits the field to
        // 16, and F.1 asks for an EC3SampleEntry for every E-AC-3 bit stream.
        w.put(bsid, 5);  // bsid
        w.put(0, 1);     // reserved
        // asvc: the associated-service flag. A/52 §5.4.2.2 puts the service type
        // in bsmod - CM/ME are main services, VI/HI/D/C/E are associated, and
        // code 7 is voice-over (associated) at acmod 1/0 but karaoke (a MAIN
        // service) everywhere else, Table 5.7's one acmod-dependent split. So
        // this is exactly "is this programme's own bsmod an associated one, per
        // Table 5.7", read off the bitstream rather than assumed - which for the
        // ordinary main-service stream still comes out 0, as it always did.
        w.put(meta::is_associated_service(static_cast<meta::BitstreamMode>(bsmod),
                                          static_cast<Acmod>(acmod))
                  ? 1U
                  : 0U,
              1);               // asvc
        w.put(bsmod, 3);        // bsmod (F.6.2.8: 0 when the substream sends none)
        w.put(acmod, 3);        // acmod
        w.put(lfe ? 1U : 0U, 1);  // lfeon
        w.put(0, 3);            // reserved
        // substreams_per_unit counts every substream of the first access unit,
        // independent one included (ScannedStream's own comment) - so the
        // dependent count is one less, floored at 0 for a stream scan() rejected
        // before ever reaching here (it never returns with substreams_per_unit
        // == 0 on success, but this keeps the subtraction defined regardless).
        const std::size_t num_dep_sub =
            units > 0 ? std::min<std::size_t>(units - 1, std::size_t{15}) : std::size_t{0};
        w.put(static_cast<std::uint32_t>(num_dep_sub), 4);  // num_dep_sub
        if (num_dep_sub > 0) {
            // chan_loc (F.6.2.13): the locations the dependents add beyond
            // 5.1. Every location Table F.6.1 has a bit for is one a 5.1 bed
            // cannot hold, so the programme's whole location word gives the
            // answer without having to subtract the bed. Left at zero this
            // was no harmless omission: F.5.2 has a player IGNORE the sample
            // entry's ChannelCount, which makes this field - with acmod and
            // lfeon - the only description of the layout the box carries.
            w.put(chan_loc_from(map), 9);  // chan_loc
        } else {
            w.put(0, 1);  // reserved
        }
    }

    if (stream.oba_complexity_index.has_value()) {
        // TS 103 420 §8.3.1/§8.3.2.2, echoed into the box exactly as
        // iclforge::ac3::io::scan() read it out of the bitstream's own addbsi (see
        // ScannedStream::oba_complexity_index) - this is the exact signal
        // FFmpeg's E-AC-3+JOC remux path is documented to drop or mis-signal
        // (https://github.com/jellyfin/jellyfin-ffmpeg/issues/584), which is
        // the reason this box is built from the bitstream rather than copied
        // from another tool's dec3.
        w.put(0, 7);                                                        // reserved
        w.put(1, 1);                                                        // flag_ec3_extension_type_a
        w.put(static_cast<std::uint32_t>(*stream.oba_complexity_index), 8); // complexity_index_type_a
    } else {
        w.put(0, 7);  // reserved
        w.put(0, 1);  // flag_ec3_extension_type_a
    }
    return w.take();
}

std::string dash_channel_configuration(const ScannedStream& stream) {
    // Four upper-case hex digits of ScannedStream::channel_map, zero-padded -
    // see this function's own comment in ac3/io/dec3.hpp for the scheme and
    // the citation.
    return fmt::format("{:04X}", stream.channel_map);
}

}  // namespace iclforge::ac3::io
