#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// A take: a singer's voice kept as a file (planning/karaoke-microphone.md, M6; D11, D13).
//
// What is written is the **dry voice**: the microphone's own samples mixed to one channel, at the
// rate the device delivers, as lossless FLAC at 24 bits. Not the backing, so the recording contains
// nothing the singer does not own and can be placed against any copy of the backing; a mix is a
// separate action. A sidecar beside it, `<name>.take.json`, says where in the item the first sample
// was sung and what the alignment in force was, so that any editor places it, and a person who
// disagrees can move it.
//
// The encoder is the one Sendspin's FLAC streams use (iclforge::sendspin::codec), so no library is
// added: the file is that encoder's header, which is the `fLaC` marker and STREAMINFO block, and
// its frames in order. The total number of samples is patched into STREAMINFO when the take ends,
// so a player shows a duration. The MD5 is left at zero, which FLAC defines as "not computed".
//
// Nothing here opens a microphone or decides when to record: the caller writes blocks and ends the
// take. A take that is never finished is still a playable FLAC of what was written.

namespace iclforge::hearth {

struct TakeMetadata {
    std::string item_path;
    std::string item_title;
    // Where in the item the first sample was sung, in milliseconds, when it was known.
    std::optional<std::int64_t> item_start_ms;
    // The alignment in force: how long after a sound reaches the microphone it is stamped, and
    // where the person moved it. The item start above already includes both.
    std::int64_t input_delay_ms = 0;
    std::int64_t offset_ms = 0;
    // The platform gave no capture times, so the start is where the block was read.
    bool timing_estimated = false;
    // What the singer was scored against, and whether they said they wore headphones (a take made
    // through speakers has the backing in it).
    std::string reference;
    bool headphones = false;
};

struct TakeInfo {
    std::filesystem::path audio;
    std::filesystem::path sidecar;
    std::uint64_t frames = 0;
    double seconds = 0.0;
    std::uint32_t sample_rate = 0;
};

class TakeRecorder {
   public:
    TakeRecorder();
    ~TakeRecorder();
    TakeRecorder(const TakeRecorder&) = delete;
    TakeRecorder& operator=(const TakeRecorder&) = delete;
    TakeRecorder(TakeRecorder&&) noexcept;
    TakeRecorder& operator=(TakeRecorder&&) noexcept;

    // Opens `file` (the folder is made if it is not there). The error is a sentence for the page.
    [[nodiscard]] std::expected<void, std::string> begin(const std::filesystem::path& file,
                                                         std::uint32_t sample_rate);
    [[nodiscard]] bool recording() const { return encoder_ != nullptr; }

    // Appends mono samples, -1 to 1. False when the file could not be written (the disk is full).
    bool write(std::span<const float> mono);
    [[nodiscard]] std::uint64_t frames() const { return frames_; }

    // Ends the take: the last frames, the duration, and the sidecar.
    [[nodiscard]] std::expected<TakeInfo, std::string> finish(const TakeMetadata& metadata);
    // Ends it and removes what was written.
    void discard();

    // A test seam: the most bytes the file may hold (the header counts). A write that would pass
    // it fails, and so does every one after it, as on a full disk; what was written stays, and
    // finish() still ends the take. 0 is no limit, which is every use outside the tests.
    void limit_bytes(std::uint64_t bytes) { limit_bytes_ = bytes; }

   private:
    struct Encoder;

    // Appends to the file, within the limit; false when it could not.
    bool append(std::span<const std::uint8_t> bytes);

    std::filesystem::path path_;
    std::ofstream out_;
    std::unique_ptr<Encoder> encoder_;
    std::uint32_t rate_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t written_ = 0;
    std::uint64_t limit_bytes_ = 0;
    std::vector<std::int32_t> scratch_;
};

// A file name for a take of `title` at a time: no characters a file system refuses, no trailing
// dot or space, and at most 80 characters of the title.
[[nodiscard]] std::string take_file_stem(const std::string& title, const std::string& timestamp);

}  // namespace iclforge::hearth
