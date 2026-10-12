#include "lyric_osc.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace iclforge::crucible::ui {

namespace {

// Big-endian, as OSC has every number.
void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>(value & 0xFFU));
}

// An OSC string: the bytes, a NUL, and NULs to a multiple of four.
void put_string(std::vector<std::byte>& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    out.push_back(std::byte{0});
    while (out.size() % 4 != 0) {
        out.push_back(std::byte{0});
    }
}

[[nodiscard]] OscMessage make_message(std::string address, std::vector<OscArg> args) {
    OscMessage out;
    out.address = std::move(address);
    out.args = std::move(args);
    return out;
}

[[nodiscard]] std::int32_t ms(std::int64_t value) {
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(value, 0, 0x7FFFFFFF));
}

}  // namespace

std::string osc_string(std::string_view text) {
    std::string out;
    out.reserve(std::min(text.size(), kOscMaxStringBytes));
    for (const char c : text) {
        if (c != '\0') {
            out.push_back(c);
        }
    }
    if (out.size() > kOscMaxStringBytes) {
        std::size_t cut = kOscMaxStringBytes;
        // Back to the start of a character: a continuation byte is 10xxxxxx.
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0U) == 0x80U) {
            --cut;
        }
        out.resize(cut);
    }
    return out;
}

std::vector<std::byte> encode_osc(const OscMessage& message) {
    std::vector<std::byte> out;
    put_string(out, message.address);
    std::string tags = ",";
    for (const OscArg& arg : message.args) {
        switch (arg.type) {
            case OscArg::Type::kInt:
                tags.push_back('i');
                break;
            case OscArg::Type::kFloat:
                tags.push_back('f');
                break;
            case OscArg::Type::kString:
                tags.push_back('s');
                break;
        }
    }
    put_string(out, tags);
    for (const OscArg& arg : message.args) {
        switch (arg.type) {
            case OscArg::Type::kInt:
                put_u32(out, static_cast<std::uint32_t>(arg.integer));
                break;
            case OscArg::Type::kFloat: {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &arg.real, sizeof(bits));
                put_u32(out, bits);
                break;
            }
            case OscArg::Type::kString:
                put_string(out, osc_string(arg.text));
                break;
        }
    }
    return out;
}

std::vector<OscMessage> LyricEventTracker::update(const std::vector<LyricSnapshot>& readings, std::int64_t now_ms) {
    std::vector<OscMessage> out;
    std::vector<int> present;
    present.reserve(readings.size());

    for (const LyricSnapshot& now : readings) {
        present.push_back(now.key);
        const auto it = last_.find(now.key);
        const bool first = it == last_.end();
        const LyricSnapshot* before = first ? nullptr : &it->second.snapshot;
        const bool was_following = before != nullptr && before->following;

        // Following started or stopped.
        if (options_.state && now.following != was_following && (now.following || before != nullptr)) {
            out.push_back(make_message("/crucible/lyrics/state",
                                  {OscArg::of(osc_string(now.app)), OscArg::of(std::string(now.following ? "following" : "stopped"))}));
        }

        if (now.following) {
            // A line is said when it is entered or left, and said again when another takes its place.
            const bool line_changed = !was_following || before->line != now.line;
            if (options_.lines && line_changed) {
                out.push_back(make_message("/crucible/lyrics/line",
                                      {OscArg::of(osc_string(now.app)), OscArg::of(static_cast<std::int32_t>(now.line)),
                                       OscArg::of(osc_string(now.line >= 0 ? now.line_text : std::string{})),
                                       OscArg::of(ms(now.line >= 0 ? now.line_start_ms : 0)),
                                       OscArg::of(ms(now.line >= 0 ? now.line_end_ms : 0))}));
            }
            // A word is said when a different one of a line is being sung; leaving a word for no word
            // is not said (the line's own message says the line is over).
            const bool word_changed = !was_following || before->line != now.line || before->word != now.word;
            if (options_.words && word_changed && now.word >= 0) {
                out.push_back(make_message("/crucible/lyrics/word",
                                      {OscArg::of(osc_string(now.app)), OscArg::of(static_cast<std::int32_t>(now.line)),
                                       OscArg::of(static_cast<std::int32_t>(now.word)),
                                       OscArg::of(osc_string(now.word_text)), OscArg::of(ms(now.word_start_ms)),
                                       OscArg::of(ms(now.word_end_ms))}));
            }
        }

        Last& last = last_[now.key];
        if (now.following && options_.position && now.position_ms >= 0) {
            const bool due = !last.position_sent || now_ms - last.position_sent_ms >= options_.position_period_ms
                             || !was_following;
            if (due) {
                out.push_back(make_message("/crucible/lyrics/position",
                                      {OscArg::of(osc_string(now.app)), OscArg::of(ms(now.position_ms))}));
                last.position_sent = true;
                last.position_sent_ms = now_ms;
            }
        } else if (!now.following) {
            last.position_sent = false;
        }
        last.snapshot = now;
    }

    // An application that is not in this reading is gone: say so once, and forget it.
    for (auto it = last_.begin(); it != last_.end();) {
        if (std::find(present.begin(), present.end(), it->first) == present.end()) {
            if (options_.state) {
                out.push_back(make_message("/crucible/lyrics/state",
                                      {OscArg::of(osc_string(it->second.snapshot.app)), OscArg::of(std::string("cleared"))}));
            }
            it = last_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

}  // namespace iclforge::crucible::ui
