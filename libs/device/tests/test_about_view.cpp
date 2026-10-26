// About, the read-only page of a board's Preferences, tested on the host - see iclforge/about_view.hpp's
// own header comment.

#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/about_view.hpp"

using namespace iclforge::aboutview;

namespace {

// The P4X kit on its cable, with Wi-Fi joined as well, paired with two servers.
Facts kit() {
    Facts f;
    f.active = "ethernet";
    f.preference = "auto";
    f.ethernet.present = true;
    f.ethernet.enabled = true;
    f.ethernet.link = true;
    f.ethernet.address = "192.168.1.163";
    f.ethernet.mac = "e8:f6:0a:e7:53:fb";
    f.wifi.present = true;
    f.wifi.enabled = true;
    f.wifi.link = true;
    f.wifi.address = "192.168.1.251";
    f.wifi.mac = "e8:f6:0a:e7:53:f9";
    f.wifi.configured = true;
    f.name = "Study";
    f.mdns = "study.local";
    f.web_address = "192.168.1.163";
    f.web_port = 80;
    f.firmware = "v0.10.0-beta.1-5228-g0e45f23a7";
    f.slot = "ota_0";
    f.chip = "ESP32-P4";
    f.revision_major = 3;
    f.revision_minor = 2;
    f.layout = "5.1.2";
    f.slots = 16;
    f.player = true;
    f.paired = 2;
    return f;
}

const Row* find(const std::vector<Row>& rows, const std::string& label) {
    for (const Row& row : rows) {
        if (row.label == label) {
            return &row;
        }
    }
    return nullptr;
}

// The row with this label, which the test requires to be there.
const Row& row_of(const std::vector<Row>& rows, const std::string& label) {
    for (const Row& row : rows) {
        if (row.label == label) {
            return row;
        }
    }
    throw std::out_of_range("no row labelled " + label);
}

}  // namespace

TEST_CASE("the version is the tag and the commits since, without git's tails", "[aboutview]") {
    CHECK(version_text("v0.10.0-beta.1-5228-g0e45f23a7") == "v0.10.0-beta.1-5228");
    CHECK(version_text("v0.10.0-beta.1-5228-g0e45f23a7-dirty") == "v0.10.0-beta.1-5228");
    CHECK(version_text("v0.10.0-beta.1-5228") == "v0.10.0-beta.1-5228");
    CHECK(version_text("0.10.0-beta.1-5127") == "0.10.0-beta.1-5127");
    CHECK(version_text("v0.11.0") == "v0.11.0");
    // A tag that ends in letters after -g that are not all hexadecimal is not a hash.
    CHECK(version_text("v1.0-gamma") == "v1.0-gamma");
    CHECK(version_text("1") == "1");  // the CI container's PROJECT_VER
    CHECK(version_text("") == "unknown");
    CHECK(version_text("-dirty") == "-dirty");
}

TEST_CASE("About reads out the friendly subset in order", "[aboutview]") {
    const std::vector<Row> rows = about_rows(kit());
    std::vector<std::string> labels;
    for (const Row& row : rows) {
        labels.push_back(row.label);
    }
    const std::vector<std::string> expected = {"Version", "Running from", "Name",   "Wired", "Wi-Fi",
                                               "Web page", "Chip",         "Speaker layout", "Paired servers"};
    CHECK(labels == expected);

    CHECK(row_of(rows, "Version").value == "v0.10.0-beta.1-5228");
    CHECK(row_of(rows, "Running from").value == "ota_0");
    CHECK(row_of(rows, "Name").value == "Study \xC2\xB7 study.local");
    CHECK(row_of(rows, "Wired").value == "in use \xC2\xB7 192.168.1.163 \xC2\xB7 e8:f6:0a:e7:53:fb");
    CHECK(row_of(rows, "Wi-Fi").value == "192.168.1.251 \xC2\xB7 e8:f6:0a:e7:53:f9");
    CHECK(row_of(rows, "Web page").value == "http://192.168.1.163/");
    CHECK(row_of(rows, "Chip").value == "ESP32-P4 v3.2");
    CHECK(row_of(rows, "Speaker layout").value == "5.1.2 \xC2\xB7 16 outputs");
    CHECK(row_of(rows, "Paired servers").value == "2");
}

TEST_CASE("the interface in use is the one the board says", "[aboutview]") {
    Facts f = kit();
    f.active = "wifi";
    const std::vector<Row> rows = about_rows(f);
    CHECK(row_of(rows, "Wired").value.find("in use") == std::string::npos);
    CHECK(row_of(rows, "Wi-Fi").value.find("in use") == 0);
}

TEST_CASE("a board with Wi-Fi alone has no wired row", "[aboutview]") {
    Facts f = kit();
    f.ethernet = {};
    f.preference.clear();
    f.active = "wifi";
    const std::vector<Row> rows = about_rows(f);
    CHECK(find(rows, "Wired") == nullptr);
    REQUIRE(find(rows, "Wi-Fi") != nullptr);
    CHECK(row_of(rows, "Wi-Fi").value.find("in use \xC2\xB7 192.168.1.251") == 0);
}

TEST_CASE("a board that has no address yet says so, quietly", "[aboutview]") {
    Facts f = kit();
    f.active.clear();
    f.web_address.clear();
    f.ethernet.address.clear();
    f.wifi.address.clear();
    const std::vector<Row> rows = about_rows(f);
    CHECK(row_of(rows, "Web page").value == "no address yet");
    CHECK(row_of(rows, "Web page").tone == Tone::muted);
    CHECK(row_of(rows, "Wired").tone == Tone::muted);
}

TEST_CASE("a build with no network says so", "[aboutview]") {
    Facts f = kit();
    f.ethernet = {};
    f.wifi = {};
    f.active.clear();
    f.web_address.clear();
    const std::vector<Row> rows = about_rows(f);
    CHECK(find(rows, "Wired") == nullptr);
    CHECK(find(rows, "Wi-Fi") == nullptr);
    CHECK(row_of(rows, "Network").value == "none");
}

TEST_CASE("a build without a Sendspin player has no pairings to count", "[aboutview]") {
    Facts f = kit();
    f.player = false;
    CHECK(row_of(about_rows(f), "Paired servers").value == "no Sendspin player in this build");
    f.player = true;
    f.paired = 0;
    const std::vector<Row> rows = about_rows(f);
    const Row* row = find(rows, "Paired servers");
    REQUIRE(row != nullptr);
    CHECK(row->value == "0");
    CHECK(row->tone == Tone::muted);
}

TEST_CASE("a non-port web page carries its port", "[aboutview]") {
    Facts f = kit();
    f.web_port = 8080;
    CHECK(row_of(about_rows(f), "Web page").value == "http://192.168.1.163:8080/");
}

TEST_CASE("About never carries what the developer view never carries", "[aboutview]") {
    Facts f = kit();
    f.wifi.ssid = "HomeNet";
    const std::vector<Row> rows = about_rows(f);
    for (const Row& row : rows) {
        CHECK(row.value.find("pass") == std::string::npos);
        CHECK(row.label.find("pass") == std::string::npos);
        CHECK(row.label.find("token") == std::string::npos);
    }
}
