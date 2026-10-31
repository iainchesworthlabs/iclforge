#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/dev_view.hpp"

// About, the read-only page of a board's Preferences (planning/p4x-preferences.md, E4): the friendly
// subset of what the developer view shows. The firmware reads the same facts for both
// (panel/lvgl/dev_info.cpp: read_network_now, read_configuration, the pairing count), so the version, the
// addresses and the chip are never read twice in two ways; this is only the choice and the words.
//
// Free of ESP-IDF and LVGL like dev_view.hpp; test_about_view.cpp holds the rows to the letter. What it
// never carries is what the developer view never carries: the Wi-Fi passphrase and the pairing token.

namespace iclforge::aboutview {

using devview::Facts;
using devview::Tone;

struct Row {
    std::string label;
    std::string value;
    Tone tone = Tone::text;
};

// The version as a person reads it: the tag and the commits since, without git's "-g<hash>" and
// "-dirty" tails, which the developer view keeps ("v0.10.0-beta.1-5228-g0e45f23a7" is "v0.10.0-beta.1-5228").
[[nodiscard]] inline std::string version_text(std::string_view firmware) {
    std::string out(firmware);
    constexpr std::string_view kDirty = "-dirty";
    if (out.size() > kDirty.size() && out.compare(out.size() - kDirty.size(), kDirty.size(), kDirty) == 0) {
        out.resize(out.size() - kDirty.size());
    }
    // "-g" and hexadecimal digits at the end.
    const std::size_t g = out.rfind("-g");
    if (g != std::string::npos && out.size() - g >= 4) {
        const bool hex = std::all_of(out.begin() + static_cast<std::ptrdiff_t>(g) + 2, out.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
        if (hex) {
            out.resize(g);
        }
    }
    return out.empty() ? std::string("unknown") : out;
}

// The rows, top to bottom. (Not `describe`, which is devview's and would be found beside it.)
[[nodiscard]] inline std::vector<Row> about_rows(const Facts& facts) {
    using devview::detail::join;
    std::vector<Row> rows;

    rows.push_back({"Version", version_text(facts.firmware), facts.firmware.empty() ? Tone::muted : Tone::text});
    rows.push_back({"Running from", facts.slot.empty() ? std::string("unknown") : facts.slot, facts.slot.empty() ? Tone::muted : Tone::text});
    rows.push_back({"Name", join({facts.name, facts.mdns}), Tone::text});

    // An address and a MAC for each interface this build drives; a build with no network says so.
    const auto interface_row = [&](const char* label, const devview::NetInterface& in) {
        if (!in.present) {
            return;
        }
        std::string value = devview::address_text(in);
        if (facts.active == (std::string_view(label) == "Wired" ? "ethernet" : "wifi")) {
            value = join({"in use", value});
        }
        rows.push_back({label, value, in.address.empty() ? Tone::muted : Tone::text});
    };
    interface_row("Wired", facts.ethernet);
    interface_row("Wi-Fi", facts.wifi);
    if (!facts.ethernet.present && !facts.wifi.present) {
        rows.push_back({"Network", "none", Tone::muted});
    }
    rows.push_back({"Web page", devview::web_url(facts), facts.web_address.empty() ? Tone::muted : Tone::text});

    rows.push_back({"Chip", join({facts.chip + (facts.chip.empty() ? "" : " " + devview::revision_text(facts.revision_major, facts.revision_minor))}),
                    Tone::text});

    std::string slots;
    if (facts.slots > 0) {
        slots = std::to_string(facts.slots) + " outputs";
    }
    rows.push_back({"Speaker layout", join({facts.layout, slots}), facts.layout.empty() ? Tone::muted : Tone::text});

    if (facts.player) {
        rows.push_back({"Paired servers", std::to_string(facts.paired), facts.paired == 0 ? Tone::muted : Tone::text});
    } else {
        rows.push_back({"Paired servers", "no Sendspin player in this build", Tone::muted});
    }
    return rows;
}

}  // namespace iclforge::aboutview
