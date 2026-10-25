#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/firmware_status.hpp"

// Updates from GitHub releases (planning/esp32-ota.md, O10): the part of a board's "Check for
// updates" that is data and rules, free of ESP-IDF like firmware_image.hpp beside it, so every rule
// is exercised on a laptop (libs/device/tests/test_firmware_update.cpp) and the screen, the page,
// ota.py and ac3hearth draw the same words.
//
// A board that pulls reads two small files for its channel (O10, "What the board reads"):
//
//   hearth-update-<channel>.json       the update index: which release is newest and, for each
//                                      image, its size and SHA-256 and the facts a board matches
//                                      on;
//   hearth-update-<channel>.sig.json   a signature over the index file's exact bytes, with the
//                                      certificate chain that made it.
//
// This header parses both and decides what to do with them. It does NOT check the signature: that
// is mbedTLS's, in firmware_pull.cpp (a chain to the trust anchors in the build, the leaf's usage
// and pinned name, then the signature over the index file), and tools/hearth/index_verify.py is the
// same check in Python. What this header gives that check is the bytes it works on
// (parse_signature_file), and what it gives the rest is everything that follows once the pair is
// trusted: which image is this board's, whether it is newer, whether it may be installed, and what
// to say about it.
//
// The rules that must agree with index_verify.py's (what an index may contain, what a version is,
// how versions order) are written twice on purpose, and both test suites hold the same cases.

namespace iclforge::update {

inline constexpr std::size_t kMaxIndexBytes = 4096;
inline constexpr std::size_t kMaxSignatureBytes = 8192;
inline constexpr std::size_t kMaxImages = 16;
inline constexpr std::size_t kMaxChainCertificates =
    2;  // the leaf and the sub-CA; the root is never sent
inline constexpr std::uint64_t kMaxImageBytes = 16ULL * 1024 * 1024;

// --- versions
// -------------------------------------------------------------------------------------

// A version as ordered for an update: v0.11.0-beta.1 is newer than v0.10.0-beta.1-2039-gf5aded471
// (a build 2,039 commits past a tag sorts after that tag and before the next release), and -beta.N
// sorts before the same number without it. `git describe` is what a build calls itself, so the hash
// and
// "-dirty" are accepted and ignored.
struct Version {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    std::uint32_t stage = 4;  // 1 alpha, 2 beta, 3 rc, 4 a release
    std::uint32_t pre = 0;
    std::uint32_t commits = 0;

    [[nodiscard]] friend constexpr bool operator==(const Version& a, const Version& b) {
        return a.key() == b.key();
    }
    [[nodiscard]] friend constexpr bool operator<(const Version& a, const Version& b) {
        return a.key() < b.key();
    }

   private:
    [[nodiscard]] constexpr std::array<std::uint32_t, 6> key() const {
        return {major, minor, patch, stage, pre, commits};
    }
};

enum class Order : std::uint8_t { kOlder, kSame, kNewer };

namespace impl {

[[nodiscard]] inline bool take_number(std::string_view& text, std::uint32_t& out) {
    std::size_t used = 0;
    std::uint64_t value = 0;
    while (used < text.size() && text[used] >= '0' && text[used] <= '9') {
        value = value * 10 + static_cast<std::uint64_t>(text[used] - '0');
        if (++used > 9) {
            return false;
        }
    }
    if (used == 0) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    text.remove_prefix(used);
    return true;
}

[[nodiscard]] inline bool take_prefix(std::string_view& text, std::string_view prefix) {
    if (text.substr(0, prefix.size()) != prefix) {
        return false;
    }
    text.remove_prefix(prefix.size());
    return true;
}

}  // namespace impl

// The version in `text`, or nothing when it is not one this board understands ("1", a bare hash,
// "main").
[[nodiscard]] inline std::optional<Version> parse_version(std::string_view text) {
    Version v;
    (void)impl::take_prefix(text, "v");
    if (!impl::take_number(text, v.major) || !impl::take_prefix(text, ".") ||
        !impl::take_number(text, v.minor) || !impl::take_prefix(text, ".") ||
        !impl::take_number(text, v.patch)) {
        return std::nullopt;
    }
    for (const auto& [name, rank] : {std::pair<std::string_view, std::uint32_t>{"-alpha.", 1},
                                     std::pair<std::string_view, std::uint32_t>{"-beta.", 2},
                                     std::pair<std::string_view, std::uint32_t>{"-rc.", 3}}) {
        if (impl::take_prefix(text, name)) {
            v.stage = rank;
            if (!impl::take_number(text, v.pre)) {
                return std::nullopt;
            }
            break;
        }
    }
    // "-<commits>-g<hash>": a build past the tag. The hash may have been cut short by the 31
    // characters a board keeps of its version.
    if (!text.empty() && text.front() == '-' && text.size() > 1 && text[1] >= '0' &&
        text[1] <= '9') {
        text.remove_prefix(1);
        if (!impl::take_number(text, v.commits) || !impl::take_prefix(text, "-g")) {
            return std::nullopt;
        }
        std::size_t hex = 0;
        while (hex < text.size() &&
               ((text[hex] >= '0' && text[hex] <= '9') || (text[hex] >= 'a' && text[hex] <= 'f'))) {
            ++hex;
        }
        text.remove_prefix(hex);
    }
    (void)impl::take_prefix(text, "-dirty");
    if (!text.empty()) {
        return std::nullopt;
    }
    return v;
}

// How `candidate` stands against `running`; nothing when either is not a version (never "newer").
[[nodiscard]] inline std::optional<Order> compare_versions(std::string_view candidate,
                                                           std::string_view running) {
    const auto a = parse_version(candidate);
    const auto b = parse_version(running);
    if (!a || !b) {
        return std::nullopt;
    }
    if (*b < *a) {
        return Order::kNewer;
    }
    if (*a < *b) {
        return Order::kOlder;
    }
    return Order::kSame;
}

// --- base64 and a small strict JSON reader
// ----------------------------------------------------------

// RFC 4648 with padding and no whitespace: what sign_index.py writes.
[[nodiscard]] inline std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text) {
    if (text.empty() || text.size() % 4 != 0) {
        return std::nullopt;
    }
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        return c == '+' ? 62 : (c == '/' ? 63 : -1);
    };
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int quad[4];
        int padding = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=') {
                if (!last || j < 2) {
                    return std::nullopt;
                }
                ++padding;
                quad[j] = 0;
            } else if (padding > 0 || value(c) < 0) {
                return std::nullopt;  // data after padding, or a character outside the alphabet
            } else {
                quad[j] = value(c);
            }
        }
        const std::uint32_t triple = (static_cast<std::uint32_t>(quad[0]) << 18) |
                                     (static_cast<std::uint32_t>(quad[1]) << 12) |
                                     (static_cast<std::uint32_t>(quad[2]) << 6) |
                                     static_cast<std::uint32_t>(quad[3]);
        out.push_back(static_cast<std::uint8_t>(triple >> 16));
        if (padding < 2) {
            out.push_back(static_cast<std::uint8_t>(triple >> 8));
        }
        if (padding < 1) {
            out.push_back(static_cast<std::uint8_t>(triple));
        }
    }
    return out;
}

namespace json {

struct Value {
    enum class Type : std::uint8_t { kNull, kBool, kNumber, kString, kArray, kObject };
    Type type = Type::kNull;
    bool boolean = false;
    bool integral = false;  // a number with no fraction or exponent that fits 63 bits
    std::int64_t integer = 0;
    std::string text;
    std::vector<Value> items;        // an array's, or an object's values
    std::vector<std::string> names;  // an object's, parallel to items

    [[nodiscard]] const Value* find(std::string_view name) const {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) {
                return &items[i];
            }
        }
        return nullptr;
    }
};

struct Parsed {
    std::optional<Value> value;
    std::string why;
};

namespace impl {

constexpr int kMaxDepth = 8;
constexpr std::size_t kMaxNodes = 1024;
constexpr std::size_t kMaxString = 4096;

class Reader {
   public:
    explicit Reader(std::string_view text) : s_(text) {}

    [[nodiscard]] Parsed run() {
        Parsed out;
        skip();
        Value v;
        if (!value(v, 0)) {
            out.why = why_.empty() ? "not JSON" : why_;
            return out;
        }
        skip();
        if (pos_ != s_.size()) {
            out.why = "text after the JSON";
            return out;
        }
        out.value = std::move(v);
        return out;
    }

   private:
    bool fail(const char* why) {
        if (why_.empty()) {
            why_ = why;
        }
        return false;
    }
    void skip() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) {
            ++pos_;
        }
    }
    [[nodiscard]] bool word(std::string_view w) {
        if (s_.substr(pos_, w.size()) != w) {
            return false;
        }
        pos_ += w.size();
        return true;
    }

    bool value(Value& v, int depth) {
        if (depth > kMaxDepth) {
            return fail("nested too deeply");
        }
        if (++nodes_ > kMaxNodes) {
            return fail("too many values");
        }
        if (pos_ >= s_.size()) {
            return fail("cut short");
        }
        const char c = s_[pos_];
        if (c == '{') {
            return object(v, depth);
        }
        if (c == '[') {
            return array(v, depth);
        }
        if (c == '"') {
            v.type = Value::Type::kString;
            return string(v.text);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return number(v);
        }
        if (word("true")) {
            v.type = Value::Type::kBool;
            v.boolean = true;
            return true;
        }
        if (word("false")) {
            v.type = Value::Type::kBool;
            return true;
        }
        if (word("null")) {
            return true;
        }
        return fail("not JSON");
    }

    bool object(Value& v, int depth) {
        v.type = Value::Type::kObject;
        ++pos_;
        skip();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip();
            std::string name;
            if (pos_ >= s_.size() || s_[pos_] != '"' || !string(name)) {
                return fail("an object member has no name");
            }
            for (const std::string& seen : v.names) {
                if (seen == name) {
                    return fail("an object names a member twice");
                }
            }
            skip();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                return fail("a member has no value");
            }
            ++pos_;
            skip();
            Value member;
            if (!value(member, depth + 1)) {
                return false;
            }
            v.names.push_back(std::move(name));
            v.items.push_back(std::move(member));
            skip();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("an object is not closed");
        }
    }

    bool array(Value& v, int depth) {
        v.type = Value::Type::kArray;
        ++pos_;
        skip();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip();
            Value item;
            if (!value(item, depth + 1)) {
                return false;
            }
            v.items.push_back(std::move(item));
            skip();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("an array is not closed");
        }
    }

    bool number(Value& v) {
        v.type = Value::Type::kNumber;
        const std::size_t start = pos_;
        if (s_[pos_] == '-') {
            ++pos_;
        }
        std::size_t digits = 0;
        std::uint64_t magnitude = 0;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
            if (digits < 18) {
                magnitude = magnitude * 10 + static_cast<std::uint64_t>(s_[pos_] - '0');
            }
            ++digits;
            ++pos_;
        }
        if (digits == 0 || (digits > 1 && s_[start + (s_[start] == '-' ? 1 : 0)] == '0')) {
            return fail("a number is malformed");
        }
        bool plain = digits <= 18;
        const auto run_of_digits = [this]() {
            std::size_t count = 0;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                ++pos_;
                ++count;
            }
            return count;
        };
        if (pos_ < s_.size() && s_[pos_] == '.') {
            plain = false;
            ++pos_;
            if (run_of_digits() == 0) {
                return fail("a number is malformed");
            }
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            plain = false;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
                ++pos_;
            }
            if (run_of_digits() == 0) {
                return fail("a number is malformed");
            }
        }
        v.integral = plain;
        if (plain) {
            v.integer = s_[start] == '-' ? -static_cast<std::int64_t>(magnitude)
                                         : static_cast<std::int64_t>(magnitude);
        }
        return true;
    }

    bool string(std::string& out) {
        ++pos_;  // the opening quote
        while (true) {
            if (pos_ >= s_.size()) {
                return fail("a string is not closed");
            }
            const char c = s_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("a string holds a control character");
            }
            if (out.size() >= kMaxString) {
                return fail("a string is too long");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) {
                return fail("a string is not closed");
            }
            const char e = s_[pos_++];
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    out += e;
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) {
                        return fail("a string escape is cut short");
                    }
                    std::uint32_t code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = s_[pos_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') {
                            code |= static_cast<std::uint32_t>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            code |= static_cast<std::uint32_t>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            code |= static_cast<std::uint32_t>(h - 'A' + 10);
                        } else {
                            return fail("a string escape is malformed");
                        }
                    }
                    if (code >= 0xD800 && code <= 0xDFFF) {
                        return fail("a string holds a surrogate escape");
                    }
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default:
                    return fail("a string escape is unknown");
            }
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    std::size_t nodes_ = 0;
    std::string why_;
};

}  // namespace impl

// Bounded: nested at most 9 levels, 1,024 values, strings to 4 KiB, no member named twice.
[[nodiscard]] inline Parsed parse(std::string_view text) {
    return impl::Reader(text).run();
}

}  // namespace json

// --- the update index
// -----------------------------------------------------------------------------

struct IndexImage {
    std::string name;
    std::string file;
    std::uint64_t size = 0;
    std::string sha256;  // 64 lower-case hex digits
    std::string project;
    std::uint32_t chip_id = 0;
    std::uint32_t min_rev_full = 0;
    std::uint32_t max_rev_full = 0;
    std::uint32_t flash_mb = 0;
    bool psram = false;
    std::uint32_t secure_version = 0;
};

struct UpdateIndex {
    std::string channel;  // "stable" or "beta"
    std::uint32_t sequence = 0;
    std::string version;
    std::string released;  // YYYY-MM-DD
    std::vector<IndexImage> images;
};

struct ParsedIndex {
    std::optional<UpdateIndex> index;
    std::string why;
};

namespace impl {

[[nodiscard]] inline bool is_hex64(std::string_view s) {
    if (s.size() != 64) {
        return false;
    }
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// A file name a board will build a URL from: [A-Za-z0-9_+-][A-Za-z0-9._+-]{0,95}.
[[nodiscard]] inline bool is_release_file_name(std::string_view s) {
    if (s.empty() || s.size() > 96) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool word = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') || c == '_' || c == '+' || c == '-';
        if (!(word || (c == '.' && i > 0))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool is_date(std::string_view s) {
    if (s.size() != 10) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool dash = i == 4 || i == 7;
        if (dash ? s[i] != '-' : !(s[i] >= '0' && s[i] <= '9')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline const json::Value* member(const json::Value& object, std::string_view name,
                                               json::Value::Type type) {
    const json::Value* v = object.find(name);
    return v != nullptr && v->type == type ? v : nullptr;
}

[[nodiscard]] inline bool whole(const json::Value& object, std::string_view name, std::int64_t low,
                                std::int64_t high, std::int64_t& out) {
    const json::Value* v = member(object, name, json::Value::Type::kNumber);
    if (v == nullptr || !v->integral || v->integer < low || v->integer > high) {
        return false;
    }
    out = v->integer;
    return true;
}

[[nodiscard]] inline bool text_of(const json::Value& object, std::string_view name,
                                  std::string& out) {
    const json::Value* v = member(object, name, json::Value::Type::kString);
    if (v == nullptr || v->text.empty()) {
        return false;
    }
    out = v->text;
    return true;
}

[[nodiscard]] inline bool read_image(const json::Value& object, IndexImage& image,
                                     std::string& why) {
    if (object.type != json::Value::Type::kObject) {
        why = "an image is not an object";
        return false;
    }
    std::int64_t n = 0;
    if (!text_of(object, "name", image.name) || !text_of(object, "project", image.project)) {
        why = "an image has no name or project";
        return false;
    }
    if (!text_of(object, "file", image.file) || !is_release_file_name(image.file)) {
        why = "an image's file name is not one a board will fetch";
        return false;
    }
    if (!text_of(object, "sha256", image.sha256) || !is_hex64(image.sha256)) {
        why = "an image's sha256 is not 64 lower-case hex digits";
        return false;
    }
    if (!whole(object, "size", 1, static_cast<std::int64_t>(kMaxImageBytes), n)) {
        why = "an image's size is out of range";
        return false;
    }
    image.size = static_cast<std::uint64_t>(n);
    if (!whole(object, "chip_id", 0, 0xFFFF, n)) {
        why = "an image's chip_id is out of range";
        return false;
    }
    image.chip_id = static_cast<std::uint32_t>(n);
    if (!whole(object, "min_rev_full", 0, 0xFFFF, n)) {
        why = "an image's min_rev_full is out of range";
        return false;
    }
    image.min_rev_full = static_cast<std::uint32_t>(n);
    if (!whole(object, "max_rev_full", 0, 0xFFFF, n)) {
        why = "an image's max_rev_full is out of range";
        return false;
    }
    image.max_rev_full = static_cast<std::uint32_t>(n);
    if (!whole(object, "secure_version", 0, 0xFFFFFFFFLL, n)) {
        why = "an image's secure_version is out of range";
        return false;
    }
    image.secure_version = static_cast<std::uint32_t>(n);
    const json::Value* psram = member(object, "psram", json::Value::Type::kBool);
    if (psram == nullptr) {
        why = "an image's psram is not true or false";
        return false;
    }
    image.psram = psram->boolean;
    std::string flash;
    if (!text_of(object, "flash_size", flash) || flash.size() < 3 || flash.size() > 5 ||
        flash.substr(flash.size() - 2) != "MB") {
        why = "an image's flash_size is not like 16MB";
        return false;
    }
    std::uint32_t mb = 0;
    for (std::size_t i = 0; i + 2 < flash.size(); ++i) {
        if (flash[i] < '0' || flash[i] > '9') {
            why = "an image's flash_size is not like 16MB";
            return false;
        }
        mb = mb * 10 + static_cast<std::uint32_t>(flash[i] - '0');
    }
    image.flash_mb = mb;
    return true;
}

}  // namespace impl

// The index in `text` under the rules index_verify.py's check_index holds it to.
[[nodiscard]] inline ParsedIndex parse_index(std::string_view text) {
    ParsedIndex out;
    if (text.size() > kMaxIndexBytes) {
        out.why = "the update list is longer than a board reads";
        return out;
    }
    const json::Parsed parsed = json::parse(text);
    if (!parsed.value) {
        out.why = "the update list is not JSON (" + parsed.why + ")";
        return out;
    }
    const json::Value& root = *parsed.value;
    std::int64_t n = 0;
    if (root.type != json::Value::Type::kObject || !impl::whole(root, "format", 1, 1, n)) {
        out.why = "the update list is not format 1";
        return out;
    }
    UpdateIndex index;
    if (!impl::text_of(root, "channel", index.channel) ||
        (index.channel != "stable" && index.channel != "beta")) {
        out.why = "the update list is for no channel this board knows";
        return out;
    }
    if (!impl::whole(root, "sequence", 1, 0xFFFFFFFFLL, n)) {
        out.why = "the update list has no sequence number";
        return out;
    }
    index.sequence = static_cast<std::uint32_t>(n);
    if (!impl::text_of(root, "version", index.version) || !parse_version(index.version)) {
        out.why = "the update list's version is not one this board understands";
        return out;
    }
    if (!impl::text_of(root, "released", index.released) || !impl::is_date(index.released)) {
        out.why = "the update list has no release date";
        return out;
    }
    const json::Value* images = impl::member(root, "images", json::Value::Type::kArray);
    if (images == nullptr || images->items.empty() || images->items.size() > kMaxImages) {
        out.why = "the update list names no images, or more than 16";
        return out;
    }
    for (const json::Value& entry : images->items) {
        IndexImage image;
        if (!impl::read_image(entry, image, out.why)) {
            return out;
        }
        index.images.push_back(std::move(image));
    }
    out.index = std::move(index);
    return out;
}

// --- the signature file
// ---------------------------------------------------------------------------

enum class SignatureAlgorithm : std::uint8_t { kRsaPkcs1Sha256, kRsaPssSha256, kEcdsaP256Sha256 };

[[nodiscard]] constexpr const char* signature_algorithm_name(SignatureAlgorithm alg) {
    switch (alg) {
        case SignatureAlgorithm::kRsaPssSha256:
            return "rsa-pss-sha256";
        case SignatureAlgorithm::kEcdsaP256Sha256:
            return "ecdsa-p256-sha256";
        case SignatureAlgorithm::kRsaPkcs1Sha256:
            break;
    }
    return "rsa-pkcs1-sha256";
}

struct SignatureFile {
    SignatureAlgorithm algorithm = SignatureAlgorithm::kRsaPkcs1Sha256;
    std::string index_sha256;  // hex, the index file's SHA-256 as the signer saw it
    std::vector<std::vector<std::uint8_t>> chain;  // DER, the leaf first; the root is never sent
    std::vector<std::uint8_t> signature;           // PKCS#1 or PSS, or ECDSA as raw r||s
};

struct ParsedSignature {
    std::optional<SignatureFile> file;
    std::string code;  // "bad_sig_file" when file is empty
    std::string why;
};

[[nodiscard]] inline ParsedSignature parse_signature_file(std::string_view text) {
    ParsedSignature out;
    out.code = "bad_sig_file";
    if (text.size() > kMaxSignatureBytes) {
        out.why = "the signature file is longer than a board reads";
        return out;
    }
    const json::Parsed parsed = json::parse(text);
    if (!parsed.value || parsed.value->type != json::Value::Type::kObject) {
        out.why = "the signature file is not JSON";
        return out;
    }
    const json::Value& root = *parsed.value;
    std::int64_t n = 0;
    if (!impl::whole(root, "format", 1, 1, n)) {
        out.why = "the signature file is not format 1";
        return out;
    }
    SignatureFile file;
    std::string alg;
    if (!impl::text_of(root, "alg", alg)) {
        out.why = "the signature file names no algorithm";
        return out;
    }
    if (alg == "rsa-pkcs1-sha256") {
        file.algorithm = SignatureAlgorithm::kRsaPkcs1Sha256;
    } else if (alg == "rsa-pss-sha256") {
        file.algorithm = SignatureAlgorithm::kRsaPssSha256;
    } else if (alg == "ecdsa-p256-sha256") {
        file.algorithm = SignatureAlgorithm::kEcdsaP256Sha256;
    } else {
        out.why = "the signature algorithm is not one a board knows";
        return out;
    }
    // The index's hash is checked later (a mismatch is its own code), so a missing one is left
    // empty.
    if (const json::Value* hash = impl::member(root, "sha256", json::Value::Type::kString)) {
        file.index_sha256 = hash->text;
    }
    const json::Value* chain = impl::member(root, "chain", json::Value::Type::kArray);
    if (chain == nullptr || chain->items.empty() || chain->items.size() > kMaxChainCertificates) {
        out.why = "the signature file's chain is not one or two certificates";
        return out;
    }
    for (const json::Value& item : chain->items) {
        auto der =
            item.type == json::Value::Type::kString ? base64_decode(item.text) : std::nullopt;
        if (!der) {
            out.why = "the signature file's chain holds text that is not base64";
            return out;
        }
        file.chain.push_back(std::move(*der));
    }
    const json::Value* signature = impl::member(root, "signature", json::Value::Type::kString);
    auto raw = signature != nullptr ? base64_decode(signature->text) : std::nullopt;
    if (!raw) {
        out.why = "the signature is not base64";
        return out;
    }
    file.signature = std::move(*raw);
    out.file = std::move(file);
    out.code.clear();
    return out;
}

// The signature file belongs to this index file: the SHA-256 it records is the one the board
// computed.
[[nodiscard]] inline bool signature_is_for(const SignatureFile& file,
                                           std::string_view index_sha256_hex) {
    return !file.index_sha256.empty() && file.index_sha256 == index_sha256_hex;
}

// What a check of the chain and the signature answered (firmware_pull.cpp fills it from mbedTLS;
// the codes are index_verify.py's): ok, or a code and the words for it.
struct TrustOutcome {
    bool ok = false;
    std::string code;       // untrusted, cert_expired, cert_not_yet_valid, leaf_usage, leaf_name,
                            // weak_key, bad_signature
    std::string leaf_name;  // the leaf's common name, when it was trusted
};

// --- which image, and whether to offer it
// ----------------------------------------------------------

// What the board is, to be matched against an index entry: the same facts firmware_image.hpp's
// refuse_image holds an image to, and its PSRAM.
struct BoardTarget {
    std::string project;
    std::uint32_t chip_id = 0;
    std::uint32_t revision_full = 0;  // major * 100 + minor
    std::uint32_t flash_mb = 0;
    bool psram = false;
    std::uint64_t slot_bytes = 0;
};

// The first image in the index this board can run, or nothing. A new board variant is offered by
// being in a release: nothing here names a board.
[[nodiscard]] inline const IndexImage* select_image(const UpdateIndex& index,
                                                    const BoardTarget& board) {
    for (const IndexImage& image : index.images) {
        if (image.project == board.project && image.chip_id == board.chip_id &&
            board.revision_full >= image.min_rev_full &&
            board.revision_full <= image.max_rev_full && image.flash_mb == board.flash_mb &&
            image.psram == board.psram && image.size <= board.slot_bytes) {
            return &image;
        }
    }
    return nullptr;
}

struct PullFacts {
    std::string channel;  // what the board follows
    std::string running_version;
    std::uint32_t running_secure_version = 0;
    std::uint32_t last_sequence = 0;  // the highest this board has accepted for its channel
    std::string skipped_version;      // a version that went back on this board, or empty
};

enum class Verdict : std::uint8_t { kAvailable, kUpToDate, kRefused };

struct Decision {
    Verdict verdict = Verdict::kRefused;
    std::string code;  // for a refusal: the code error_text() has words for
    const IndexImage* image = nullptr;
    bool skipped = false;  // available, and the same version went back on this board before
};

// What to do with an index that came from a trusted signature (O10, "What an update may do").
[[nodiscard]] inline Decision decide(const UpdateIndex& index, const BoardTarget& board,
                                     const PullFacts& facts) {
    Decision d;
    if (index.channel != facts.channel) {
        d.code = "bad_index";
        return d;
    }
    if (index.sequence < facts.last_sequence) {
        d.code = "replayed";
        return d;
    }
    const std::optional<Order> order = compare_versions(index.version, facts.running_version);
    if (!order) {
        d.code = "unknown_version";
        return d;
    }
    if (*order != Order::kNewer) {
        d.verdict = Verdict::kUpToDate;
        return d;
    }
    d.image = select_image(index, board);
    if (d.image == nullptr) {
        d.code = "no_image";
        return d;
    }
    if (d.image->secure_version < facts.running_secure_version) {
        d.image = nullptr;
        d.code = "older_security";
        return d;
    }
    d.verdict = Verdict::kAvailable;
    d.skipped = !facts.skipped_version.empty() && facts.skipped_version == index.version;
    return d;
}

// --- URLs
// -------------------------------------------------------------------------------------------

inline constexpr std::string_view kChannelsReleaseTag = "firmware-channels";

// `base` is ICLFORGE_FIRMWARE_UPDATE_BASE_URL,
// "https://github.com/<owner>/<repo>/releases/download".
[[nodiscard]] inline std::string index_url(std::string_view base, std::string_view channel,
                                           bool signature) {
    std::string url(base);
    url += '/';
    url += kChannelsReleaseTag;
    url += "/hearth-update-";
    url += channel;
    url += signature ? ".sig.json" : ".json";
    return url;
}

// The URL of an image of the release `tag`, or nothing when the tag or the file is not one to
// fetch.
[[nodiscard]] inline std::optional<std::string> image_url(std::string_view base,
                                                          std::string_view tag,
                                                          std::string_view file) {
    if (!impl::is_release_file_name(tag) || !impl::is_release_file_name(file)) {
        return std::nullopt;
    }
    std::string url(base);
    url += '/';
    url += tag;
    url += '/';
    url += file;
    return url;
}

// The release's page, for a person: the repository's address with /releases/tag/<tag>.
[[nodiscard]] inline std::string notes_url(std::string_view base, std::string_view tag) {
    constexpr std::string_view kSuffix = "/releases/download";
    if (base.size() <= kSuffix.size() || base.substr(base.size() - kSuffix.size()) != kSuffix ||
        !impl::is_release_file_name(tag)) {
        return {};
    }
    std::string url(base.substr(0, base.size() - kSuffix.size()));
    url += "/releases/tag/";
    url += tag;
    return url;
}

// --- words
// ------------------------------------------------------------------------------------------

// What a person is told when something fails. One place, so the screen, the page, ota.py and
// ac3hearth say the same.
[[nodiscard]] inline std::string error_text(std::string_view code) {
    struct Entry {
        std::string_view code;
        std::string_view text;
    };
    static constexpr Entry kEntries[] = {
        {"no_network", "The player has no network address yet."},
        {"no_clock", "The player does not know the date yet."},
        {"tls", "Could not make a secure connection to GitHub."},
        {"unreachable", "Could not reach GitHub."},
        {"bad_index", "The update list was not understood."},
        {"bad_sig_file", "The update list's signature file was not understood."},
        {"hash_mismatch",
         "The update list and its signature do not belong together. Try again in a minute."},
        {"untrusted", "The update list is not trusted: nothing was changed."},
        {"leaf_usage", "The update list is not trusted: nothing was changed."},
        {"weak_key", "The update list is not trusted: nothing was changed."},
        {"bad_signature", "The update list is not trusted: nothing was changed."},
        {"leaf_name",
         "The update list was signed with a certificate this player does not accept for this "
         "channel."},
        {"cert_expired", "The update certificate has expired, or this player's date is wrong."},
        {"cert_not_yet_valid",
         "The update certificate is not valid yet, or this player's date is wrong."},
        {"replayed", "This update list is older than one the player has already seen."},
        {"no_image", "This version has no image for this board."},
        {"no_memory", "Stop what is playing and try again."},
        {"bad_image", "The download is not an image this player can run: nothing was changed."},
        {"bad_hash", "The download did not match its checksum: nothing was changed."},
        {"unknown_version",
         "This player's version is not one it can compare with a release. Install a release once "
         "from a computer."},
        {"older_security",
         "This release is below the security level of the version running, so it was not offered."},
        {"write_failed", "Writing the new firmware failed: nothing was changed."},
        {"no_slot", "This board takes no updates over the network."},
        {"on_trial",
         "This version is still being tried; wait for it to be accepted, or go back to the "
         "previous "
         "one."},
        {"busy", "An update is already running."},
        {"nothing_offered", "No update is waiting: check for updates first."},
        {"offer_stale", "That check is too old: check for updates again."},
        {"not_supported", "This player cannot update itself from GitHub."},
    };
    for (const Entry& entry : kEntries) {
        if (entry.code == code) {
            return std::string(entry.text);
        }
    }
    return "Something went wrong (" + std::string(code) + ").";
}

// Why a board cannot ever do this, for good: its build and hardware (BoardCaps for the registry).
// Empty when it can.
struct SupportFacts {
    bool pull_built = false;  // ICLFORGE_FIRMWARE_PULL: the HTTPS client, the bundle and the routes
    int trust_anchors = 0;    // certificates in the build's anchor file
    bool has_network = false;
    bool has_clock = false;  // the board can keep the time of day
    bool has_second_slot = false;
};

[[nodiscard]] inline std::string unsupported_reason(const SupportFacts& facts) {
    if (!facts.has_network) {
        return "This board has no network.";
    }
    if (!facts.has_second_slot) {
        return "This board has one app slot; an update over the network needs a USB flash first.";
    }
    if (!facts.pull_built) {
        return "This firmware has no room for a secure connection.";
    }
    if (facts.trust_anchors <= 0) {
        return "This firmware carries no update key.";
    }
    if (!facts.has_clock) {
        return "This board keeps no time, and a secure connection needs the date.";
    }
    return {};
}

// Why a check or an install cannot start right now. Empty when it can.
struct BlockFacts {
    bool network_up = false;
    bool clock_set = false;
    bool time_sync_on = true;
    bool on_trial = false;
    std::uint32_t trial_minutes_left = 0;
    bool flash_mode = false;
    bool update_running = false;
    bool room_beside_stream = true;  // a TLS session fits beside what is playing
};

[[nodiscard]] inline std::string blocked_reason(const BlockFacts& facts) {
    if (facts.update_running) {
        return "An update is already running.";
    }
    if (facts.flash_mode) {
        return "The player is waiting for a firmware upload.";
    }
    if (facts.on_trial) {
        const std::uint32_t minutes = facts.trial_minutes_left < 1U ? 1U : facts.trial_minutes_left;
        return "This version is still being tried; wait " + std::to_string(minutes) +
               (minutes == 1U ? " minute" : " minutes") + ", or go back to the previous one.";
    }
    if (!facts.network_up) {
        return "The player has no network address yet.";
    }
    if (!facts.time_sync_on) {
        return "Time sync is off.";
    }
    if (!facts.clock_set) {
        return "The player does not know the date yet.";
    }
    if (!facts.room_beside_stream) {
        return "Stop what is playing and try again.";
    }
    return {};
}

// --- the state the routes and the screen show
// -------------------------------------------------------

enum class State : std::uint8_t { kIdle, kChecking, kUpToDate, kAvailable, kInstalling, kFailed };

[[nodiscard]] constexpr const char* state_name(State state) {
    switch (state) {
        case State::kChecking:
            return "checking";
        case State::kUpToDate:
            return "up_to_date";
        case State::kAvailable:
            return "available";
        case State::kInstalling:
            return "installing";
        case State::kFailed:
            return "failed";
        case State::kIdle:
            break;
    }
    return "idle";
}

struct Latest {
    std::string version;
    std::string released;  // YYYY-MM-DD
    std::uint64_t size = 0;
    std::string notes;  // the release's page
    std::string image;  // the image's name
};

struct TrustInfo {
    int anchors = 0;
    std::string source = "build";  // "build", or "board" for an anchor set on the board (later)
    std::vector<std::string> fingerprints;  // short SHA-256 of each anchor
};

struct UpdateStatus {
    bool supported = false;
    std::string why;      // when not supported: for good
    std::string blocked;  // what stops a check or an install now
    State state = State::kIdle;
    std::string channel = "beta";
    std::string running;
    std::optional<std::int64_t> checked;  // unix seconds
    std::optional<Latest> latest;
    TrustInfo trust;
    std::string signed_by;  // the leaf's name, from the last verified index
    std::string error_code;
    std::string error_text;
    std::string skipped;  // a version that went back on this board
    std::string stage;    // while installing: "downloading", "checking" or "restarting"
    int percent = -1;     // while downloading
};

namespace impl {

inline void text_or_null(iclforge::detail::JsonObject& object, std::string_view name,
                         std::string_view value) {
    if (value.empty()) {
        object.null(name);
    } else {
        object.text(name, value);
    }
}

}  // namespace impl

// GET /firmware/available's body: every key always present, null where it does not apply.
[[nodiscard]] inline std::string render_status(const UpdateStatus& status) {
    std::string out;
    {
        iclforge::detail::JsonObject object(out);
        object.key("supported") += status.supported ? "true" : "false";
        impl::text_or_null(object, "why", status.why);
        impl::text_or_null(object, "blocked", status.blocked);
        object.text("state", state_name(status.state));
        object.text("channel", status.channel);
        object.text("running", status.running);
        if (status.checked) {
            object.key("checked") += std::to_string(*status.checked);
        } else {
            object.null("checked");
        }
        if (status.latest) {
            iclforge::detail::JsonObject latest(object.key("latest"));
            latest.text("version", status.latest->version);
            latest.text("released", status.latest->released);
            latest.number("size", status.latest->size);
            impl::text_or_null(latest, "notes", status.latest->notes);
            latest.text("image", status.latest->image);
        } else {
            object.null("latest");
        }
        {
            iclforge::detail::JsonObject trust(object.key("trust"));
            trust.number("anchors", static_cast<std::uint64_t>(
                                        status.trust.anchors < 0 ? 0 : status.trust.anchors));
            trust.text("source", status.trust.source);
            std::string& list = trust.key("fingerprints");
            list += '[';
            for (std::size_t i = 0; i < status.trust.fingerprints.size(); ++i) {
                if (i > 0) {
                    list += ',';
                }
                iclforge::detail::append_json_text(list, status.trust.fingerprints[i]);
            }
            list += ']';
        }
        impl::text_or_null(object, "signed_by", status.signed_by);
        if (!status.error_code.empty()) {
            iclforge::detail::JsonObject error(object.key("error"));
            error.text("code", status.error_code);
            error.text("text", status.error_text);
        } else {
            object.null("error");
        }
        impl::text_or_null(object, "skipped", status.skipped);
    }
    out += '\n';
    return out;
}

namespace impl {

// "2026-11-05" as "5 Nov"; the date as it came when it is not one.
[[nodiscard]] inline std::string short_date(std::string_view iso) {
    static constexpr std::string_view kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (!is_date(iso)) {
        return std::string(iso);
    }
    const int month = (iso[5] - '0') * 10 + (iso[6] - '0');
    const int day = (iso[8] - '0') * 10 + (iso[9] - '0');
    if (month < 1 || month > 12) {
        return std::string(iso);
    }
    return std::to_string(day) + " " + std::string(kMonths[month - 1]);
}

// 1583104 as "1.6 MB".
[[nodiscard]] inline std::string megabytes(std::uint64_t bytes) {
    const std::uint64_t tenths = (bytes + 50'000) / 100'000;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
}

}  // namespace impl

// The line the screen's status row and the page's result line show. `checked_clock` is the time of
// the last check as the board's clock reads it ("14:02"), or empty.
[[nodiscard]] inline std::string status_sentence(const UpdateStatus& status,
                                                 std::string_view checked_clock = {}) {
    if (!status.supported) {
        return status.why;
    }
    const std::string running = "Version " + status.running + ".";
    switch (status.state) {
        case State::kChecking:
            return "Checking for updates...";
        case State::kUpToDate: {
            std::string line = running + " You have the newest version.";
            if (!checked_clock.empty()) {
                line += " Checked " + std::string(checked_clock);
            }
            return line;
        }
        case State::kAvailable: {
            if (!status.latest) {
                return running;
            }
            std::string line = "Version " + status.latest->version + " is available (" +
                               impl::megabytes(status.latest->size) + ", " +
                               impl::short_date(status.latest->released) + ").";
            if (!status.skipped.empty() && status.skipped == status.latest->version) {
                line += " It went back on this player before.";
            }
            return line;
        }
        case State::kInstalling: {
            const std::string what =
                status.latest ? status.latest->version : std::string("the update");
            if (status.stage == "checking") {
                return "Checking the download of " + what + "...";
            }
            if (status.stage == "restarting") {
                return "Restarting into " + what + "...";
            }
            return "Downloading " + what +
                   (status.percent >= 0 ? " " + std::to_string(status.percent) + "%" : "...");
        }
        case State::kFailed:
            return status.error_text.empty() ? error_text(status.error_code) : status.error_text;
        case State::kIdle:
            break;
    }
    return running + " Channel: " + status.channel + ".";
}

}  // namespace iclforge::update
