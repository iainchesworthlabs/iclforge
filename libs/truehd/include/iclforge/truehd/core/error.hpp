#pragma once

#include <cstdint>
#include <string_view>

namespace iclforge::truehd {

// Why a read or a decode stopped. Every one is an expected condition of untrusted input and is
// returned, not thrown (CONTRIBUTING.md).
enum class ErrorKind : std::uint8_t {
    kTruncated,        // the data ends inside a field or a unit
    kBadSync,          // no major sync, or no access unit, where one was required
    kBadCheckNibble,   // TH spec §3.2: the nibbles of the header and directory do not XOR to 0xF
    kBadLength,        // a length, an end pointer or a size outside the data or outside its limit
    kBadSignature,     // TH spec §4.3: the major sync's signature is not 0xB752
    kBadMajorSyncCrc,  // TH spec §4.1: the CRC-16 of the major sync
    kBadRestartSync,   // TH spec §7.2
    kBadRestartCrc,    // TH spec §7.1: the CRC-8 of the restart header
    kBadParity,        // TH spec §6.2: the parity byte at the end of a substream segment
    kBadSubstreamCrc,  // TH spec §6.2: the CRC-8 at the end of a substream segment
    kBadParameter,     // a field outside the range the specification allows
    kBadBlockData,  // a block whose sample data does not end where it says (TH spec §8.1) or a bad
                    // code
    kBadLosslessCheck,  // TH spec §7.3: the decoded samples do not match the restart header's check
    kNeedsRestart,      // a substream needs a restart header (or a major sync) it has not seen
    kUnsupported,       // valid, and a form this version does not decode (detail names it)
};

struct Error {
    ErrorKind kind = ErrorKind::kTruncated;
    // A sentence for a log: a string literal, so it needs no storage and no lifetime.
    std::string_view detail;
};

[[nodiscard]] std::string_view to_string(ErrorKind kind) noexcept;

}  // namespace iclforge::truehd
