#include "take_recorder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <system_error>
#include <utility>

#include "iclforge/sendspin/codec.hpp"
#include "iclforge/sendspin/messages.hpp"

namespace iclforge::hearth {

namespace fs = std::filesystem;

namespace {

constexpr std::int32_t kBitDepth = 24;
constexpr double kFullScale = 8388607.0;  // 2^23 - 1

// STREAMINFO's sample count is the low 36 bits of the eight bytes that start at file offset 18:
// "fLaC" (4), the block header (4), then 10 bytes of block sizes and frame sizes, then
// sample rate (20 bits), channels - 1 (3), bits per sample - 1 (5) and the count (36).
constexpr std::streamoff kStreamInfoSamplesAt = 18;

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}",
                                       static_cast<unsigned>(static_cast<unsigned char>(c)));
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string utc_now() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

}  // namespace

struct TakeRecorder::Encoder {
    std::unique_ptr<sendspin::codec::Encoder> encoder;
};

TakeRecorder::TakeRecorder() = default;
TakeRecorder::~TakeRecorder() = default;
TakeRecorder::TakeRecorder(TakeRecorder&&) noexcept = default;
TakeRecorder& TakeRecorder::operator=(TakeRecorder&&) noexcept = default;

std::expected<void, std::string> TakeRecorder::begin(const fs::path& file,
                                                     std::uint32_t sample_rate) {
    if (encoder_) {
        return std::unexpected(std::string{"A take is already being recorded."});
    }
    std::error_code ec;
    if (file.has_parent_path()) {
        fs::create_directories(file.parent_path(), ec);
        if (ec) {
            return std::unexpected("The folder for takes could not be made: " + ec.message() + ".");
        }
    }
    sendspin::messages::AudioFormat format;
    format.codec = sendspin::messages::Codec::kFlac;
    format.channels = 1;
    format.sample_rate = static_cast<std::int32_t>(sample_rate);
    format.bit_depth = kBitDepth;
    auto made = sendspin::codec::make_encoder(format);
    if (!made) {
        return std::unexpected(std::string{"No FLAC encoder for this microphone's sample rate."});
    }
    out_.open(file, std::ios::binary | std::ios::trunc);
    if (!out_) {
        return std::unexpected("The take's file could not be created: " + utf8(file) + ".");
    }
    const std::vector<std::uint8_t>& header = made->codec_header();
    out_.write(reinterpret_cast<const char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    if (!out_) {
        out_.close();
        fs::remove(file, ec);
        return std::unexpected(std::string{"The take's file could not be written."});
    }
    encoder_ = std::make_unique<Encoder>();
    encoder_->encoder = std::move(made);
    path_ = file;
    rate_ = sample_rate;
    frames_ = 0;
    return {};
}

bool TakeRecorder::write(std::span<const float> mono) {
    if (!encoder_ || mono.empty()) {
        return encoder_ != nullptr;
    }
    scratch_.resize(mono.size());
    for (std::size_t i = 0; i < mono.size(); ++i) {
        const double v = std::clamp(static_cast<double>(mono[i]), -1.0, 1.0);
        scratch_[i] = static_cast<std::int32_t>(std::lround(v * kFullScale));
    }
    const auto units = encoder_->encoder->encode(scratch_);
    if (!units) {
        return false;
    }
    for (const auto& unit : *units) {
        out_.write(reinterpret_cast<const char*>(unit.bytes.data()),
                   static_cast<std::streamsize>(unit.bytes.size()));
    }
    frames_ += mono.size();
    return static_cast<bool>(out_);
}

std::expected<TakeInfo, std::string> TakeRecorder::finish(const TakeMetadata& metadata) {
    if (!encoder_) {
        return std::unexpected(std::string{"No take is being recorded."});
    }
    const auto last = encoder_->encoder->finish();
    if (last) {
        for (const auto& unit : *last) {
            out_.write(reinterpret_cast<const char*>(unit.bytes.data()),
                       static_cast<std::streamsize>(unit.bytes.size()));
        }
    }
    // The duration into STREAMINFO.
    out_.flush();
    if (out_) {
        out_.seekp(kStreamInfoSamplesAt, std::ios::beg);
        std::ifstream in(path_, std::ios::binary);
        in.seekg(kStreamInfoSamplesAt, std::ios::beg);
        unsigned char packed[8] = {};
        in.read(reinterpret_cast<char*>(packed), 8);
        if (in.gcount() == 8) {
            std::uint64_t word = 0;
            for (const unsigned char b : packed) {
                word = (word << 8) | b;
            }
            constexpr std::uint64_t kMask = (std::uint64_t{1} << 36) - 1;
            word = (word & ~kMask) | (frames_ & kMask);
            for (int i = 7; i >= 0; --i) {
                packed[7 - i] = static_cast<unsigned char>((word >> (8 * i)) & 0xFF);
            }
            out_.write(reinterpret_cast<const char*>(packed), 8);
        }
    }
    out_.close();

    TakeInfo info;
    info.audio = path_;
    info.sidecar = path_;
    info.sidecar.replace_extension(".take.json");
    info.frames = frames_;
    info.sample_rate = rate_;
    info.seconds = rate_ > 0 ? static_cast<double>(frames_) / static_cast<double>(rate_) : 0.0;

    std::string json = "{\n";
    json += "  \"format\": \"iclforge-take-1\",\n";
    json += "  \"audio\": " + json_string(utf8(path_.filename())) + ",\n";
    json += "  \"dry_voice\": true,\n";
    json += std::format("  \"sample_rate\": {},\n  \"channels\": 1,\n  \"bit_depth\": {},\n", rate_,
                        kBitDepth);
    json += std::format("  \"frames\": {},\n  \"seconds\": {:.3f},\n", frames_, info.seconds);
    json += "  \"created_utc\": " + json_string(utc_now()) + ",\n";
    json += "  \"item\": {\"path\": " + json_string(metadata.item_path) +
            ", \"title\": " + json_string(metadata.item_title) + "},\n";
    if (metadata.item_start_ms) {
        json += std::format("  \"item_start_ms\": {},\n", *metadata.item_start_ms);
    } else {
        json += "  \"item_start_ms\": null,\n";
    }
    json += std::format(
        "  \"alignment\": {{\"input_delay_ms\": {}, \"offset_ms\": {}, \"timing_estimated\": "
        "{}}},\n",
        metadata.input_delay_ms, metadata.offset_ms, metadata.timing_estimated ? "true" : "false");
    json += "  \"reference\": " + json_string(metadata.reference) + ",\n";
    json += std::format("  \"headphones\": {}\n", metadata.headphones ? "true" : "false");
    json += "}\n";
    std::ofstream side(info.sidecar, std::ios::binary | std::ios::trunc);
    side.write(json.data(), static_cast<std::streamsize>(json.size()));
    side.close();

    encoder_.reset();
    if (!side) {
        return std::unexpected(
            std::string{"The take was saved but its description could not be written."});
    }
    return info;
}

void TakeRecorder::discard() {
    if (!encoder_) {
        return;
    }
    encoder_.reset();
    out_.close();
    std::error_code ec;
    fs::remove(path_, ec);
}

std::string take_file_stem(const std::string& title, const std::string& timestamp) {
    std::string stem;
    for (const char c : title) {
        const bool bad = static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>' || c == ':' ||
                         c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*';
        stem += bad ? '_' : c;
    }
    // At most 80 bytes, never through the middle of a UTF-8 sequence.
    if (stem.size() > 80) {
        stem.resize(80);
        while (!stem.empty() && (static_cast<unsigned char>(stem.back()) & 0xC0U) == 0x80U) {
            stem.pop_back();
        }
        if (!stem.empty() && (static_cast<unsigned char>(stem.back()) & 0xC0U) == 0xC0U) {
            stem.pop_back();
        }
    }
    // Do not end on a space or a dot (Windows refuses it), nor begin on one.
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) {
        stem.pop_back();
    }
    const auto first = stem.find_first_not_of(" .");
    stem = first == std::string::npos ? std::string{} : stem.substr(first);
    if (stem.empty()) {
        stem = "Take";
    }
    return stem + " - " + timestamp;
}

}  // namespace iclforge::hearth
