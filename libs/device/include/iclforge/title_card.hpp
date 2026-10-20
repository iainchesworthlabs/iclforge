#pragma once

// What the panel knows of the song a server says is playing (planning/p4x-now-playing.md, stage 2): the
// title, the artist, the album and its year, and where in the song it is. A server tells a client that
// lists metadata@v1 in server/state, and the two servers a board meets say it differently
// (planning/hearth-sendspin-extension.md, C36):
//
//   the specification   every object is the whole state: a field left out is a cleared one, an omitted
//                       progress clears the position;
//   aiosendspin 9.1.1   (Music Assistant) the first object is whole and later ones carry only what
//                       changed: a field left out is unchanged, and a cleared one is null.
//
// The store applies either, so that a track change to one with no artist does not keep the last
// track's, and a pause (a progress and nothing else) does not lose the title. Standard library only, so
// that the host tests it (libs/device/tests/test_title_card.cpp); the Sendspin types are turned into a
// TitleUpdate by the sink (firmware/hearth-sink/main/sendspin/player/sendspin.cpp).
//
// The strings keep their capacity (they are reserved to their limits when the store is made), so that
// applying an update, and the panel's five reads a second, allocate nothing from the heap a decoder
// leaves little of.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace iclforge::nowview {

// How one field of an update reads: left out of the object, null, or a value.
enum class Say : std::uint8_t { kLeave, kClear, kSet };

struct TitleUpdate {
    struct Text {
        Say say = Say::kLeave;
        std::string_view value = {};
    };
    struct Number {
        Say say = Say::kLeave;
        std::int32_t value = 0;
    };
    struct Progress {
        Say say = Say::kLeave;
        std::int64_t position_ms = 0;
        // 0 for an unknown or unlimited duration.
        std::int64_t duration_ms = 0;
        // The speed times 1,000: 1,000 normal, 0 paused.
        std::int32_t speed = 1000;
    };

    // `metadata: null`: nothing is playing, whatever the fields say.
    bool clear_all = false;
    // The object is the whole state (the specification): what it leaves out is cleared.
    bool whole = false;
    // Server clock time the update takes effect at, microseconds: the time the progress is the position at.
    std::int64_t timestamp_us = 0;
    Text title;
    Text artist;
    Text album;
    Number year;
    Progress progress;
};

class TitleStore {
   public:
    // What is kept of a longer text: a card draws a title in two rows and the others in one, and a server's
    // text is as long as it likes.
    static constexpr std::size_t kTitleBytes = 256;
    static constexpr std::size_t kArtistBytes = 160;
    static constexpr std::size_t kAlbumBytes = 160;

    struct Progress {
        // The position at `at_us` on the server's clock.
        std::int64_t at_us = 0;
        std::int64_t position_ms = 0;
        std::int64_t duration_ms = 0;
        std::int32_t speed = 1000;
    };

    TitleStore() {
        title_.reserve(kTitleBytes);
        artist_.reserve(kArtistBytes);
        album_.reserve(kAlbumBytes);
    }

    void apply(const TitleUpdate& update) {
        if (update.clear_all) {
            clear();
            return;
        }
        const auto resolve = [&update](Say say) { return say == Say::kLeave && update.whole ? Say::kClear : say; };
        bool changed = false;
        changed |= put(title_, update.title.value, resolve(update.title.say), kTitleBytes);
        changed |= put(artist_, update.artist.value, resolve(update.artist.say), kArtistBytes);
        changed |= put(album_, update.album.value, resolve(update.album.say), kAlbumBytes);
        const Say year = resolve(update.year.say);
        if (year == Say::kSet && (!year_ || *year_ != update.year.value)) {
            year_ = update.year.value;
            changed = true;
        } else if (year == Say::kClear && year_) {
            year_.reset();
            changed = true;
        }
        // The progress is anchored where it was told, not at the last object's time: a title that arrives
        // after the progress does not move the song.
        switch (resolve(update.progress.say)) {
            case Say::kSet:
                progress_ = Progress{.at_us = update.timestamp_us,
                                     .position_ms = std::max<std::int64_t>(update.progress.position_ms, 0),
                                     .duration_ms = std::max<std::int64_t>(update.progress.duration_ms, 0),
                                     .speed = std::max<std::int32_t>(update.progress.speed, 0)};
                break;
            case Say::kClear:
                progress_.reset();
                break;
            case Say::kLeave:
                break;
        }
        if (changed) {
            ++revision_;
        }
    }

    void clear() {
        const bool had = !title_.empty() || !artist_.empty() || !album_.empty() || year_;
        title_.clear();
        artist_.clear();
        album_.clear();
        year_.reset();
        progress_.reset();
        if (had) {
            ++revision_;
        }
    }

    // A title card needs a title: an artist with none is not a card.
    [[nodiscard]] bool known() const { return !title_.empty(); }
    [[nodiscard]] const std::string& title() const { return title_; }
    [[nodiscard]] const std::string& artist() const { return artist_; }
    [[nodiscard]] const std::string& album() const { return album_; }
    [[nodiscard]] std::optional<std::int32_t> year() const { return year_; }
    [[nodiscard]] const std::optional<Progress>& progress() const { return progress_; }
    // Bumped whenever a text or the year changed, so that a reader copies the strings only then.
    [[nodiscard]] std::uint32_t revision() const { return revision_; }

    // Where the song is at `server_now_us` on the server's clock, by the role's calculation
    // (metadata::position_ms): the position advanced at the speed, held between nought and the duration
    // when there is one. Nothing without a progress.
    [[nodiscard]] std::optional<std::int64_t> position_ms(std::int64_t server_now_us) const {
        if (!progress_) {
            return std::nullopt;
        }
        const double moved = static_cast<double>(server_now_us - progress_->at_us) *
                             static_cast<double>(progress_->speed) / 1'000'000.0;
        const double calculated = static_cast<double>(progress_->position_ms) + moved;
        const double upper = progress_->duration_ms > 0 ? static_cast<double>(progress_->duration_ms) : 9.0e18;
        return static_cast<std::int64_t>(std::clamp(calculated, 0.0, upper));
    }

    // The song's length, when the server says one.
    [[nodiscard]] std::optional<std::int64_t> length_ms() const {
        if (progress_ && progress_->duration_ms > 0) {
            return progress_->duration_ms;
        }
        return std::nullopt;
    }

   private:
    // `from` cut to `limit` bytes without cutting a UTF-8 sequence in two (the screen's fonts draw a
    // half of one as a box).
    [[nodiscard]] static std::string_view cut(std::string_view from, std::size_t limit) {
        if (from.size() <= limit) {
            return from;
        }
        std::size_t end = limit;
        while (end > 0 && (static_cast<unsigned char>(from[end]) & 0xC0U) == 0x80U) {
            --end;
        }
        return from.substr(0, end);
    }

    [[nodiscard]] static bool put(std::string& to, std::string_view value, Say say, std::size_t limit) {
        switch (say) {
            case Say::kSet: {
                const std::string_view kept = cut(value, limit);
                if (to == kept) {
                    return false;
                }
                to.assign(kept.data(), kept.size());
                return true;
            }
            case Say::kClear:
                if (to.empty()) {
                    return false;
                }
                to.clear();
                return true;
            case Say::kLeave:
                break;
        }
        return false;
    }

    std::string title_;
    std::string artist_;
    std::string album_;
    std::optional<std::int32_t> year_;
    std::optional<Progress> progress_;
    std::uint32_t revision_ = 0;
};

}  // namespace iclforge::nowview
