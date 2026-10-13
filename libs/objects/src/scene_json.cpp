#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// nlohmann/json 3.12 can keep where each parsed value starts, which is what lets an error name its
// line. It is switched on here and only here: it changes the type's ABI tag
// (nlohmann::json_abi_v3_12_0_dp), so these values cannot meet the plain ones another translation
// unit - the project file's, apps/shared/project - uses, and the library's headers name no JSON type.
#define JSON_DIAGNOSTIC_POSITIONS 1
#include <nlohmann/json.hpp>

#include "iclforge/objects/oamd.hpp"
#include "iclforge/objects/scene.hpp"

// The serialised form of an ObjectScene. RFC 8259 JSON, tokenised by nlohmann/json (cmake/
// NlohmannJson.cmake: header-only, compiled into this library, nothing in the export graph) and
// walked here against the schema.
//
// The reader is strict about what it does not recognise: an unknown member is
// an error, not something to ignore. A hand-authored scene file's likeliest
// fault by far is a misspelled key, and silently defaulting "gian" to 1.0 would
// produce a scene that is wrong in a way nothing reports. Forward compatibility
// rides on the "iclforge_scene" version number instead, which is what it is for.
// A key an object names twice is refused for the same reason: a document keeps
// one of them and says nothing, so which one is in force would be the reader's
// choice rather than the file's.

namespace iclforge::objects::oba {

namespace {

// Insertion-ordered, so that members are visited in the order the file wrote them and the first
// problem reported is the first one in the file.
using Json = nlohmann::ordered_json;

constexpr int kFormatVersion = 1;
constexpr double kPi = 3.141592653589793238462643383279502884;

struct BedLabel {
    std::string_view name;
    std::uint16_t mask;
};

// TS 103 420 Table 12's channel labels, in the array order that clause uses
// (index 9 downwards), spelled for a JSON file rather than for C++.
constexpr std::array<BedLabel, 10> kBedLabels{{
    {"lr", bed::kLR},
    {"c", bed::kC},
    {"lfe", bed::kLfe},
    {"ls_rs", bed::kLsRs},
    {"lb_rb", bed::kLbRb},
    {"tfl_tfr", bed::kTflTfr},
    {"tsl_tsr", bed::kTslTsr},
    {"tbl_tbr", bed::kTblTbr},
    {"lw_rw", bed::kLwRw},
    {"lfe2", bed::kLfe2},
}};

constexpr std::string_view name_of(Interpolation interp) {
    switch (interp) {
        case Interpolation::kHold:
            return "hold";
        case Interpolation::kSmooth:
            return "smooth";
        case Interpolation::kLinear:
            break;
    }
    return "linear";
}

// --- Writing --------------------------------------------------------------

// A name as a JSON string: the two characters with a mandatory backslash, every C0 control
// (\b \f \n \r \t, the rest as \u00xx) and everything at or above 0x20 as it is - UTF-8 included,
// which is what keeps a non-ASCII object name readable in the file. Bytes that are not UTF-8 become
// U+FFFD rather than making the whole scene unwritable.
void write_string(std::string& out, std::string_view value) {
    out += Json(std::string(value)).dump(-1, ' ', false, Json::error_handler_t::replace);
}

}  // namespace

std::string to_json(const ObjectScene& scene) {
    std::string out = fmt::format("{{\n  \"iclforge_scene\": {},\n", kFormatVersion);
    const auto& orientation = scene.orientation();
    out += fmt::format("  \"orientation\": {{ \"yaw_rad\": {}, \"pitch_rad\": {}, "
                       "\"roll_rad\": {} }},\n",
                       orientation.yaw_rad, orientation.pitch_rad, orientation.roll_rad);
    out += "  \"objects\": [\n";
    const auto objects = scene.objects();
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto& object = objects[i];
        out += "    {\n      \"name\": ";
        write_string(out, object.name);
        out += ",\n      \"bed\": [";
        bool first = true;
        for (const auto& label : kBedLabels) {
            if ((object.bed & label.mask) == 0) {
                continue;
            }
            if (!first) {
                out += ", ";
            }
            first = false;
            write_string(out, label.name);
        }
        out += "],\n      \"automation\": [\n";
        for (std::size_t k = 0; k < object.automation.size(); ++k) {
            const auto& point = object.automation[k];
            // One point per line: a scene under version control should show
            // the cue that moved, not a reflowed block.
            out += fmt::format(
                "        {{ \"t\": {}, \"x\": {}, \"y\": {}, \"z\": {}, \"gain\": {}, "
                "\"lfe\": {}, \"interp\": \"{}\" }}{}\n",
                point.time_s, point.position.x, point.position.y, point.position.z, point.gain,
                point.lfe_send, name_of(point.interp), k + 1 == object.automation.size() ? "" : ",");
        }
        out += fmt::format("      ]\n    }}{}\n", i + 1 == objects.size() ? "" : ",");
    }
    out += "  ]\n}\n";
    return out;
}

// --- Reading --------------------------------------------------------------

namespace {

// What a parsed document cannot say, learnt by feeding the same text through nlohmann's SAX
// interface (which sees every token the DOM builder does): where a syntax error is and what the
// parser called it, and whether one object names a key twice - the DOM keeps the last and says
// nothing. It builds nothing; it is run once per read, after the document.
class Probe final : public Json::json_sax_t {
   public:
    // The first object that names a key twice: the pointer to it ("/objects/2") and the key.
    [[nodiscard]] bool has_duplicate() const { return duplicate_; }
    [[nodiscard]] const std::string& duplicate_object() const { return duplicate_object_; }
    [[nodiscard]] const std::string& duplicate_key() const { return duplicate_key_; }
    // The parse error, when there was one: where (the offending character's 0-based index) and
    // nlohmann's description of it.
    [[nodiscard]] bool has_syntax_error() const { return syntax_error_; }
    [[nodiscard]] std::size_t error_offset() const { return error_offset_; }
    [[nodiscard]] const std::string& error_text() const { return error_text_; }

    bool null() override { return value(); }
    bool boolean(bool) override { return value(); }
    bool number_integer(number_integer_t) override { return value(); }
    bool number_unsigned(number_unsigned_t) override { return value(); }
    bool number_float(number_float_t, const string_t&) override { return value(); }
    bool string(string_t&) override { return value(); }
    bool binary(binary_t&) override { return value(); }

    bool start_object(std::size_t) override {
        value();
        frames_.push_back({.is_object = true});
        return true;
    }
    bool key(string_t& name) override {
        Frame& frame = frames_.back();
        if (std::ranges::find(frame.keys, name) != frame.keys.end()) {
            duplicate_ = true;
            duplicate_object_ = pointer_to_top();
            duplicate_key_ = name;
            return false;
        }
        frame.keys.push_back(name);
        return true;
    }
    bool end_object() override {
        frames_.pop_back();
        return true;
    }
    bool start_array(std::size_t) override {
        value();
        frames_.push_back({.is_object = false});
        return true;
    }
    bool end_array() override {
        frames_.pop_back();
        return true;
    }

    bool parse_error(std::size_t position, const std::string&,
                     const nlohmann::detail::exception& ex) override {
        syntax_error_ = true;
        // `position` counts the characters read, the offending one included.
        error_offset_ = position > 0 ? position - 1 : 0;
        error_text_ = ex.what();
        // "[json.exception.parse_error.101] parse error at line 1, column 20: syntax error ..."
        // - the exception's name is no help to someone with a scene file, and the line is
        // reported separately, so keep what follows the position.
        if (error_text_.starts_with('[')) {
            if (const auto close = error_text_.find("] "); close != std::string::npos) {
                error_text_.erase(0, close + 2);
            }
        }
        if (const auto column = error_text_.find("column "); column != std::string::npos) {
            if (const auto colon = error_text_.find(": ", column); colon != std::string::npos) {
                error_text_.erase(0, colon + 2);
            }
        }
        return false;
    }

   private:
    struct Frame {
        bool is_object = false;
        std::vector<std::string> keys{};  // an object's members so far, in order
        std::size_t elements = 0;         // an array's values so far
    };

    // A value is starting: if it sits in an array, that array has one more element.
    bool value() {
        if (!frames_.empty() && !frames_.back().is_object) {
            ++frames_.back().elements;
        }
        return true;
    }

    // RFC 6901 pointer from the root to the innermost open object.
    [[nodiscard]] std::string pointer_to_top() const {
        std::string out;
        for (std::size_t i = 0; i + 1 < frames_.size(); ++i) {
            const Frame& frame = frames_[i];
            out += '/';
            if (frame.is_object) {
                for (const char c : frame.keys.back()) {
                    out += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
                }
            } else {
                out += std::to_string(frame.elements - 1);
            }
        }
        return out;
    }

    std::vector<Frame> frames_;
    bool duplicate_ = false;
    std::string duplicate_object_;
    std::string duplicate_key_;
    bool syntax_error_ = false;
    std::size_t error_offset_ = 0;
    std::string error_text_;
};

// Walks a parsed document against the schema, in the order the file wrote it. Every member this
// format knows is consumed where it is expected, and anything else is a diagnosed error at the
// value that is wrong. There is deliberately no generic value-skipper - see this file's header
// comment on why an unrecognised member is refused rather than stepped over.
class Reader {
   public:
    explicit Reader(std::string_view text) : text_(text) {}

    [[nodiscard]] const SceneError& error() const { return error_; }

    // Records the first failure and returns false; every reader below propagates that false
    // straight out, so the message a caller sees is the first problem in the file.
    bool fail(SceneErrorKind kind, std::size_t offset, std::string message) {
        if (!failed_) {
            failed_ = true;
            error_ = {.kind = kind, .line = line_at(offset), .message = std::move(message)};
        }
        return false;
    }
    bool fail(SceneErrorKind kind, const Json& at, std::string message) {
        return fail(kind, at.start_pos(), std::move(message));
    }

    bool read_string(const Json& v, std::string& out) {
        if (!v.is_string()) {
            return fail(SceneErrorKind::kSyntax, v, "expected a string");
        }
        out = v.get_ref<const std::string&>();
        return true;
    }

    bool read_number(const Json& v, double& out) {
        if (!v.is_number()) {
            return fail(SceneErrorKind::kSyntax, v, "expected a number");
        }
        out = v.get<double>();
        // A number too large for a double reads as infinity; nothing in this format is one.
        if (!std::isfinite(out)) {
            return fail(SceneErrorKind::kBadValue, v, "numbers must be finite");
        }
        return true;
    }

    bool expect_array(const Json& v) {
        return v.is_array() || fail(SceneErrorKind::kSyntax, v, "expected an array");
    }

    bool expect_object(const Json& v) {
        return v.is_object() || fail(SceneErrorKind::kSyntax, v, "expected an object");
    }

   private:
    [[nodiscard]] std::size_t line_at(std::size_t offset) const {
        return 1 + static_cast<std::size_t>(
                       std::ranges::count(text_.substr(0, std::min(offset, text_.size())), '\n'));
    }

    std::string_view text_;
    bool failed_ = false;
    SceneError error_{};
};

bool read_interpolation(Reader& reader, const Json& v, Interpolation& out) {
    std::string name;
    if (!reader.read_string(v, name)) {
        return false;
    }
    if (name == "hold") {
        out = Interpolation::kHold;
    } else if (name == "linear") {
        out = Interpolation::kLinear;
    } else if (name == "smooth") {
        out = Interpolation::kSmooth;
    } else {
        return reader.fail(SceneErrorKind::kBadValue, v,
                           fmt::format("unknown interpolation '{}' (hold, linear or smooth)",
                                       name));
    }
    return true;
}

bool read_bed(Reader& reader, const Json& v, std::uint16_t& out) {
    if (!reader.expect_array(v)) {
        return false;
    }
    out = 0;
    for (const Json& element : v) {
        std::string name;
        if (!reader.read_string(element, name)) {
            return false;
        }
        const auto label = std::ranges::find(kBedLabels, name, &BedLabel::name);
        if (label == kBedLabels.end()) {
            return reader.fail(SceneErrorKind::kBadValue, element,
                               fmt::format("unknown bed channel label '{}'", name));
        }
        out |= label->mask;
    }
    return true;
}

bool read_point(Reader& reader, const Json& v, AutomationPoint& out) {
    if (!reader.expect_object(v)) {
        return false;
    }
    if (v.empty()) {
        return reader.fail(SceneErrorKind::kBadField, v,
                           "an automation point needs at least 't', 'x', 'y' and 'z'");
    }
    bool have_t = false;
    bool have_x = false;
    bool have_y = false;
    bool have_z = false;
    for (auto it = v.begin(); it != v.end(); ++it) {
        const std::string& key = it.key();
        const Json& member = *it;
        bool ok = true;
        if (key == "t") {
            ok = reader.read_number(member, out.time_s);
            have_t = true;
        } else if (key == "x") {
            ok = reader.read_number(member, out.position.x);
            have_x = true;
        } else if (key == "y") {
            ok = reader.read_number(member, out.position.y);
            have_y = true;
        } else if (key == "z") {
            ok = reader.read_number(member, out.position.z);
            have_z = true;
        } else if (key == "gain") {
            ok = reader.read_number(member, out.gain);
        } else if (key == "lfe") {
            ok = reader.read_number(member, out.lfe_send);
        } else if (key == "interp") {
            ok = read_interpolation(reader, member, out.interp);
        } else {
            return reader.fail(SceneErrorKind::kBadField, member,
                               fmt::format("unknown automation member '{}'", key));
        }
        if (!ok) {
            return false;
        }
    }
    if (!have_t || !have_x || !have_y || !have_z) {
        // Defaulting a missing coordinate would silently move the object to
        // room centre, which is exactly the kind of wrong-but-plausible a
        // hand-authored file must not be allowed to be.
        return reader.fail(SceneErrorKind::kBadField, v,
                           "an automation point needs 't', 'x', 'y' and 'z'");
    }
    return true;
}

bool read_object(Reader& reader, const Json& v, SceneObject& out) {
    if (!reader.expect_object(v)) {
        return false;
    }
    if (v.empty()) {
        return reader.fail(SceneErrorKind::kBadField, v, "an object needs an 'automation' array");
    }
    bool have_automation = false;
    for (auto it = v.begin(); it != v.end(); ++it) {
        const std::string& key = it.key();
        const Json& member = *it;
        if (key == "name") {
            if (!reader.read_string(member, out.name)) {
                return false;
            }
        } else if (key == "bed") {
            if (!read_bed(reader, member, out.bed)) {
                return false;
            }
        } else if (key == "automation") {
            have_automation = true;
            if (!reader.expect_array(member)) {
                return false;
            }
            for (const Json& element : member) {
                AutomationPoint point;
                if (!read_point(reader, element, point)) {
                    return false;
                }
                out.automation.push_back(point);
            }
        } else {
            return reader.fail(SceneErrorKind::kBadField, member,
                               fmt::format("unknown object member '{}'", key));
        }
    }
    if (!have_automation) {
        return reader.fail(SceneErrorKind::kBadField, v, "an object needs an 'automation' array");
    }
    return true;
}

// yaw/pitch/roll, each accepted as either radians or degrees but never both -
// a file that gave both would leave which one is in force to whichever the
// reader happened to see last, and that is not a thing to guess at.
bool read_angle(Reader& reader, const Json& v, std::string_view key, double& out, bool& seen) {
    if (seen) {
        return reader.fail(SceneErrorKind::kBadField, v,
                           fmt::format("orientation gives '{}' twice, or in both units", key));
    }
    seen = true;
    if (!reader.read_number(v, out)) {
        return false;
    }
    if (key.ends_with("_deg")) {
        out *= kPi / 180.0;
    }
    return true;
}

bool read_orientation(Reader& reader, const Json& v, Orientation& out) {
    if (!reader.expect_object(v)) {
        return false;
    }
    bool yaw = false;
    bool pitch = false;
    bool roll = false;
    for (auto it = v.begin(); it != v.end(); ++it) {
        const std::string& key = it.key();
        const Json& member = *it;
        bool ok = true;
        if (key == "yaw_rad" || key == "yaw_deg") {
            ok = read_angle(reader, member, key, out.yaw_rad, yaw);
        } else if (key == "pitch_rad" || key == "pitch_deg") {
            ok = read_angle(reader, member, key, out.pitch_rad, pitch);
        } else if (key == "roll_rad" || key == "roll_deg") {
            ok = read_angle(reader, member, key, out.roll_rad, roll);
        } else {
            return reader.fail(SceneErrorKind::kBadField, member,
                               fmt::format("unknown orientation member '{}'", key));
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::expected<ObjectScene, SceneError> scene_from_json(std::string_view text) {
    auto contents = read_scene_json(text);
    if (!contents) {
        return std::unexpected(std::move(contents.error()));
    }
    return ObjectScene::create(std::move(contents->objects), contents->orientation);
}

std::expected<SceneContents, SceneError> read_scene_json(std::string_view text) {
    Reader reader{text};

    // The document first, then the probe over the same text: a syntax error or a repeated key is
    // reported before any schema problem, as a reader that took the file in one pass would.
    const Json document = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    Probe probe;
    Json::sax_parse(text.begin(), text.end(), &probe);
    if (probe.has_syntax_error() || document.is_discarded()) {
        reader.fail(SceneErrorKind::kSyntax, probe.error_offset(),
                    probe.has_syntax_error() ? probe.error_text() : "not valid JSON");
        return std::unexpected(reader.error());
    }
    if (probe.has_duplicate()) {
        // The document kept the later of the two, which is where the repeat is.
        const Json::json_pointer where{probe.duplicate_object()};
        std::size_t offset = 0;
        if (document.contains(where)) {
            const Json& object = document.at(where);
            if (const auto it = object.find(probe.duplicate_key()); it != object.end()) {
                offset = it->start_pos();
            }
        }
        reader.fail(SceneErrorKind::kBadField, offset,
                    fmt::format("member '{}' appears twice in one object", probe.duplicate_key()));
        return std::unexpected(reader.error());
    }

    if (!document.is_object()) {
        reader.fail(SceneErrorKind::kSyntax, document, "a scene file is a JSON object");
        return std::unexpected(reader.error());
    }

    std::vector<SceneObject> objects;
    Orientation orientation{};
    bool have_version = false;
    bool have_objects = false;

    for (auto it = document.begin(); it != document.end(); ++it) {
        const std::string& key = it.key();
        const Json& member = *it;
        if (key == "iclforge_scene") {
            double version = 0.0;
            if (!reader.read_number(member, version)) {
                return std::unexpected(reader.error());
            }
            if (version != kFormatVersion) {
                reader.fail(SceneErrorKind::kBadValue, member,
                            fmt::format("this is an iclforge scene version {}; this build "
                                        "reads version {}",
                                        version, kFormatVersion));
                return std::unexpected(reader.error());
            }
            have_version = true;
        } else if (key == "orientation") {
            if (!read_orientation(reader, member, orientation)) {
                return std::unexpected(reader.error());
            }
        } else if (key == "objects") {
            have_objects = true;
            if (!reader.expect_array(member)) {
                return std::unexpected(reader.error());
            }
            for (const Json& element : member) {
                SceneObject object;
                if (!read_object(reader, element, object)) {
                    return std::unexpected(reader.error());
                }
                objects.push_back(std::move(object));
            }
        } else {
            reader.fail(SceneErrorKind::kBadField, member,
                        fmt::format("unknown scene member '{}'", key));
            return std::unexpected(reader.error());
        }
    }
    if (!have_version) {
        reader.fail(SceneErrorKind::kBadField, document,
                    "not an iclforge scene: no 'iclforge_scene' version member");
        return std::unexpected(reader.error());
    }
    if (!have_objects) {
        reader.fail(SceneErrorKind::kBadField, document, "a scene needs an 'objects' array");
        return std::unexpected(reader.error());
    }
    return SceneContents{.objects = std::move(objects), .orientation = orientation};
}

}  // namespace iclforge::objects::oba
