#include "adm_xml_extras.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace iclforge::adm::detail {

namespace {

struct XmlTag {
    std::string_view name;  // local name: any "prefix:" is dropped
    bool closing = false;
    bool self_closing = false;
    std::vector<std::pair<std::string_view, std::string_view>> attributes;  // raw, undecoded
    std::size_t begin = 0;  // index of '<'
    std::size_t end = 0;    // index one past '>'
};

[[nodiscard]] bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] bool is_name_end(char c) {
    return is_space(c) || c == '/' || c == '>' || c == '=';
}

[[nodiscard]] std::string_view local_name(std::string_view name) {
    const auto colon = name.rfind(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

// The five predefined XML entities plus numeric references are not needed for the values this
// module reads (IDs, zone labels, numbers), but a label such as "A&amp;B" should still round
// trip, so the predefined five are decoded.
[[nodiscard]] std::string decode_entities(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') {
            out.push_back(raw[i]);
            continue;
        }
        const auto semicolon = raw.find(';', i);
        if (semicolon == std::string_view::npos) {
            out.push_back(raw[i]);
            continue;
        }
        const auto entity = raw.substr(i + 1, semicolon - i - 1);
        char decoded = 0;
        if (entity == "amp") {
            decoded = '&';
        } else if (entity == "lt") {
            decoded = '<';
        } else if (entity == "gt") {
            decoded = '>';
        } else if (entity == "quot") {
            decoded = '"';
        } else if (entity == "apos") {
            decoded = '\'';
        }
        if (decoded == 0) {
            out.push_back(raw[i]);
            continue;
        }
        out.push_back(decoded);
        i = semicolon;
    }
    return out;
}

[[nodiscard]] std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// Skips to just past `terminator`, or to the end of the text when it never appears.
[[nodiscard]] std::size_t skip_past(std::string_view xml, std::size_t from, std::string_view terminator) {
    const auto at = xml.find(terminator, from);
    return at == std::string_view::npos ? xml.size() : at + terminator.size();
}

// Advances `pos` to the next element tag, stepping over comments, CDATA sections, processing
// instructions and declarations. Returns false at the end of the text or on a tag it cannot
// finish.
[[nodiscard]] bool next_tag(std::string_view xml, std::size_t& pos, XmlTag& tag) {
    while (true) {
        const auto lt = xml.find('<', pos);
        if (lt == std::string_view::npos) {
            pos = xml.size();
            return false;
        }
        const auto rest = xml.substr(lt);
        if (rest.starts_with("<!--")) {
            pos = skip_past(xml, lt + 4, "-->");
            continue;
        }
        if (rest.starts_with("<![CDATA[")) {
            pos = skip_past(xml, lt + 9, "]]>");
            continue;
        }
        if (rest.starts_with("<?")) {
            pos = skip_past(xml, lt + 2, "?>");
            continue;
        }
        if (rest.starts_with("<!")) {
            pos = skip_past(xml, lt + 2, ">");
            continue;
        }

        tag = XmlTag{};
        tag.begin = lt;
        std::size_t i = lt + 1;
        if (i < xml.size() && xml[i] == '/') {
            tag.closing = true;
            ++i;
        }
        const auto name_begin = i;
        while (i < xml.size() && !is_name_end(xml[i])) {
            ++i;
        }
        tag.name = local_name(xml.substr(name_begin, i - name_begin));

        // Attributes, up to '>' or '/>'.
        while (true) {
            while (i < xml.size() && is_space(xml[i])) {
                ++i;
            }
            if (i >= xml.size()) {
                pos = xml.size();
                return false;
            }
            if (xml[i] == '>') {
                ++i;
                break;
            }
            if (xml[i] == '/') {
                tag.self_closing = true;
                ++i;
                continue;
            }
            const auto attribute_begin = i;
            while (i < xml.size() && !is_name_end(xml[i])) {
                ++i;
            }
            const auto attribute_name = local_name(xml.substr(attribute_begin, i - attribute_begin));
            while (i < xml.size() && is_space(xml[i])) {
                ++i;
            }
            if (i >= xml.size() || xml[i] != '=') {
                pos = xml.size();
                return false;
            }
            ++i;
            while (i < xml.size() && is_space(xml[i])) {
                ++i;
            }
            if (i >= xml.size() || (xml[i] != '"' && xml[i] != '\'')) {
                pos = xml.size();
                return false;
            }
            const char quote = xml[i++];
            const auto value_end = xml.find(quote, i);
            if (value_end == std::string_view::npos) {
                pos = xml.size();
                return false;
            }
            tag.attributes.emplace_back(attribute_name, xml.substr(i, value_end - i));
            i = value_end + 1;
        }
        tag.end = i;
        pos = i;
        return true;
    }
}

[[nodiscard]] const std::string_view* find_attribute(const XmlTag& tag, std::string_view name) {
    for (const auto& [key, value] : tag.attributes) {
        if (key == name) {
            return &value;
        }
    }
    return nullptr;
}

[[nodiscard]] bool read_bound(const XmlTag& tag, std::string_view name, double& out) {
    const auto* raw = find_attribute(tag, name);
    if (raw == nullptr) {
        return false;
    }
    const auto text = trim(*raw);
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

// libadm prints an ID with its hexadecimal digits in upper case, and the model's IDs are what it
// printed, so a key read out of the text has to match that spelling to find its block or pack.
[[nodiscard]] std::string upper_ascii(std::string text) {
    for (auto& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

// A finite number from an attribute. from_chars accepts "inf" and "nan", which no field read here
// can use, so those count as absent.
[[nodiscard]] bool read_finite(const XmlTag& tag, std::string_view name, double& out) {
    double value = 0.0;
    if (!read_bound(tag, name, value) || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] bool parse_finite(std::string_view text, double& out) {
    text = trim(text);
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

// The decoded, trimmed text between a start tag and the next '<'. Empty for a self-closing tag.
[[nodiscard]] std::string element_text(std::string_view xml, const XmlTag& tag) {
    if (tag.self_closing) {
        return {};
    }
    const auto text_end = std::min(xml.find('<', tag.end), xml.size());
    return decode_entities(trim(xml.substr(tag.end, text_end - tag.end)));
}

// An ADM boolean element: "1" or "true".
[[nodiscard]] bool is_true(std::string_view text) {
    text = trim(text);
    return text == "1" || text == "true";
}

[[nodiscard]] std::string attribute_text(const XmlTag& tag, std::string_view name) {
    const auto* raw = find_attribute(tag, name);
    return raw == nullptr ? std::string{} : decode_entities(trim(*raw));
}

[[nodiscard]] std::string format_number(double value) {
    char buffer[64];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error != std::errc{}) {
        return "0";
    }
    return std::string(buffer, end);
}

[[nodiscard]] std::string zone_xml(const std::vector<ExclusionZone>& zones) {
    std::string out = "<zoneExclusion>";
    for (const auto& zone : zones) {
        out += "<zone";
        if (zone.has_bounds) {
            out += " minX=\"" + format_number(zone.min_x) + "\"";
            out += " maxX=\"" + format_number(zone.max_x) + "\"";
            out += " minY=\"" + format_number(zone.min_y) + "\"";
            out += " maxY=\"" + format_number(zone.max_y) + "\"";
            out += " minZ=\"" + format_number(zone.min_z) + "\"";
            out += " maxZ=\"" + format_number(zone.max_z) + "\"";
        }
        if (zone.label.empty()) {
            out += "/>";
        } else {
            out += ">" + escape(zone.label) + "</zone>";
        }
    }
    out += "</zoneExclusion>";
    return out;
}

}  // namespace

ZonesByBlockId scan_zone_exclusions(std::string_view xml) {
    ZonesByBlockId result;
    std::string block_id;
    bool in_block = false;
    bool in_exclusion = false;

    std::size_t pos = 0;
    XmlTag tag;
    while (next_tag(xml, pos, tag)) {
        if (tag.name == "audioBlockFormat") {
            if (tag.closing || tag.self_closing) {
                in_block = false;
                in_exclusion = false;
            } else if (const auto* id = find_attribute(tag, "audioBlockFormatID")) {
                in_block = true;
                block_id = upper_ascii(decode_entities(*id));
            } else {
                in_block = false;
            }
            continue;
        }
        if (!in_block) {
            continue;
        }
        if (tag.name == "zoneExclusion") {
            in_exclusion = !tag.closing && !tag.self_closing;
            continue;
        }
        if (!in_exclusion || tag.closing || tag.name != "zone") {
            continue;
        }

        ExclusionZone zone;
        double min_x = 0.0;
        double max_x = 0.0;
        double min_y = 0.0;
        double max_y = 0.0;
        double min_z = 0.0;
        double max_z = 0.0;
        if (read_bound(tag, "minX", min_x) && read_bound(tag, "maxX", max_x) &&
            read_bound(tag, "minY", min_y) && read_bound(tag, "maxY", max_y) &&
            read_bound(tag, "minZ", min_z) && read_bound(tag, "maxZ", max_z)) {
            zone.has_bounds = true;
            zone.min_x = min_x;
            zone.max_x = max_x;
            zone.min_y = min_y;
            zone.max_y = max_y;
            zone.min_z = min_z;
            zone.max_z = max_z;
        }
        if (!tag.self_closing) {
            const auto text_end = std::min(xml.find('<', tag.end), xml.size());
            zone.label = decode_entities(trim(xml.substr(tag.end, text_end - tag.end)));
        }
        result[block_id].push_back(std::move(zone));
    }
    return result;
}

MatrixBlocksByChannelId scan_matrix_blocks(std::string_view xml) {
    MatrixBlocksByChannelId result;
    std::string channel_id;
    bool in_channel = false;  // inside an audioChannelFormat with the Matrix type label
    bool in_block = false;
    bool in_matrix = false;
    MatrixBlockText block;

    std::size_t pos = 0;
    XmlTag tag;
    while (next_tag(xml, pos, tag)) {
        if (tag.name == "audioChannelFormat") {
            in_channel = false;
            in_block = false;
            in_matrix = false;
            if (!tag.closing && !tag.self_closing) {
                if (const auto* id = find_attribute(tag, "audioChannelFormatID")) {
                    auto key = upper_ascii(decode_entities(*id));
                    // "AC_" and the four hexadecimal digits of the type label; 0002 is Matrix.
                    if (key.starts_with("AC_0002")) {
                        in_channel = true;
                        channel_id = key;
                        // A Matrix channel with no blocks is still seen: an empty entry.
                        result.try_emplace(channel_id);
                    }
                }
            }
            continue;
        }
        if (!in_channel) {
            continue;
        }
        if (tag.name == "audioBlockFormat") {
            if (tag.closing) {
                if (in_block) {
                    result[channel_id].push_back(std::move(block));
                }
                in_block = false;
                in_matrix = false;
                continue;
            }
            in_matrix = false;
            block = MatrixBlockText{};
            block.id = upper_ascii(attribute_text(tag, "audioBlockFormatID"));
            block.rtime = attribute_text(tag, "rtime");
            block.duration = attribute_text(tag, "duration");
            if (tag.self_closing) {
                result[channel_id].push_back(std::move(block));
                in_block = false;
            } else {
                in_block = true;
            }
            continue;
        }
        if (!in_block) {
            continue;
        }
        if (tag.name == "matrix") {
            in_matrix = !tag.closing && !tag.self_closing;
            continue;
        }
        if (tag.closing) {
            continue;
        }
        if (tag.name == "outputChannelFormatIDRef" || tag.name == "outputChannelIDRef") {
            block.output_channel_format_ref = upper_ascii(element_text(xml, tag));
        } else if (tag.name == "gain") {
            block.gain = element_text(xml, tag);
            block.gain_unit = attribute_text(tag, "gainUnit");
        } else if (tag.name == "importance") {
            block.importance = element_text(xml, tag);
        } else if (tag.name == "jumpPosition") {
            block.has_jump_position = true;
            block.jump_position = is_true(element_text(xml, tag));
            double length = 0.0;
            if (read_finite(tag, "interpolationLength", length)) {
                block.has_interpolation_length = true;
                block.interpolation_length_s = length;
            }
        } else if (in_matrix && tag.name == "coefficient") {
            MatrixCoefficient coefficient;
            coefficient.input_channel_format_ref = upper_ascii(element_text(xml, tag));
            double value = 0.0;
            if (read_finite(tag, "gain", value)) {
                // gainUnit "dB" (BS.2076-3 Table A1-16) is stored as linear, as a block's gain is.
                coefficient.gain =
                    attribute_text(tag, "gainUnit") == "dB" ? std::pow(10.0, value / 20.0) : value;
            }
            if (read_finite(tag, "phase", value)) {
                coefficient.phase_deg = value;
            }
            if (read_finite(tag, "delay", value)) {
                coefficient.delay_ms = value;
            }
            coefficient.gain_var = attribute_text(tag, "gainVar");
            coefficient.phase_var = attribute_text(tag, "phaseVar");
            coefficient.delay_var = attribute_text(tag, "delayVar");
            block.matrix.push_back(std::move(coefficient));
        }
    }
    return result;
}

PackExtrasById scan_pack_extras(std::string_view xml) {
    PackExtrasById result;
    std::string pack_id;
    bool in_pack = false;
    bool seen = false;
    PackExtras pack;

    std::size_t pos = 0;
    XmlTag tag;
    while (next_tag(xml, pos, tag)) {
        if (tag.name == "audioPackFormat") {
            if (in_pack && tag.closing && seen) {
                result[pack_id] = std::move(pack);
            }
            in_pack = false;
            seen = false;
            pack = PackExtras{};
            if (!tag.closing && !tag.self_closing) {
                if (const auto* id = find_attribute(tag, "audioPackFormatID")) {
                    in_pack = true;
                    pack_id = upper_ascii(decode_entities(*id));
                }
            }
            continue;
        }
        if (!in_pack || tag.closing) {
            continue;
        }
        if (tag.name == "encodePackFormatIDRef") {
            pack.encode_pack_format_refs.push_back(upper_ascii(element_text(xml, tag)));
            seen = true;
        } else if (tag.name == "decodePackFormatIDRef") {
            pack.decode_pack_format_refs.push_back(upper_ascii(element_text(xml, tag)));
            seen = true;
        } else if (tag.name == "inputPackFormatIDRef") {
            pack.input_pack_format_ref = upper_ascii(element_text(xml, tag));
            seen = true;
        } else if (tag.name == "outputPackFormatIDRef") {
            pack.output_pack_format_ref = upper_ascii(element_text(xml, tag));
            seen = true;
        } else if (tag.name == "normalization") {
            pack.hoa_normalization = element_text(xml, tag);
            seen = true;
        } else if (tag.name == "nfcRefDist") {
            double distance = 0.0;
            if (parse_finite(element_text(xml, tag), distance)) {
                pack.has_nfc_ref_dist = true;
                pack.nfc_ref_dist = distance;
                seen = true;
            }
        } else if (tag.name == "screenRef") {
            pack.screen_ref = is_true(element_text(xml, tag));
            seen = true;
        }
    }
    return result;
}

namespace {

[[nodiscard]] std::string matrix_block_xml(const AudioBlockFormat& block) {
    std::string out;
    if (!block.output_channel_format_ref.empty()) {
        out += "<outputChannelFormatIDRef>" + escape(block.output_channel_format_ref) +
               "</outputChannelFormatIDRef>";
    }
    if (block.has_jump_position) {
        out += "<jumpPosition";
        if (block.has_interpolation_length) {
            out += " interpolationLength=\"" + format_number(block.interpolation_length_s) + "\"";
        }
        out += block.jump_position ? ">1</jumpPosition>" : ">0</jumpPosition>";
    }
    out += "<matrix>";
    for (const auto& coefficient : block.matrix) {
        out += "<coefficient";
        // The standard allows a constant or a variable for each of the three, never both.
        if (!coefficient.gain_var.empty()) {
            out += " gainVar=\"" + escape(coefficient.gain_var) + "\"";
        } else if (coefficient.gain != 1.0) {
            out += " gain=\"" + format_number(coefficient.gain) + "\"";
        }
        if (!coefficient.phase_var.empty()) {
            out += " phaseVar=\"" + escape(coefficient.phase_var) + "\"";
        } else if (coefficient.phase_deg != 0.0) {
            out += " phase=\"" + format_number(coefficient.phase_deg) + "\"";
        }
        if (!coefficient.delay_var.empty()) {
            out += " delayVar=\"" + escape(coefficient.delay_var) + "\"";
        } else if (coefficient.delay_ms != 0.0) {
            out += " delay=\"" + format_number(coefficient.delay_ms) + "\"";
        }
        out += ">" + escape(coefficient.input_channel_format_ref) + "</coefficient>";
    }
    out += "</matrix>";
    if (block.gain != 1.0) {
        out += "<gain>" + format_number(block.gain) + "</gain>";
    }
    if (block.has_importance && block.importance != 10) {
        out += "<importance>" + std::to_string(block.importance) + "</importance>";
    }
    return out;
}

[[nodiscard]] std::string pack_extras_xml(const PackExtras& pack) {
    std::string out;
    for (const auto& ref : pack.encode_pack_format_refs) {
        out += "<encodePackFormatIDRef>" + escape(ref) + "</encodePackFormatIDRef>";
    }
    for (const auto& ref : pack.decode_pack_format_refs) {
        out += "<decodePackFormatIDRef>" + escape(ref) + "</decodePackFormatIDRef>";
    }
    if (!pack.input_pack_format_ref.empty()) {
        out += "<inputPackFormatIDRef>" + escape(pack.input_pack_format_ref) +
               "</inputPackFormatIDRef>";
    }
    if (!pack.output_pack_format_ref.empty()) {
        out += "<outputPackFormatIDRef>" + escape(pack.output_pack_format_ref) +
               "</outputPackFormatIDRef>";
    }
    if (!pack.hoa_normalization.empty()) {
        out += "<normalization>" + escape(pack.hoa_normalization) + "</normalization>";
    }
    if (pack.has_nfc_ref_dist) {
        out += "<nfcRefDist>" + format_number(pack.nfc_ref_dist) + "</nfcRefDist>";
    }
    if (pack.screen_ref) {
        out += "<screenRef>1</screenRef>";
    }
    return out;
}

}  // namespace

std::string inject_matrix_extras(std::string_view xml, const MatrixBlocksById& blocks,
                                 const PackExtrasById& packs) {
    if (blocks.empty() && packs.empty()) {
        return std::string(xml);
    }

    struct Edit {
        std::size_t begin;
        std::size_t end;  // [begin, end) is replaced; begin == end inserts
        std::string text;
    };
    std::vector<Edit> edits;

    std::size_t pos = 0;
    XmlTag tag;
    while (next_tag(xml, pos, tag)) {
        if (tag.closing) {
            continue;
        }
        std::string content;
        if (tag.name == "audioBlockFormat") {
            const auto it = blocks.find(upper_ascii(attribute_text(tag, "audioBlockFormatID")));
            if (it != blocks.end()) {
                content = matrix_block_xml(it->second);
            }
        } else if (tag.name == "audioPackFormat") {
            const auto it = packs.find(upper_ascii(attribute_text(tag, "audioPackFormatID")));
            if (it != packs.end()) {
                content = pack_extras_xml(it->second);
            }
        }
        if (content.empty()) {
            continue;
        }
        if (tag.self_closing) {
            // "<name attr=... />" becomes "<name attr=...>" + content + "</name>".
            const auto raw = xml.substr(tag.begin, tag.end - tag.begin);
            std::string open(raw.substr(0, raw.rfind('/')));
            while (!open.empty() && is_space(open.back())) {
                open.pop_back();
            }
            edits.push_back(
                {tag.begin, tag.end, open + ">" + content + "</" + std::string(tag.name) + ">"});
        } else {
            edits.push_back({tag.end, tag.end, std::move(content)});
        }
    }

    std::string out;
    out.reserve(xml.size() + edits.size() * 160);
    std::size_t copied = 0;
    for (const auto& edit : edits) {
        out.append(xml.substr(copied, edit.begin - copied));
        out += edit.text;
        copied = edit.end;
    }
    out.append(xml.substr(copied));
    return out;
}

std::string inject_zone_exclusions(std::string_view xml, const ZonesByBlockId& zones) {
    if (zones.empty()) {
        return std::string(xml);
    }

    std::vector<std::pair<std::size_t, std::string>> insertions;
    const std::vector<ExclusionZone>* pending = nullptr;
    std::size_t importance_at = std::string_view::npos;

    std::size_t pos = 0;
    XmlTag tag;
    while (next_tag(xml, pos, tag)) {
        if (tag.name == "audioBlockFormat") {
            if (tag.closing) {
                if (pending != nullptr) {
                    insertions.emplace_back(importance_at != std::string_view::npos ? importance_at : tag.begin,
                                            zone_xml(*pending));
                }
                pending = nullptr;
                continue;
            }
            pending = nullptr;
            importance_at = std::string_view::npos;
            if (tag.self_closing) {
                continue;
            }
            if (const auto* id = find_attribute(tag, "audioBlockFormatID")) {
                const auto it = zones.find(decode_entities(*id));
                if (it != zones.end() && !it->second.empty()) {
                    pending = &it->second;
                }
            }
            continue;
        }
        if (pending != nullptr && !tag.closing && tag.name == "importance" &&
            importance_at == std::string_view::npos) {
            importance_at = tag.begin;
        }
    }

    std::string out;
    out.reserve(xml.size() + insertions.size() * 128);
    std::size_t copied = 0;
    for (const auto& [at, text] : insertions) {
        out.append(xml.substr(copied, at - copied));
        out += text;
        copied = at;
    }
    out.append(xml.substr(copied));
    return out;
}

}  // namespace iclforge::adm::detail
