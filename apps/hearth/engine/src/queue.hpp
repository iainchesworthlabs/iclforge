#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/audio/passthrough.hpp"

// The play queue (planning/hearth-reference-player.md, Media): the items the
// player was given, in the order they will play, and which one is current.
//
// Deliberately a plain container with no I/O in it. An item arrives as a path
// and gains what a probe found out about it - format, width, rate, duration -
// and nothing here reads a file, so the whole of the queue's behaviour is
// testable on any machine. The probe itself belongs to the session that
// opens the item.
//
// What "current" means when the queue is empty, when the current item is
// removed under the transport's feet, and when a folder's worth of items is
// added mid-play are all decided here rather than in the transport, because
// they are questions about a list rather than about playback.

namespace iclforge::hearth {

// What an item is coded in, as far as the player is concerned with it: the output decision names a
// codec the item cannot be sent as (TrueHD, whose passthrough is not available), and the window
// names the codec of an item in its queue and Media page.
enum class ItemCodec : std::uint8_t { kUnknown, kAc3, kEac3, kAc4, kTruehd, kMlp };

// What a probe found out about one item. Every field is optional in effect:
// an item that has not been probed yet has zeros, and the transport treats
// that as "not known yet" rather than as a format.
struct ItemFacts {
    // What it is coded in; unknown until the item is opened.
    ItemCodec codec = ItemCodec::kUnknown;
    // What it carries, and nullopt for anything IEC 61937 cannot wrap - a
    // WAV - or that this player does not send as it is: a TrueHD or MLP
    // item's is always unset, because passthrough of TrueHD (MAT) is not
    // available. An AC-4 stream's is the burst type its largest frame needs
    // (Session::open()).
    std::optional<audio::BitstreamFormat> stream = std::nullopt;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    // An AC-3 stream flagged bsmod 7 with an acmod above 1/0: Annex C's karaoke (L, M, R and one
    // or two vocals). Only an AC-3 stream is: E-AC-3 and AC-4 never set the flag, as the library's
    // output stage has it. What a network sink is sent depends on it (sink_form.hpp).
    bool karaoke = false;
    bool has_objects = false;
    // The most dynamic objects the programme places, as an E-AC-3 stream declares in its
    // addbsi (TS 103 420's complexity index, which is an object count) and an AC-4
    // presentation's first I-frame decodes to (the LFE, which no sink places, is not one); 0 for
    // a stream that declares none and where it is not known. What a sink's stated limit is
    // compared with (sink_form.hpp).
    std::uint16_t objects = 0;
    // An AC-4 presentation of object audio alone: it decodes to no channels, so a decoder that
    // places no objects has no bed to play and plays nothing. False for every E-AC-3 stream,
    // whose bed is its core, and for an AC-4 presentation that carries channels.
    bool objects_alone = false;
    // The frames (access units, one a burst chunk) a second the programme is coded at, as its
    // samples and its frame count give it: 31.25 for AC-3 and for E-AC-3 at six blocks a unit,
    // and for AC-4 the stream's own rate (23.44, 24, 25, 29.97 and on). What a sink's stated
    // limit (`max_frame_rate`) is compared with; 0 where it is not known.
    double frame_rate = 0.0;
    // The most coded data a second the programme is declared to need, in kbit/s: for TrueHD and
    // MLP the peak data rate the stream's major sync states (TH spec section 4.4), which a sink's
    // stated limit (`max_data_rate`) and chunk size are compared with. 0 where it is not known,
    // and for every other codec.
    std::uint32_t peak_kbps = 0;
    // For TrueHD and MLP, the channels each presentation decodes to, lowest first and cumulative
    // (2, 6, 8): substream 0 alone, substreams 0 and 1, and so on, as the stream's restart headers
    // say. A sink that decodes fewer channels than the stream's best can be sent the first
    // substreams alone (TruehdIndex::channels). Empty for every other codec.
    std::vector<std::uint16_t> presentations{};
    // Measured over the whole programme, the same definition
    // io::ProbeReport::bitrate_kbps uses: its bytes * 8, over its duration.
    // Unset until the item has been probed, the same as the fields above.
    std::optional<double> bitrate_kbps = std::nullopt;
    // From the access-unit count and the samples each one carries - NOT a
    // fixed 1536, which is wrong for an E-AC-3 frame with fewer than six
    // blocks (the plan's own note on apps/forge/gui/src/stream_player_controller.cpp).
    std::optional<std::chrono::milliseconds> duration = std::nullopt;
    // Set when the item was recognised but cannot be played here: an AC-4
    // stream with no presentation this build decodes, say. It stays in the
    // queue and is skipped, with the reason shown.
    std::string unplayable_because{};
    // Something to show beside an item that does play: that its edit list
    // could not be applied, say.
    std::string note{};
};

struct QueueItem {
    std::string path{};
    // What to show. The file's own name until metadata has been read.
    std::string title{};
    ItemFacts facts{};

    [[nodiscard]] bool playable() const { return facts.unplayable_because.empty(); }
};

class Queue {
public:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    [[nodiscard]] std::size_t size() const { return items_.size(); }
    [[nodiscard]] bool empty() const { return items_.empty(); }
    [[nodiscard]] std::span<const QueueItem> items() const { return items_; }

    // The item playing, or kNone when the queue is empty or has been stopped
    // past its end.
    [[nodiscard]] std::size_t current_index() const { return current_; }
    [[nodiscard]] const QueueItem* current() const {
        return current_ < items_.size() ? &items_[current_] : nullptr;
    }
    [[nodiscard]] QueueItem* current_mutable() {
        return current_ < items_.size() ? &items_[current_] : nullptr;
    }

    // Appends, and makes the first item added to an empty queue the current
    // one so that a play command has somewhere to start.
    void add(QueueItem item);
    void add(std::span<const QueueItem> items);

    // Inserts before `index` (clamped to the end). The current item stays the
    // same item, whatever its index becomes.
    void insert(std::size_t index, QueueItem item);

    // Removes `index`. Removing the current item leaves the NEXT item
    // current - which is what a person deleting the playing track expects -
    // and removing the last item leaves kNone. Returns whether the current
    // item changed, since the transport has to restart playback if it did.
    bool remove(std::size_t index);

    void clear();

    // Moves one item, for a drag in the queue list. The current item follows
    // its own move. False, changing nothing, for an index out of range.
    bool move(std::size_t from, std::size_t to);

    // Makes `index` current without saying anything about playback: the
    // transport decides what that means. False for an index out of range.
    bool set_current(std::size_t index);

    // The next and previous PLAYABLE items, skipping any the probe marked
    // unplayable, or kNone at either end. `repeat` makes the ends meet.
    [[nodiscard]] std::size_t next_index(bool repeat = false) const;
    [[nodiscard]] std::size_t previous_index(bool repeat = false) const;

    // Facts for an item, as a probe learns them.
    bool set_facts(std::size_t index, ItemFacts facts);

private:
    std::vector<QueueItem> items_;
    std::size_t current_ = kNone;
};

// An item as the queue list shows it, for the diagnostics file: `item 3
// "Title"`, numbered from 1, or `"Title" (no longer in the queue)` for
// Queue::kNone, which with no title is `an item no longer in the queue`.
[[nodiscard]] std::string describe_item(std::size_t index, std::string_view title);

}  // namespace iclforge::hearth
