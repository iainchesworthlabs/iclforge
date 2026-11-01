#include "iclforge/sink/passthrough.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/containers/iec61937/iec61937.hpp"

namespace iclforge::sink {

namespace {

namespace iec = containers::iec61937;
namespace ac = sendspin::player;

// IEC 61937's data-type word (Pc) of the bursts a stream of `type` is made of, where it is one value:
// AC-3 is 1, E-AC-3 21. AC-4's has a subtype that the first burst names and that depends on the
// frame rate, so a link for it is opened from a burst and not from the stream's start, which this
// output does not do yet.
[[nodiscard]] std::optional<std::uint16_t> pc_of(ac::DataType type) {
    switch (type) {
        case ac::DataType::kAc3:
            return std::uint16_t{0x0001};
        case ac::DataType::kEac3:
            return std::uint16_t{0x0015};
        case ac::DataType::kAc4:
        case ac::DataType::kTruehd:  // access units, not IEC 61937 bursts: this output does not take them
            break;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<LinkFormat> format_of(const ac::StreamStart& stream) {
    const std::optional<std::uint16_t> pc = pc_of(stream.data_type);
    if (!pc) {
        return std::nullopt;
    }
    const std::optional<std::size_t> bytes = iec::burst_period_bytes(*pc);
    if (!bytes) {
        return std::nullopt;
    }
    const std::uint32_t ratio = stream.data_type == ac::DataType::kEac3 ? 4U : 1U;
    // A link frame is two channels of 16 bits.
    const auto link_frames = static_cast<std::uint32_t>(*bytes / 4U);
    return LinkFormat{.data_type = stream.data_type,
                      .sample_rate = static_cast<std::uint32_t>(stream.sample_rate),
                      .carrier_ratio = ratio,
                      .burst_bytes = *bytes,
                      .frames_per_burst = link_frames / ratio,
                      .pc = *pc};
}

[[nodiscard]] bool same_format(const LinkFormat& a, const LinkFormat& b) {
    return a.data_type == b.data_type && a.sample_rate == b.sample_rate && a.burst_bytes == b.burst_bytes;
}

}  // namespace

PassthroughOutput::PassthroughOutput(BurstLink& link, std::function<std::int64_t()> now_us,
                                     PassthroughOptions options)
    : link_(&link), now_us_(std::move(now_us)), options_(options) {}

PassthroughOutput::~PassthroughOutput() {
    stop_thread();
    const std::lock_guard lock(mutex_);
    if (link_->is_open()) {
        link_->close();
    }
}

std::int64_t PassthroughOutput::frames_to_us(std::uint64_t frames) const {
    const std::uint32_t rate = format_ ? format_->sample_rate : 48000U;
    return static_cast<std::int64_t>((frames * 1'000'000ULL) / rate);
}

void PassthroughOutput::reset_clock_locked() {
    clock_samples_.clear();
    origin_us_.reset();
    offset_us_.reset();
    moving_ = false;
    underruns_based_ = false;
    opened_at_us_ = now_us_();
}

bool PassthroughOutput::start(const ac::StreamStart& stream) {
    const std::lock_guard lock(mutex_);
    pending_.clear();
    decoder_.reset();
    streaming_ = false;
    first_placed_ = false;
    started_at_us_ = now_us_();
    ++counts_.streams;

    const std::optional<LinkFormat> wanted = format_of(stream);
    if (!wanted) {
        return false;
    }
    if (format_ && link_->is_open() && same_format(*format_, *wanted)) {
        // The same bitstream is already on the link, which is kept alive between items: it is not
        // opened again, and the receiver does not lose its lock.
        streaming_ = true;
        return true;
    }
    if (link_->is_open()) {
        link_->close();
    }
    format_.reset();
    if (const std::expected<void, std::string> opened = link_->open(*wanted); !opened) {
        return false;
    }
    format_ = wanted;
    if (options_.note) {
        options_.note("link opened in " + std::to_string((now_us_() - started_at_us_) / 1000) + " ms");
    }
    const std::expected<std::vector<std::byte>, iec::WrapError> null = iec::null_burst(wanted->burst_bytes);
    if (!null) {
        link_->close();
        format_.reset();
        return false;
    }
    null_burst_ = *null;
    counts_.underruns = 0;
    reset_clock_locked();
    streaming_ = true;
    return true;
}

void PassthroughOutput::clear() {
    const std::lock_guard lock(mutex_);
    pending_.clear();
    if (format_ && link_->is_open()) {
        link_->flush();
        reset_clock_locked();
    }
}

void PassthroughOutput::end() {
    const std::lock_guard lock(mutex_);
    streaming_ = false;
    pending_.clear();
    ended_at_us_ = now_us_();
}

void PassthroughOutput::set_paused(bool paused) {
    const std::lock_guard lock(mutex_);
    if (paused_ == paused) {
        return;
    }
    paused_ = paused;
    pending_.clear();
    if (format_ && link_->is_open()) {
        link_->pause(paused);
        if (!paused) {
            // The device starts from what was queued; its clock does not carry over a pause.
            reset_clock_locked();
        }
    }
}

void PassthroughOutput::write(const sendspin::BurstChunk& chunk, std::int64_t local_time) {
    const std::lock_guard lock(mutex_);
    if (!format_ || !streaming_ || paused_) {
        ++counts_.dropped;
        return;
    }
    const std::span<const std::uint8_t> payload = chunk.chunk.data;
    const std::int64_t offset = local_time - chunk.chunk.timestamp_us;
    if (options_.note && offset_us_ && (offset > *offset_us_ + 8'000 || offset < *offset_us_ - 8'000)) {
        options_.note("time offset moved " + std::to_string((offset - *offset_us_) / 1000) + " ms (stream started " +
                      std::to_string((now_us_() - started_at_us_) / 1000) + " ms ago)");
    }
    offset_us_ = offset;
    if (!decoder_) {
        // What the first syncframe says, as a decoder would report it, without decoding it: the
        // receiver decodes. Objects and substreams are not read here (the receiver places them).
        const std::expected<ac3::io::FrameHeader, ac3::io::ScanError> header =
            ac3::io::read_frame_header(std::as_bytes(payload));
        if (header) {
            decoder_ = ac::DecoderReport{.data_type = format_->data_type,
                                         .acmod = static_cast<std::int32_t>(header->acmod),
                                         .lfe = header->lfe,
                                         .substreams = 1,
                                         .objects = 0,
                                         .objects_placed = false,
                                         .dialnorm = -static_cast<double>(header->dialnorm)};
        }
    }
    std::expected<std::vector<std::byte>, iec::WrapError> burst =
        iec::burst_from_parts(chunk.pc, chunk.pd, std::as_bytes(payload));
    if (!burst || burst->size() != format_->burst_bytes) {
        // A burst of another length than the stream's cannot go in the slot a link was opened for.
        ++counts_.dropped;
        return;
    }
    pending_.push_back({.burst = std::move(*burst), .due_us = local_time});
}

bool PassthroughOutput::streaming() const {
    const std::lock_guard lock(mutex_);
    return streaming_;
}

OutputCounts PassthroughOutput::counts() const {
    const std::lock_guard lock(mutex_);
    return counts_;
}

void PassthroughOutput::note_position_locked(std::int64_t now_us, const LinkPosition& position) {
    if (position.frames_played == 0) {
        return;
    }
    moving_ = true;
    clock_samples_.push_back(now_us - frames_to_us(position.frames_played));
    while (clock_samples_.size() > options_.clock_window) {
        clock_samples_.pop_front();
    }
    origin_us_ = *std::min_element(clock_samples_.begin(), clock_samples_.end());
}

void PassthroughOutput::fill_slots_locked(std::int64_t now_us) {
    if (!format_ || !link_->is_open()) {
        return;
    }
    const std::optional<LinkPosition> position = link_->position();
    if (!position) {
        return;
    }
    note_position_locked(now_us, *position);
    if (moving_) {
        if (!underruns_based_) {
            underruns_base_ = link_->underruns();
            underruns_based_ = true;
        }
        // A device that starved has stopped counting, and its clock is not the one it had.
        const std::uint64_t underruns = link_->underruns() - underruns_base_;
        if (underruns != counts_.underruns) {
            counts_.underruns = underruns;
            clock_samples_.clear();
            origin_us_.reset();
        }
    }

    const std::uint32_t per_burst = format_->frames_per_burst;
    const std::int64_t period_us = frames_to_us(per_burst);
    const std::uint64_t want = static_cast<std::uint64_t>(moving_ ? options_.bursts_ahead : options_.prime_bursts) * per_burst;
    std::uint64_t queued = position->frames_queued;
    const std::uint64_t played = position->frames_played;
    while (queued < want) {
        // When the burst submitted now would play: the origin of the link's frames plus what is
        // ahead of it. The device's first frame plays when it starts to move, which is about
        // start_latency_us after the first burst is handed over and no more certain than that, so
        // until it has moved only null bursts are written: a real one would be placed by a guess.
        const std::int64_t taken = static_cast<std::int64_t>(played + queued);
        const std::int64_t slot_us =
            origin_us_ ? *origin_us_ + frames_to_us(static_cast<std::uint64_t>(taken))
                       : opened_at_us_ + options_.start_latency_us + frames_to_us(static_cast<std::uint64_t>(taken));
        const bool placing = moving_ && origin_us_ && streaming_ && !paused_;
        if (placing) {
            // A burst more than half its length late cannot be on time in a slot, and is dropped
            // rather than played late: alignment with the group is what a pass-through member keeps.
            const auto late_us = static_cast<std::int64_t>(static_cast<double>(period_us) * options_.late_fraction);
            while (!pending_.empty() && pending_.front().due_us < slot_us - late_us) {
                if (options_.note) {
                    options_.note("burst dropped, " + std::to_string((slot_us - pending_.front().due_us) / 1000) +
                                  " ms late (stream started " + std::to_string((now_us - started_at_us_) / 1000) +
                                  " ms ago)");
                }
                pending_.pop_front();
                ++counts_.late;
                ++counts_.dropped;
            }
        }
        const bool due = placing && !pending_.empty() && pending_.front().due_us <= slot_us + period_us / 2;
        if (due) {
            if (!link_->submit(pending_.front().burst)) {
                break;
            }
            if (!first_placed_) {
                first_placed_ = true;
                if (options_.note) {
                    options_.note("first burst placed " + std::to_string((now_us - started_at_us_) / 1000) +
                                  " ms after the stream started, " +
                                  std::to_string((slot_us - pending_.front().due_us) / 1000) + " ms from its time");
                }
            }
            pending_.pop_front();
            ++counts_.units;
            counts_.frames += per_burst;
        } else if (!link_->submit(null_burst_)) {
            break;
        }
        queued += per_burst;
    }
}

void PassthroughOutput::pump() {
    const std::int64_t now = now_us_();
    const std::lock_guard lock(mutex_);
    if (!format_) {
        return;
    }
    if (!link_->is_open()) {
        // The device went away under the stream (the display slept, the cable moved). What was due
        // meanwhile is dropped, and counted as such: a run on a Shield whose HDMI link dropped every
        // 20 s showed a quarter of the bursts missing with no late or dropped burst reported. While
        // the stream is open the link is tried again, since the route comes back with the display.
        counts_.dropped += pending_.size();
        pending_.clear();
        if (streaming_ && now - reopened_at_us_ >= options_.reopen_period_us) {
            reopened_at_us_ = now;
            if (link_->open(*format_)) {
                if (options_.note) {
                    options_.note("link opened again");
                }
                reset_clock_locked();
                underruns_based_ = false;
            }
        } else if (!streaming_) {
            format_.reset();
        }
        return;
    }
    if (!streaming_ && now - ended_at_us_ > options_.idle_hold_us) {
        link_->close();
        format_.reset();
        return;
    }
    if (paused_) {
        return;
    }
    fill_slots_locked(now);
}

void PassthroughOutput::run(std::chrono::microseconds period) {
    stop_thread();
    stop_flag_.store(false);
    thread_ = std::thread([this, period] {
        while (!stop_flag_.load(std::memory_order_acquire)) {
            pump();
            std::this_thread::sleep_for(period);
        }
    });
}

void PassthroughOutput::stop_thread() {
    stop_flag_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

}  // namespace iclforge::sink
