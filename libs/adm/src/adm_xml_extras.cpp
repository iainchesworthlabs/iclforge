#include "adm_xml_extras.hpp"

#include <algorithm>
#include <charconv>
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
                block_id = decode_entities(*id);
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
