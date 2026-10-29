#include "iclforge/truehd/core/format.hpp"

#include <bit>
#include <string_view>

#include "iclforge/truehd/core/error.hpp"

namespace iclforge::truehd {

std::string_view channel_name(Channel channel) noexcept {
    switch (channel) {
        case Channel::kL:
            return "L";
        case Channel::kR:
            return "R";
        case Channel::kC:
            return "C";
        case Channel::kLfe:
            return "LFE";
        case Channel::kLs:
            return "Ls";
        case Channel::kRs:
            return "Rs";
        case Channel::kLvh:
            return "Lvh";
        case Channel::kRvh:
            return "Rvh";
        case Channel::kLc:
            return "Lc";
        case Channel::kRc:
            return "Rc";
        case Channel::kLrs:
            return "Lrs";
        case Channel::kRrs:
            return "Rrs";
        case Channel::kCs:
            return "Cs";
        case Channel::kTs:
            return "Ts";
        case Channel::kLsd:
            return "Lsd";
        case Channel::kRsd:
            return "Rsd";
        case Channel::kLw:
            return "Lw";
        case Channel::kRw:
            return "Rw";
        case Channel::kCvh:
            return "Cvh";
        case Channel::kLfe2:
            return "LFE2";
    }
    return "?";
}

namespace {

// TH spec §13.1: bit n of the assignment lists these speakers, left before right. A zero second entry is a
// bit with one speaker.
struct AssignmentBit {
    Channel first;
    bool pair;
    Channel second;
};

constexpr std::array<AssignmentBit, kChannelAssignmentBits> kAssignmentBits{{
    {Channel::kL, true, Channel::kR},
    {Channel::kC, false, Channel::kC},
    {Channel::kLfe, false, Channel::kLfe},
    {Channel::kLs, true, Channel::kRs},
    {Channel::kLvh, true, Channel::kRvh},
    {Channel::kLc, true, Channel::kRc},
    {Channel::kLrs, true, Channel::kRrs},
    {Channel::kCs, false, Channel::kCs},
    {Channel::kTs, false, Channel::kTs},
    {Channel::kLsd, true, Channel::kRsd},
    {Channel::kLw, true, Channel::kRw},
    {Channel::kCvh, false, Channel::kCvh},
    {Channel::kLfe2, false, Channel::kLfe2},
}};

// TH spec App. D, the channel count of each MLP arrangement; -1 where the table is reserved.
constexpr std::array<signed char, 32> kMlpArrangementCounts{
    1, 2, 3, 4, 3, 4, 5, 3, 4, 5, 4, 5, 6, 4, 5, 4, 5, 6, 5, 5, 6,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

}  // namespace

std::vector<Channel> channels_of_assignment(std::uint32_t assignment) {
    std::vector<Channel> channels;
    for (int bit = 0; bit < kChannelAssignmentBits; ++bit) {
        if ((assignment >> bit) & 1U) {
            const AssignmentBit& entry = kAssignmentBits[static_cast<std::size_t>(bit)];
            channels.push_back(entry.first);
            if (entry.pair) {
                channels.push_back(entry.second);
            }
        }
    }
    return channels;
}

int channel_count_of_assignment(std::uint32_t assignment) noexcept {
    int count = 0;
    for (int bit = 0; bit < kChannelAssignmentBits; ++bit) {
        if ((assignment >> bit) & 1U) {
            count += kAssignmentBits[static_cast<std::size_t>(bit)].pair ? 2 : 1;
        }
    }
    return count;
}

int channel_count_of_mlp_arrangement(unsigned arrangement) noexcept {
    return arrangement < kMlpArrangementCounts.size() ? kMlpArrangementCounts[arrangement] : -1;
}

std::string_view to_string(ErrorKind kind) noexcept {
    switch (kind) {
        case ErrorKind::kTruncated:
            return "truncated";
        case ErrorKind::kBadSync:
            return "no sync";
        case ErrorKind::kBadCheckNibble:
            return "access unit check nibble";
        case ErrorKind::kBadLength:
            return "length";
        case ErrorKind::kBadSignature:
            return "major sync signature";
        case ErrorKind::kBadMajorSyncCrc:
            return "major sync CRC";
        case ErrorKind::kBadRestartSync:
            return "restart sync";
        case ErrorKind::kBadRestartCrc:
            return "restart header CRC";
        case ErrorKind::kBadParity:
            return "substream parity";
        case ErrorKind::kBadSubstreamCrc:
            return "substream CRC";
        case ErrorKind::kBadParameter:
            return "parameter out of range";
        case ErrorKind::kBadBlockData:
            return "block data";
        case ErrorKind::kBadLosslessCheck:
            return "lossless check";
        case ErrorKind::kNeedsRestart:
            return "waiting for a restart header";
        case ErrorKind::kUnsupported:
            return "unsupported";
    }
    return "?";
}

}  // namespace iclforge::truehd
