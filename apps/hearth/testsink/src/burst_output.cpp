#include "burst_output.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace iclforge::hearth::testsink {

WavPcmSink::WavPcmSink(std::filesystem::path directory, std::string prefix)
    : directory_(std::move(directory)), prefix_(std::move(prefix)) {}

// writer_'s and log_'s own destructors close both files as close() does.
WavPcmSink::~WavPcmSink() = default;

bool WavPcmSink::open(std::uint32_t sample_rate, std::uint16_t channels) {
    close();
    ++streams_;
    if (directory_.empty()) {
        return true;
    }
    file_ = directory_ / (prefix_ + "-" + std::to_string(streams_) + ".wav");
    if (!writer_.open(file_.string(), sample_rate, channels)) {
        return false;
    }
    log_.open(std::filesystem::path(file_).replace_extension(".times.csv"), std::ios::trunc);
    log_ << "local_time_us,first_frame,frames\n";
    return true;
}

void WavPcmSink::burst(std::int64_t local_time_us, std::uint64_t first_frame, std::uint64_t frames) {
    if (log_.is_open()) {
        log_ << local_time_us << "," << first_frame << "," << frames << "\n";
    }
}

void WavPcmSink::write(std::span<const float> interleaved) {
    if (writer_.is_open()) {
        (void)writer_.write(interleaved);
    }
}

void WavPcmSink::clear(std::uint64_t frames) {
    if (log_.is_open()) {
        log_ << "clear," << frames << "\n";
    }
}

void WavPcmSink::close() {
    writer_.close();
    if (log_.is_open()) {
        log_.close();
    }
}

BurstOutput::BurstOutput(std::filesystem::path directory, std::string prefix, const render::OutputLayout& layout)
    : sink::DecodingBurstOutput(layout, std::make_unique<WavPcmSink>(std::move(directory), std::move(prefix))) {}

}  // namespace iclforge::hearth::testsink
