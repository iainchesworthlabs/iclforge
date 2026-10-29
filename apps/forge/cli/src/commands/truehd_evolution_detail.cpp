#include "truehd_evolution_detail.hpp"

#include <fmt/base.h>
#include <fmt/format.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/thrift_binary.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/evolution.hpp"
#include "json_sink.hpp"

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;
namespace thr = iclforge::base::crypto::thrift;
using Node = EvolutionDetail::ThriftNode;
using Outline = EvolutionDetail::ThriftOutline;

constexpr std::size_t kMaxFrames = 8;
constexpr std::size_t kMaxShapes = 16;
constexpr std::size_t kMaxOffset = 16;
constexpr std::size_t kMaxNodes = 400;
constexpr int kMaxWalkDepth = 8;

std::string type_name(std::uint8_t type) {
    switch (type) {
        case 2:
            return "bool";
        case 3:
            return "byte";
        case 4:
            return "double";
        case 6:
            return "i16";
        case 8:
            return "i32";
        case 10:
            return "i64";
        case 11:
            return "string";
        case 12:
            return "struct";
        case 13:
            return "map";
        case 14:
            return "set";
        case 15:
            return "list";
        default:
            return fmt::format("type{}", type);
    }
}

std::uint64_t be32(std::span<const std::byte> bytes, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[at + i]);
    }
    return value;
}

// The shape of a struct: field ids, types and lengths. A field's value is never read except for
// the length or count that its encoding states.
void walk_fields(std::span<const thr::Field> fields, int depth, std::size_t& budget, bool& capped,
                 std::vector<Node>& out) {
    for (const thr::Field& field : fields) {
        if (budget == 0) {
            capped = true;
            return;
        }
        --budget;
        Node node;
        node.id = field.id;
        node.type = type_name(static_cast<std::uint8_t>(field.type));
        const std::span<const std::byte> value = field.value;
        switch (field.type) {
            case thr::Type::kString:
                if (value.size() >= 4) {
                    node.has_count = true;
                    node.count = be32(value, 0);
                }
                break;
            case thr::Type::kList:
            case thr::Type::kSet:
                if (value.size() >= 5) {
                    node.element = type_name(std::to_integer<std::uint8_t>(value[0]));
                    node.has_count = true;
                    node.count = be32(value, 1);
                    if (field.type == thr::Type::kList &&
                        value[0] == static_cast<std::byte>(thr::Type::kStruct) &&
                        depth < kMaxWalkDepth) {
                        const auto structs = thr::as_struct_list(field);
                        if (structs && !structs->empty()) {
                            if (const auto inner = thr::read_struct((*structs)[0])) {
                                walk_fields(*inner, depth + 1, budget, capped, node.children);
                            }
                        }
                    }
                }
                break;
            case thr::Type::kMap:
                if (value.size() >= 6) {
                    node.key = type_name(std::to_integer<std::uint8_t>(value[0]));
                    node.element = type_name(std::to_integer<std::uint8_t>(value[1]));
                    node.has_count = true;
                    node.count = be32(value, 2);
                }
                break;
            case thr::Type::kStruct:
                if (depth < kMaxWalkDepth) {
                    if (const auto inner = thr::as_struct(field)) {
                        walk_fields(*inner, depth + 1, budget, capped, node.children);
                    }
                }
                break;
            default:
                break;
        }
        out.push_back(std::move(node));
    }
}

// Where a whole Thrift struct reads in `bytes`: the first offset from 0 to kMaxOffset.
Outline outline_of(std::span<const std::byte> bytes) {
    Outline outline;
    outline.payload_bytes = bytes.size();
    for (std::size_t offset = 0; offset <= std::min(kMaxOffset, bytes.size()); ++offset) {
        std::size_t consumed = 0;
        const auto fields = thr::read_struct(bytes.subspan(offset), &consumed);
        if (!fields || fields->empty()) {
            continue;
        }
        outline.found = true;
        outline.offset = offset;
        outline.consumed = consumed;
        std::size_t budget = kMaxNodes;
        walk_fields(*fields, 0, budget, outline.capped, outline.fields);
        break;
    }
    return outline;
}

void write_node(iclforge::apps::JsonSink& json, const Node& node) {
    json.begin_object();
    json.member("id", node.id);
    json.member("type", node.type);
    if (!node.key.empty()) {
        json.member("key", node.key);
    }
    if (!node.element.empty()) {
        json.member("element", node.element);
    }
    if (node.has_count) {
        json.member("count", node.count);
    }
    if (!node.children.empty()) {
        json.key("fields");
        json.begin_array();
        for (const Node& child : node.children) {
            write_node(json, child);
        }
        json.end_array();
    }
    json.end_object();
}

void write_outline(iclforge::apps::JsonSink& json, const Outline& outline) {
    json.begin_object();
    json.member("found", outline.found);
    json.member("payload_bytes", static_cast<std::uint64_t>(outline.payload_bytes));
    if (outline.found) {
        json.member("offset", static_cast<std::uint64_t>(outline.offset));
        json.member("consumed", static_cast<std::uint64_t>(outline.consumed));
        json.member("capped", outline.capped);
        json.key("fields");
        json.begin_array();
        for (const Node& node : outline.fields) {
            write_node(json, node);
        }
        json.end_array();
    }
    json.end_object();
}

void write_optional(iclforge::apps::JsonSink& json, std::string_view name,
                    const std::optional<std::uint64_t>& value) {
    if (value) {
        json.member(name, *value);
    } else {
        json.member_null(name);
    }
}

void write_config(iclforge::apps::JsonSink& json, const th::EvolutionPayloadConfig& config) {
    json.begin_object();
    write_optional(json, "sample_offset", config.sample_offset);
    write_optional(json, "duration", config.duration);
    write_optional(json, "group_id", config.group_id);
    if (config.codec_data) {
        json.member("codec_data_present", true);
    } else {
        json.member("codec_data_present", false);
    }
    json.member("discard_unknown", config.discard_unknown);
    json.member("frame_aligned", config.frame_aligned);
    json.member("create_duplicate", config.create_duplicate);
    json.member("remove_duplicate", config.remove_duplicate);
    json.member("priority", static_cast<std::uint64_t>(config.priority));
    json.member("proc_allowed", static_cast<std::uint64_t>(config.proc_allowed));
    json.end_object();
}

std::string config_text(const th::EvolutionPayloadConfig& config) {
    std::string text;
    const auto add = [&text](const std::string& part) {
        text += text.empty() ? part : ", " + part;
    };
    if (config.sample_offset) {
        add(fmt::format("sample offset {}", *config.sample_offset));
    }
    if (config.duration) {
        add(fmt::format("duration {}", *config.duration));
    }
    if (config.group_id) {
        add(fmt::format("group id {}", *config.group_id));
    }
    if (config.codec_data) {
        add("codec data byte");
    }
    if (config.discard_unknown) {
        add("discard unknown");
    }
    if (config.frame_aligned) {
        add("frame aligned");
    }
    if (config.create_duplicate) {
        add("create duplicate");
    }
    if (config.remove_duplicate) {
        add("remove duplicate");
    }
    if (config.priority != 0 || config.proc_allowed != 0) {
        add(fmt::format("priority {}, proc allowed {}", config.priority, config.proc_allowed));
    }
    return text.empty() ? "no flags" : text;
}

void print_nodes(const std::vector<Node>& nodes, int indent) {
    for (const Node& node : nodes) {
        std::string text =
            fmt::format("{:<16}{:{}}field {} {}", "", "", indent, node.id, node.type);
        if (!node.key.empty()) {
            text += fmt::format(" key {}", node.key);
        }
        if (!node.element.empty()) {
            text += fmt::format(" of {}", node.element);
        }
        if (node.has_count) {
            text += fmt::format(" x{}", node.count);
        }
        fmt::println("{}", text);
        print_nodes(node.children, indent + 2);
    }
}

void print_payload(const EvolutionDetail::PayloadShape& payload, int indent) {
    fmt::println("{:<16}{:{}}payload {}: {} byte(s), {}", "", "", indent, payload.id, payload.bytes,
                 config_text(payload.config));
}

void print_outline(const Outline& outline, int indent) {
    if (!outline.found) {
        fmt::println("{:<16}{:{}}no Thrift struct reads at offsets 0 to {}", "", "", indent,
                     kMaxOffset);
        return;
    }
    fmt::println("{:<16}{:{}}a Thrift struct reads at offset {} and takes {} of {} byte(s){}", "",
                 "", indent, outline.offset, outline.consumed, outline.payload_bytes,
                 outline.capped ? " (outline capped)" : "");
    print_nodes(outline.fields, indent + 2);
}

}  // namespace

void EvolutionDetail::add(std::uint64_t unit_index, const th::ExtraData& extra,
                          const th::EvolutionRead& emdf, const th::EvolutionRead& letter,
                          const th::EvolutionFrame* frame, const char* reading) {
    ++frames_total_;
    FrameRecord record;
    record.unit = unit_index;
    record.extra_data_length = extra.length;
    record.evo_frame_byte_length = extra.evo_frame_byte_length;
    record.check_ok = emdf.check_ok;
    record.framing_ok = emdf.framing_ok;
    record.parity_ok = emdf.parity_ok;
    record.reserved_zero = emdf.reserved_zero;
    record.length_fits = emdf.length_fits;
    record.reading = frame != nullptr ? reading : "none";
    if (!emdf.container) {
        record.emdf_error = std::string(th::to_string(emdf.container.error().kind));
    }
    if (!letter.container) {
        record.letter_error = std::string(th::to_string(letter.container.error().kind));
    }
    std::vector<PayloadShape> payloads;
    if (frame != nullptr) {
        record.version = frame->version;
        record.key_id = frame->key_id;
        record.primary_code = frame->protection.primary_code;
        record.secondary_code = frame->protection.secondary_code;
        record.bits_used = frame->bits_used;
        record.trailing_zero = frame->trailing_zero;
        for (const th::EvolutionPayload& payload : frame->payloads) {
            PayloadShape shape;
            shape.id = payload.id;
            shape.bytes = payload.bytes.size();
            shape.config = payload.config;
            record.payloads.push_back(shape);
            payloads.push_back(std::move(shape));
        }
    }

    // The shape: what a signature per frame or per stream would show in.
    std::vector<std::uint64_t> ids;
    for (const PayloadShape& payload : payloads) {
        ids.push_back(payload.id);
    }
    const std::string unread =
        frame != nullptr ? std::string()
                         : "emdf: " + record.emdf_error + ", spec36: " + record.letter_error;
    Shape* shape = nullptr;
    for (Shape& candidate : shapes_) {
        if (candidate.reading == record.reading && candidate.unread == unread &&
            candidate.ids == ids && candidate.primary_code == record.primary_code &&
            candidate.secondary_code == record.secondary_code) {
            shape = &candidate;
            break;
        }
    }
    if (shape == nullptr && shapes_.size() < kMaxShapes) {
        Shape created;
        created.reading = record.reading;
        created.unread = unread;
        created.ids = ids;
        created.primary_code = record.primary_code;
        created.secondary_code = record.secondary_code;
        created.first_unit = unit_index;
        created.example = payloads;
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            created.size_min.push_back(payloads[i].bytes);
            created.size_max.push_back(payloads[i].bytes);
            if (payloads[i].id != th::kEvolutionPayloadOamd) {
                created.example[i].thrift = outline_of(frame->payloads[i].bytes);
            }
        }
        shapes_.push_back(std::move(created));
        shape = &shapes_.back();
    } else if (shape != nullptr) {
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            shape->size_min[i] = std::min(shape->size_min[i], payloads[i].bytes);
            shape->size_max[i] = std::max(shape->size_max[i], payloads[i].bytes);
        }
    }
    if (shape != nullptr) {
        ++shape->frames;
        shape->last_unit = unit_index;
    } else {
        ++other_shape_frames_;
    }
    if (frames_.size() < kMaxFrames) {
        frames_.push_back(std::move(record));
    }
}

void EvolutionDetail::write_json(iclforge::apps::JsonSink& json) const {
    json.begin_object();
    json.member("frames_total", frames_total_);
    json.member("frames_listed", static_cast<std::uint64_t>(frames_.size()));
    json.member("walk_offsets_tried", static_cast<std::uint64_t>(kMaxOffset + 1));
    json.key("frames");
    json.begin_array();
    for (const FrameRecord& frame : frames_) {
        json.begin_object();
        json.member("unit", frame.unit);
        json.member("extra_data_length", frame.extra_data_length);
        json.member("evo_frame_byte_length", frame.evo_frame_byte_length);
        json.member("check_nibble_ok", frame.check_ok);
        json.member("framing_ok", frame.framing_ok);
        json.member("parity_ok", frame.parity_ok);
        json.member("evo_reserved_zero", frame.reserved_zero);
        json.member("length_fits", frame.length_fits);
        json.member("reading", frame.reading);
        if (!frame.emdf_error.empty()) {
            json.member("emdf_error", frame.emdf_error);
        }
        if (!frame.letter_error.empty()) {
            json.member("spec36_error", frame.letter_error);
        }
        if (frame.reading != "none") {
            json.member("version", frame.version);
            json.member("key_id", frame.key_id);
            json.member("primary_protection_code", frame.primary_code);
            json.member("secondary_protection_code", frame.secondary_code);
            json.member("bits_used", static_cast<std::uint64_t>(frame.bits_used));
            json.member("trailing_zero", frame.trailing_zero);
            json.key("payloads");
            json.begin_array();
            for (const PayloadShape& payload : frame.payloads) {
                json.begin_object();
                json.member("id", payload.id);
                json.member("bytes", static_cast<std::uint64_t>(payload.bytes));
                json.key("config");
                write_config(json, payload.config);
                json.end_object();
            }
            json.end_array();
        }
        json.end_object();
    }
    json.end_array();
    json.key("shapes");
    json.begin_array();
    for (const Shape& shape : shapes_) {
        json.begin_object();
        json.member("reading", shape.reading);
        if (!shape.unread.empty()) {
            json.member("unread", shape.unread);
        }
        json.key("payload_ids");
        json.begin_array();
        for (const std::uint64_t id : shape.ids) {
            json.value(id);
        }
        json.end_array();
        json.member("primary_protection_code", shape.primary_code);
        json.member("secondary_protection_code", shape.secondary_code);
        json.member("frames", shape.frames);
        json.member("first_unit", shape.first_unit);
        json.member("last_unit", shape.last_unit);
        json.key("payload_bytes");
        json.begin_array();
        for (std::size_t i = 0; i < shape.size_min.size(); ++i) {
            json.begin_object();
            json.member("min", static_cast<std::uint64_t>(shape.size_min[i]));
            json.member("max", static_cast<std::uint64_t>(shape.size_max[i]));
            json.end_object();
        }
        json.end_array();
        json.key("thrift_walks");
        json.begin_array();
        for (const PayloadShape& payload : shape.example) {
            if (!payload.thrift) {
                continue;
            }
            json.begin_object();
            json.member("payload_id", payload.id);
            json.key("walk");
            write_outline(json, *payload.thrift);
            json.end_object();
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();
    json.member("frames_in_other_shapes", other_shape_frames_);
    json.end_object();
}

void EvolutionDetail::print_text() const {
    fmt::println("{:<16}frames in detail (the first {} of {}):", "", frames_.size(), frames_total_);
    for (const FrameRecord& frame : frames_) {
        fmt::println(
            "{:<16}  unit {}: extra data {} word(s), frame {} byte(s); nibble {}, framing {}, "
            "parity {}, evo_reserved {}, length {}",
            "", frame.unit, frame.extra_data_length, frame.evo_frame_byte_length,
            frame.check_ok ? "ok" : "BAD", frame.framing_ok ? "ok" : "BAD",
            frame.parity_ok ? "ok" : "BAD", frame.reserved_zero ? "zero" : "SET",
            frame.length_fits ? "fits" : "TOO SMALL");
        if (frame.reading == "none") {
            fmt::println("{:<16}    not read: EMDF {}, section 15.4 {}", "", frame.emdf_error,
                         frame.letter_error);
            continue;
        }
        fmt::println(
            "{:<16}    read as {}: version {}, key id {}, protection codes {}/{}, {} bit(s) "
            "used, {}",
            "", frame.reading, frame.version, frame.key_id, frame.primary_code,
            frame.secondary_code, frame.bits_used,
            frame.trailing_zero ? "only zero bits after" : "NON-ZERO bits after");
        for (const PayloadShape& payload : frame.payloads) {
            print_payload(payload, 4);
        }
    }
    fmt::println("{:<16}shapes (payload ids, protection codes, reading):", "");
    int number = 0;
    for (const Shape& shape : shapes_) {
        std::string ids;
        for (const std::uint64_t id : shape.ids) {
            ids += fmt::format("{}{}", ids.empty() ? "" : ",", id);
        }
        std::string sizes;
        for (std::size_t i = 0; i < shape.size_min.size(); ++i) {
            sizes += fmt::format("{}{}..{}", sizes.empty() ? "" : " ", shape.size_min[i],
                                 shape.size_max[i]);
        }
        fmt::println(
            "{:<16}  shape {}: {} frame(s), units {} to {}, {}, payload ids [{}], bytes [{}], "
            "protection codes {}/{}{}",
            "", ++number, shape.frames, shape.first_unit, shape.last_unit, shape.reading, ids,
            sizes, shape.primary_code, shape.secondary_code,
            shape.unread.empty() ? "" : " (" + shape.unread + ")");
        for (const PayloadShape& payload : shape.example) {
            if (payload.thrift) {
                fmt::println("{:<16}    payload {} of its first frame:", "", payload.id);
                print_outline(*payload.thrift, 6);
            }
        }
    }
    if (other_shape_frames_ != 0) {
        fmt::println("{:<16}  {} frame(s) of further shapes not listed", "", other_shape_frames_);
    }
}

}  // namespace forge_cli::commands
