// The lyric screen on a computer (see CMakeLists.txt): lyric_screen.cpp, as the board runs it, fed a
// document and a position instead of the Sendspin player, drawn by LVGL's software renderer into a
// 1024 by 600 buffer and written as one 24-bit BMP a scenario. The scenarios are the screens of
// planning/karaoke-p4x-panel.md, "The screens", S1 to S9.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "lvgl.h"

#include "iclforge/dev_view.hpp"
#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/stage.hpp"
#include "iclforge/now_view.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/room_plan.hpp"
#include "iclforge/title_card.hpp"
#include "iclforge/wall_clock.hpp"
#include "panel/lvgl/board_info.hpp"
#include "panel/lvgl/dev_info.hpp"
#include "panel/lvgl/fonts.hpp"
#include "panel/lvgl/lyric_screen.hpp"
#include "panel/lvgl/now_screen.hpp"
#include "panel/lyrics_reader.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kWidth = 1024;
constexpr int kHeight = 600;
constexpr int kBufferRows = 40;

// --- the preview's player: one document, one position, one clock.
iclforge::lyrics::Lyrics g_doc;
bool g_held = false;
std::optional<std::int64_t> g_position;
std::int64_t g_item = 1;
std::int64_t g_now_us = 0;
player::panel::BoardInfo g_board;
// What the developer view reads, and the console under it.
iclforge::devview::Facts g_dev;
std::string g_log;
// The no-lyrics screen's data: what the player says is playing, and what the settings say to show.
player::panel::NowPlaying g_now;
bool g_now_playing = false;
player::panel::NowSettings g_settings;
int g_backlight = 80;
// What the server says is playing (metadata@v1): the player's store, and where the song is on the clock.
iclforge::nowview::TitleStore g_title;
bool g_title_held = false;
std::optional<std::int64_t> g_title_position;
// The clock view's time (what the board's wall clock would say), and what LVGL has flushed so far.
iclforge::wallclock::Reading g_wall;
std::uint64_t g_flushed_pixels = 0;
std::uint32_t g_flushed_areas = 0;

std::vector<std::uint16_t> g_frame(static_cast<std::size_t>(kWidth) * kHeight, 0);

struct Pointer {
    int x = 0;
    int y = 0;
    bool down = false;
} g_pointer;

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string read_text(const fs::path& path) {
    const auto bytes = read_file(path);
    return {bytes.begin(), bytes.end()};
}

player::panel::FontFile load_font(const char* name) {
    static std::vector<std::vector<std::uint8_t>> keep;
    keep.push_back(read_file(fs::path(PREVIEW_FONT_DIR) / name));
    return {keep.back().data(), keep.back().size()};
}

void flush(lv_display_t* disp, const lv_area_t* area, std::uint8_t* px_map) {
    const int width = lv_area_get_width(area);
    g_flushed_pixels += static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(lv_area_get_height(area));
    ++g_flushed_areas;
    const auto* src = reinterpret_cast<const std::uint16_t*>(px_map);
    for (int y = area->y1; y <= area->y2; ++y) {
        std::copy(src, src + width, g_frame.begin() + static_cast<std::ptrdiff_t>(y) * kWidth + area->x1);
        src += width;
    }
    lv_display_flush_ready(disp);
}

void read_pointer(lv_indev_t*, lv_indev_data_t* data) {
    data->point.x = g_pointer.x;
    data->point.y = g_pointer.y;
    data->state = g_pointer.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// One 24-bit BMP, bottom row first.
void write_bmp(const fs::path& path) {
    const std::size_t row_bytes = (static_cast<std::size_t>(kWidth) * 3 + 3) / 4 * 4;
    const std::size_t image = row_bytes * kHeight;
    std::vector<std::uint8_t> out(54 + image, 0);
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    };
    out[0] = 'B';
    out[1] = 'M';
    put(2, static_cast<std::uint32_t>(out.size()));
    put(10, 54);
    put(14, 40);
    put(18, kWidth);
    put(22, kHeight);
    out[26] = 1;
    out[28] = 24;
    put(34, static_cast<std::uint32_t>(image));
    for (int y = 0; y < kHeight; ++y) {
        std::uint8_t* row = out.data() + 54 + static_cast<std::size_t>(kHeight - 1 - y) * row_bytes;
        for (int x = 0; x < kWidth; ++x) {
            const std::uint16_t p = g_frame[static_cast<std::size_t>(y) * kWidth + x];
            const std::uint8_t r5 = (p >> 11) & 0x1F;
            const std::uint8_t g6 = (p >> 5) & 0x3F;
            const std::uint8_t b5 = p & 0x1F;
            *row++ = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));
            *row++ = static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4));
            *row++ = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
        }
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

// Frames of the screen's own loop: the clock moves 33 ms a frame, as the panel's task's does.
void pump(int frames) {
    for (int i = 0; i < frames; ++i) {
        g_now_us += 33'000;
        player::panel::screen_update();
        lv_timer_handler();
    }
}

void hold(const std::string& lrc, std::optional<std::int64_t> position_ms) {
    const auto parsed = iclforge::lyrics::parse_lyrics(lrc);
    if (!parsed) {
        std::fprintf(stderr, "scenario document: %s\n", parsed.error().c_str());
        std::exit(2);
    }
    g_doc = *parsed;
    g_held = true;
    g_position = position_ms;
    ++g_item;
}

void tap(int x, int y) {
    g_pointer = {x, y, true};
    pump(4);
    g_pointer.down = false;
    pump(4);
}

// One output's level over the last 100 ms, in dB of full scale: the peak and the RMS.
struct Level {
    float peak;
    float rms;
};

// A stream playing to the room `layout` names, with the decoder's findings and a level for every slot.
void play_room(const char* layout, const iclforge::nowview::StreamFacts& facts, const std::vector<Level>& levels,
               int kbps = 0, const char* server = "") {
    const auto parsed = iclforge::render::OutputLayout::parse(layout);
    if (!parsed) {
        std::fprintf(stderr, "scenario layout %s\n", layout);
        std::exit(2);
    }
    g_now = {};
    const auto spots = iclforge::render::plan_spots(*parsed);
    g_now.spots.assign(spots.begin(), spots.end());
    const std::string heading = iclforge::nowview::heading(facts, layout, spots.size());
    g_now.heading.assign(heading.data(), heading.size());
    g_now.facts = facts;
    g_now.kbps = kbps;
    g_board.server = server;
    for (const Level& level : levels) {
        g_now.peak_db.push_back(level.peak);
        g_now.rms_db.push_back(level.rms);
    }
    g_now_playing = true;
    g_board.stream = "bursts";
    g_held = false;
    g_position.reset();
}

// A music mix over a few rooms: sample levels, not measurements.
// The server says a song is playing (the specification's whole object), and where it is: the title card's
// and the speakers' header's data. An empty artist or album is not said.
void say_title(const char* title, const char* artist, const char* album, std::int32_t year,
               std::optional<std::int64_t> position_ms, std::int64_t length_ms) {
    namespace nv = iclforge::nowview;
    g_title.clear();
    nv::TitleUpdate update;
    update.whole = true;
    const auto text = [](const char* value) {
        return *value == '\0' ? nv::TitleUpdate::Text{} : nv::TitleUpdate::Text{.say = nv::Say::kSet, .value = value};
    };
    update.title = text(title);
    update.artist = text(artist);
    update.album = text(album);
    if (year != 0) {
        update.year = {.say = nv::Say::kSet, .value = year};
    }
    if (position_ms) {
        update.progress = {.say = nv::Say::kSet, .position_ms = *position_ms, .duration_ms = length_ms, .speed = 1000};
    }
    g_title.apply(update);
    g_title_held = true;
    g_title_position = position_ms;
}

void say_no_title() {
    g_title.clear();
    g_title_held = false;
    g_title_position.reset();
}

const std::vector<Level> kStereo = {{-9, -19}, {-11, -21}};
const std::vector<Level> kFiveOne = {{-10, -19}, {-14, -24}, {-11, -20}, {-28, -37}, {-26, -35}, {-16, -27}};
const std::vector<Level> kFiveOneNoCentre = {{-10, -19}, {-120, -120}, {-11, -20}, {-27, -36}, {-26, -35}, {-16, -27}};
const std::vector<Level> kSevenOneFour = {{-12, -21}, {-17, -27}, {-13, -22}, {-24, -33}, {-22, -31}, {-30, -40},
                                          {-31, -41}, {-36, -46}, {-35, -45}, {-42, -52}, {-44, -54}, {-18, -29}};
// 9.1.6: L C R Ls Rs Lrs Rrs Lw Rw, six heights, the LFE.
const std::vector<Level> kNineOneSix = {{-12, -21}, {-17, -27}, {-13, -22}, {-24, -33}, {-22, -31}, {-30, -40}, {-31, -41},
                                        {-26, -35}, {-27, -36}, {-36, -46}, {-35, -45}, {-38, -48}, {-42, -52}, {-44, -54},
                                        {-46, -56}, {-18, -29}};

constexpr iclforge::nowview::StreamFacts kDolbyFiveOne{.codec = iclforge::nowview::Codec::kEac3, .have_decoder = true,
                                                       .acmod = 7, .lfe = true, .objects = 0};

struct Scenario {
    const char* name;
    std::function<void()> set_up;
    // How far the clock moves before the picture, so that a glow settles on the levels it is given.
    std::int64_t settle_us = 1'000'000;
};

// A P4X kit with its cable in and its radio joined, a stream playing and nothing gone wrong: the
// developer view's first picture.
iclforge::devview::Facts healthy_board() {
    using namespace iclforge::devview;
    Facts f;
    f.active = "ethernet";
    f.preference = "auto";
    f.ethernet = {.present = true, .enabled = true, .link = true, .address = "192.168.1.163",
                  .mac = "e8:f6:0a:e7:53:fb", .speed_mbps = 100, .full_duplex = true};
    f.wifi = {.present = true, .enabled = true, .link = true, .address = "192.168.1.251",
              .mac = "e8:f6:0a:e7:53:f9", .configured = true, .ssid = "HomeNet", .rssi_dbm = -52};
    f.name = "hearth-e753f9";
    f.mdns = "hearth-e753f9.local";
    f.web_address = "192.168.1.163";
    f.profile = "p4-rev3";
    f.profile_hash = "ff9b27a6c4fd";
    f.firmware = "0.10.0-beta.1-5127-g2337af6d2";
    f.slot = "ota_0";
    f.idf = "v6.1.0";
    f.chip = "ESP32-P4";
    f.revision_major = 3;
    f.revision_minor = 2;
    f.cores = 2;
    f.cpu_mhz = 400;
    f.psram_mhz = 200;
    f.psram_bytes = 32ULL * 1024 * 1024;
    f.layout = "5.1";
    f.sink = "i2s-tdm";
    f.slots = 16;
    f.slot_bits = 32;
    f.uptime_ms = (3ULL * 3600 + 12 * 60) * 1000;
    f.reset_reason = "wdt";
    f.player = true;
    f.server = "Hearth on STUDIO-PC";
    f.connections = 1;
    f.paired = 2;
    f.clock_converged = true;
    f.clock_error_us = 120;
    f.stream = "bursts";
    f.bursts = 1234;
    f.error_us = 400;
    f.worst_error_us = 2100;
    f.burst_us = 410;
    f.worst_burst_us = 1120;
    f.peak_db = {-9.0F, -11.5F, -30.0F, -18.0F, -80.0F, -16.0F};
    f.internal_free = 100'762;
    f.internal_low = 62'464;
    f.internal_largest = 53'248;
    f.psram_free = 29'800'000;
    f.psram_low = 29'000'000;
    f.refreshes = 4200;
    f.render_us_mean = 6100;
    f.render_us_max = 21'000;
    f.touch_reads = 12'345;
    f.touch_points = 41;
    return f;
}

const char* const kHealthyLog =
    "I (11210) sendspin: server 'Hearth on STUDIO-PC' admitted, clock converged\n"
    "I (11420) sendspin: stream started: bursts, 5.1, 48000 Hz\n"
    "I (21460) panel: lyrics, 2150 refreshes, render 6100 us mean 21000 max, rebuild 3100 us max, 0 late\n"
    "I (31470) net: ethernet: link up, 100 Mb/s full duplex\n"
    "I (41480) panel: lyrics, 2290 refreshes, render 6100 us mean 21000 max, rebuild 3100 us max, 0 late\n";

}  // namespace

namespace player::panel {

bool read_lyrics(LyricsReader& reader, std::int64_t delay_ms,
                 const iclforge::lyrics::StageOptions& options) {
    const iclforge::lyrics::Lyrics* const lyrics = g_held ? &g_doc : nullptr;
    const iclforge::lyrics::Stage stage =
        iclforge::lyrics::make_stage(lyrics, g_position, delay_ms, options);
    reader.on_lyrics(g_held ? g_item : -1, lyrics, stage);
    return true;
}

BoardInfo read_board_info() { return g_board; }

iclforge::devview::Facts read_dev_facts() { return g_dev; }

std::string read_log_tail(std::size_t /*limit*/) { return g_log; }

// The board takes a moment to change; the preview changes at once and says it is done.
bool request_network_preference(iclforge::devview::Preference preference) {
    g_dev.preference = iclforge::devview::preference_word(preference);
    g_dev.net_change = iclforge::devview::NetChange::done;
    g_dev.net_change_word = g_dev.preference;
    return true;
}

bool read_now_playing(NowPlaying& out) {
    if (!g_now_playing) {
        return false;
    }
    out = g_now;
    return true;
}

bool read_now_title(NowTitle& out) {
    now_copy_title(out, g_title_held && g_title.known() ? &g_title : nullptr, g_title_position);
    return true;
}

NowSettings read_now_settings() { return g_settings; }

iclforge::wallclock::Reading read_wall_clock() { return g_wall; }

void set_backlight(int percent) { g_backlight = percent; }

void choose_now_view(iclforge::nowview::View view) { g_settings.view = view; }

void choose_dim_after(int minutes) { g_settings.dim_after_minutes = minutes; }

std::int64_t time_us() { return g_now_us; }

FontFile archivo_regular() {
    static const FontFile font = load_font("Archivo-Regular.ttf");
    return font;
}
FontFile archivo_semibold() {
    static const FontFile font = load_font("Archivo-SemiBold.ttf");
    return font;
}
FontFile noto_sans_greek_cyrillic_regular() {
    static const FontFile font = load_font("NotoSansGreekCyrillic-Regular.ttf");
    return font;
}
FontFile noto_sans_greek_cyrillic_semibold() {
    static const FontFile font = load_font("NotoSansGreekCyrillic-SemiBold.ttf");
    return font;
}
FontFile noto_sans_hebrew_regular() {
    static const FontFile font = load_font("NotoSansHebrew-Regular.ttf");
    return font;
}
FontFile noto_sans_hebrew_semibold() {
    static const FontFile font = load_font("NotoSansHebrew-SemiBold.ttf");
    return font;
}

}  // namespace player::panel

int main(int argc, char** argv) {
    const fs::path out = argc > 1 ? fs::path(argv[1]) : fs::path("panel-preview");
    fs::create_directories(out);
    const fs::path examples = PREVIEW_EXAMPLES_DIR;
    const std::string auld = read_text(examples / "auld-lang-syne" / "auld-lang-syne.lrc");
    const std::string auld_lines = read_text(examples / "auld-lang-syne" / "auld-lang-syne-lines-only.lrc");
    const std::string johnny = read_text(examples / "leave-her-johnny" / "leave-her-johnny.lrc");

    // "Hava nagila", as UTF-8 bytes (nothing here depends on the compiler's source encoding).
    const std::string hava = "\xD7\x94\xD7\x91\xD7\x94 ";
    const std::string nagila = "\xD7\xA0\xD7\x92\xD7\x99\xD7\x9C\xD7\x94";
    const std::string venismeha = "\xD7\x95\xD7\xA0\xD7\xA9\xD7\x9E\xD7\x97\xD7\x94";
    const std::string hebrew =
        "[ti:Hava nagila]\n[00:00.00]<00:00.00>" + hava + "<00:00.80>" + nagila + "\n"
        "[00:03.00]<00:03.00>" + hava + "<00:03.80>" + nagila + "\n"
        "[00:06.00]<00:06.00>" + hava + "<00:06.70>" + nagila + " <00:07.60>" + venismeha + "\n"
        "[00:10.00]<00:10.00>" + hava + "<00:10.70>" + nagila + " <00:11.60>" + venismeha + "\n";
    // Russian and Greek, which the fonts have, among Latin ones: "Privet, mir" and "Kalimera kosme".
    const std::string privet = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
    const std::string mir = "\xD0\xBC\xD0\xB8\xD1\x80";
    const std::string kalimera = "\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1";
    const std::string kosme = "\xCE\xBA\xCF\x8C\xCF\x83\xCE\xBC\xCE\xB5";
    const std::string cyrillic_greek =
        "[ti:" + privet + "]\n[00:00.00]<00:00.00>" + privet + ", <00:01.00>" + mir +
        "\n[00:03.00]<00:03.00>" + kalimera + " <00:04.00>" + kosme +
        "\n[00:06.00]Back to the Latin letters\n[00:09.00]The end\n";
    // A line in Arabic, which the fonts have not, among Latin ones, and a line with music notes.
    const std::string marhaba = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";
    const std::string note = "\xE2\x99\xAA";
    const std::string mixed =
        "[ti:" + marhaba + "]\n[00:00.00]<00:00.00>" + note + " <00:00.60>la <00:01.20>la <00:01.80>la " + note +
        "\n[00:03.00]" + marhaba + "\n[00:06.00]Back to the Latin letters\n[00:09.00]The end\n";
    const std::string plain =
        "Should auld acquaintance be forgot,\nAnd never brought to mind?\n"
        "Should auld acquaintance be forgot,\nAnd auld lang syne!\n\n"
        "For auld lang syne, my dear,\nFor auld lang syne,\n"
        "We'll tak a cup o' kindness yet,\nFor auld lang syne.\n";

    lv_init();
    lv_tick_set_cb([]() -> std::uint32_t { return static_cast<std::uint32_t>(g_now_us / 1000); });
    lv_display_t* const disp = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    static std::uint16_t first[kWidth * kBufferRows];
    static std::uint16_t second[kWidth * kBufferRows];
    lv_display_set_buffers(disp, first, second, sizeof first, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, &flush);
    lv_indev_t* const pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, &read_pointer);
    lv_indev_set_display(pointer, disp);

    g_board.name = "hearth-4f2a91";
    g_board.address = "192.168.1.50";
    g_board.network = "HomeNet";
    g_board.firmware = "0.11.0";
    g_board.player = true;
    if (!player::panel::screen_init(disp, kWidth, kHeight)) {
        return 1;
    }

    const std::vector<Scenario> scenarios = {
        {"s1-idle-unpaired", [&] { g_held = false; g_position.reset(); }},
        {"s1-idle-pairing",
         [&] {
             g_board.pairing_code = "4821";
             g_held = false;
         }},
        {"s1-idle-paired",
         [&] {
             g_board.pairing_code.clear();
             g_board.server = "Hearth on STUDIO-PC";
             g_board.paired = 1;
             g_board.connections = 1;
             g_held = false;
         }},
        {"s7-no-lyrics", [&] {
             g_board.stream = "bursts";
             g_held = false;
             g_settings.view = iclforge::nowview::View::kOff;
         }},
        {"s13-speakers-2-0", [&] {
             g_settings.view = iclforge::nowview::View::kSpeakers;
             play_room("2.0", {.codec = iclforge::nowview::Codec::kPcm}, kStereo);
         }, 10'000'000},
        {"s13-speakers-5-1", [&] { play_room("5.1", kDolbyFiveOne, kFiveOne); }, 10'000'000},
        {"s13-speakers-5-1-silent-centre", [&] { play_room("5.1", kDolbyFiveOne, kFiveOneNoCentre); }, 10'000'000},
        {"s13-speakers-7-1-4", [&] {
             play_room("7.1.4", {.codec = iclforge::nowview::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true,
                                 .objects = 15},
                       kSevenOneFour);
         }, 10'000'000},
        {"s13-speakers-9-1-6", [&] { play_room("9.1.6", kDolbyFiveOne, kNineOneSix); }, 10'000'000},
        // Stream details (a server that is not Hearth: Music Assistant): decoded on this board, the bitrate and
        // who sends it, in the footer; and PCM, which the server decoded.
        {"s17-ma-atmos-7-1-4", [&] {
             g_settings.view = iclforge::nowview::View::kSpeakers;
             play_room("7.1.4", {.codec = iclforge::nowview::Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true,
                                 .objects = 15},
                       kSevenOneFour, 768, "Music Assistant (d5369777-music-assistant)");
         }, 10'000'000},
        {"s17-ma-title-card", [&] {
             g_settings.view = iclforge::nowview::View::kTitle;
             play_room("5.1", kDolbyFiveOne, kFiveOne, 448, "Music Assistant (d5369777-music-assistant)");
             say_title("Blue in Green", "Miles Davis", "Kind of Blue", 1959, 83'000, 337'000);
         }},
        {"s17-ma-pcm", [&] {
             g_settings.view = iclforge::nowview::View::kSpeakers;
             play_room("2.0", {.codec = iclforge::nowview::Codec::kPcm}, kStereo, 0,
                       "Music Assistant (d5369777-music-assistant)");
             say_no_title();
         }, 10'000'000},
        {"s17-long-server-name", [&] {
             g_settings.view = iclforge::nowview::View::kSpeakers;
             play_room("5.1", kDolbyFiveOne, kFiveOne, 640, "Living room Hearth server on a very long host name (id)");
             say_no_title();
         }, 10'000'000},
        // Stage 2: the title card, once a server has said a title. A song of a record's length, partway.
        {"s16-title-card", [&] {
             g_settings.view = iclforge::nowview::View::kTitle;
             play_room("5.1", kDolbyFiveOne, kFiveOne);
             say_title("Blue in Green", "Miles Davis", "Kind of Blue", 1959, 83'000, 337'000);
         }},
        // A long title is cut at two rows with three dots, and the type steps down to fit.
        {"s16-title-card-long", [&] {
             say_title("Symphony No. 9 in D minor, Op. 125 \"Choral\": IV. Finale - Presto - Allegro assai - "
                       "Allegro energico, sempre ben marcato",
                       "Berliner Philharmoniker, Herbert von Karajan",
                       "Beethoven: The Nine Symphonies (Remastered 2018, Deutsche Grammophon)", 1963, 1'412'000, 1'485'000);
         }},
        // Only a title and an artist: nothing is made up for the lines the server did not send.
        {"s16-title-card-no-album", [&] { say_title("Bohemian Rhapsody", "Queen", "", 0, 15'000, 355'000); }},
        // A title and nothing else, no progress: no time and no line over it.
        {"s16-title-card-title-only", [&] { say_title("Untitled radio programme", "", "", 0, std::nullopt, 0); }},
        // A title in a script the fonts lack is not drawn as boxes: the speakers stand in its place.
        {"s16-title-card-undrawable", [&] { say_title("\xE3\x83\x9C\xE3\x83\x98\xE3\x83\x9F\xE3\x82\xA2\xE3\x83\xB3", "Queen", "", 0, 15'000, 355'000); }},
        // The title card is chosen and no server has said anything: the speakers, never an empty card.
        {"s16-title-chosen-none-said", [&] { say_no_title(); }},
        // The speakers with a title said: the header is the song, with its time and the progress line.
        {"s16-speakers-with-title", [&] {
             g_settings.view = iclforge::nowview::View::kSpeakers;
             say_title("Blue in Green", "Miles Davis", "Kind of Blue", 1959, 83'000, 337'000);
         }, 10'000'000},
        // The clock view (stage 3): Tuesday 20 October 2026, 21:47 in Sydney, the prototype's picture; the same
        // with no time zone set, which says UTC; and before a time server has answered, which says nothing.
        {"s16-clock", [&] {
             g_settings.view = iclforge::nowview::View::kClock;
             play_room("5.1", kDolbyFiveOne, kFiveOne);
             g_wall = {.known = true, .local = {2026, 10, 20, 21, 47, 12, 2}, .zone_set = true};
         }},
        {"s16-clock-utc", [&] { g_wall = {.known = true, .local = {2026, 10, 20, 10, 47, 12, 2}, .zone_set = false}; }},
        {"s16-clock-waiting", [&] { g_wall = {}; }},
        {"s0-waiting", [&] {
             g_board.stream = "bursts";
             hold(auld, std::nullopt);
         }},
        // Half way through "quain" of "acquaintance": three words sung, one being sung.
        {"s2-singing", [&] { hold(auld, 1'750); }},
        {"s2-singing-late-line", [&] { hold(auld, 13'750); }},
        {"s4-instrumental-gap", [&] {
             hold("[00:02.00]<00:02.00>Come <00:02.60>on <00:03.20>in\n[00:05.00]\n"
                  "[00:14.00]<00:14.00>The <00:14.60>second <00:15.20>verse\n",
                  11'400);
         }},
        {"s5-line-timed", [&] { hold(auld_lines, 5'000); }},
        {"s6-plain-text", [&] { hold(plain, 0); }},
        {"s3-duet-first", [&] { hold(johnny, 6'000); }},
        {"s3-duet-second", [&] { hold(johnny, 20'000); }},
        {"s8-right-to-left", [&] { hold(hebrew, 7'000); }},
        {"s10-script-undrawable", [&] { hold(mixed, 4'000); }},
        {"s11-notes-dropped", [&] { hold(mixed, 1'200); }},
        {"s12-cyrillic-greek", [&] { hold(cyrillic_greek, 3'500); }},
    };

    int number = 0;
    for (const Scenario& scenario : scenarios) {
        scenario.set_up();
        g_now_us += scenario.settle_us;  // so that the board's info is read again, and a glow has settled
        pump(6);
        write_bmp(out / (std::string(scenario.name) + ".bmp"));
        const player::panel::ScreenStats stats = player::panel::screen_stats();
        std::printf("%2d  %-26s screen %-9s rebuilds %u\n", ++number, scenario.name, stats.name, stats.rebuilds);
    }

    // S14: the speakers dimmed after six minutes of nothing touched, and a touch waking them. Checked
    // as well as drawn: a screen that stayed dark, or a backlight that stayed down, is a failure.
    {
        g_settings.view = iclforge::nowview::View::kSpeakers;
        g_settings.dim_after_minutes = 5;
        play_room("5.1", kDolbyFiveOne, kFiveOne);
        g_now_us += 10'000'000;
        pump(6);
        const bool lit_before = !player::panel::screen_stats().dimmed && g_backlight == 80;
        g_now_us += 4LL * 60 * 1'000'000;
        pump(6);
        const bool lit_at_four = !player::panel::screen_stats().dimmed && g_backlight == 80;
        g_now_us += 2LL * 60 * 1'000'000;
        pump(6);
        write_bmp(out / "s14-dimmed.bmp");
        const bool dark = player::panel::screen_stats().dimmed && g_backlight == 4;
        tap(500, 300);
        pump(6);
        const bool woken = !player::panel::screen_stats().dimmed && g_backlight == 80;
        std::printf("%2d  %-26s lit %s, lit at 4 min %s, dark at 6 min %s, a touch wakes it %s\n", ++number,
                    "s14-dimmed", lit_before ? "yes" : "NO", lit_at_four ? "yes" : "NO", dark ? "yes" : "NO",
                    woken ? "yes" : "NO");
        if (!lit_before || !lit_at_four || !dark || !woken) {
            return 3;
        }
        // Never dims: the same quiet, no dark.
        g_settings.dim_after_minutes = 0;
        g_now_us += 60LL * 60 * 1'000'000;
        pump(6);
        if (player::panel::screen_stats().dimmed) {
            std::puts("dim after never dimmed the screen");
            return 3;
        }
        g_settings.dim_after_minutes = 5;
    }

    // S16: the title card dims as the speakers do, its dark screen names the song, and a touch wakes it.
    {
        g_settings.view = iclforge::nowview::View::kTitle;
        g_settings.dim_after_minutes = 5;
        play_room("5.1", kDolbyFiveOne, kFiveOne);
        say_title("Blue in Green", "Miles Davis", "Kind of Blue", 1959, 83'000, 337'000);
        // The last scenario left the screen untouched for an hour: a touch is what starts the count.
        tap(500, 300);
        g_now_us += 10'000'000;
        pump(6);
        const bool card = std::string(player::panel::screen_stats().name) == "nolyrics" && !player::panel::screen_stats().dimmed;
        g_now_us += 6LL * 60 * 1'000'000;
        pump(6);
        write_bmp(out / "s16-title-card-dimmed.bmp");
        const bool dark = player::panel::screen_stats().dimmed && g_backlight == 4;
        tap(500, 300);
        pump(6);
        const bool woken = !player::panel::screen_stats().dimmed && g_backlight == 80;
        std::printf("%2d  %-26s card %s, dark at 6 min %s, a touch wakes it %s\n", ++number, "s16-title-dimmed",
                    card ? "yes" : "NO", dark ? "yes" : "NO", woken ? "yes" : "NO");
        if (!card || !dark || !woken) {
            return 3;
        }
        // The server says nothing is playing: the card gives way to the speakers.
        say_no_title();
        g_now_us += 1'000'000;
        pump(6);
        write_bmp(out / "s16-title-cleared.bmp");
        say_no_title();
        g_settings.dim_after_minutes = 5;
        // The sheet below shows the default choice, as a board out of the box does.
        g_settings.view = iclforge::nowview::View::kSpeakers;
    }

    // S16: the clock. A minute that changes redraws the time and nothing else, which is what keeps it cheap
    // beside the audio; a second that changes redraws nothing; and a dim leaves the clock on the glass.
    {
        g_settings.view = iclforge::nowview::View::kClock;
        g_settings.dim_after_minutes = 5;
        play_room("5.1", kDolbyFiveOne, kFiveOne);
        g_wall = {.known = true, .local = {2026, 10, 20, 21, 47, 12, 2}, .zone_set = true};
        g_now_us += 10'000'000;
        pump(6);
        const std::uint64_t before_seconds = g_flushed_pixels;
        g_wall.local.second = 40;
        g_now_us += 1'000'000;
        pump(6);
        const std::uint64_t by_a_second = g_flushed_pixels - before_seconds;
        const std::uint64_t before_minute = g_flushed_pixels;
        const std::uint32_t areas_before = g_flushed_areas;
        g_wall.local.minute = 48;
        g_wall.local.second = 1;
        g_now_us += 1'000'000;
        pump(6);
        const std::uint64_t by_a_minute = g_flushed_pixels - before_minute;
        const std::uint32_t minute_areas = g_flushed_areas - areas_before;
        const auto bright_pixels = [](int x0, int y0, int x1, int y1) {
            int count = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const std::uint16_t p = g_frame[static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x)];
                    count += ((p >> 11) & 0x1F) > 8 ? 1 : 0;
                }
            }
            return count;
        };
        const int lit_pixels = bright_pixels(240, 140, 780, 330);
        // Dim: the backlight goes down and the clock stays.
        g_now_us += 6LL * 60 * 1'000'000;
        g_wall.local.minute = 54;
        pump(6);
        const bool dimmed = player::panel::screen_stats().dimmed && g_backlight == 4;
        const int dim_pixels = bright_pixels(240, 140, 780, 330);
        tap(500, 300);
        pump(6);
        const bool woken = !player::panel::screen_stats().dimmed && g_backlight == 80;
        std::printf("%2d  %-26s a second flushed %llu px, a minute %llu px in %u areas, %d clock pixels lit, dimmed %s "
                    "with %d still lit, a touch wakes it %s\n",
                    ++number, "s16-clock", static_cast<unsigned long long>(by_a_second),
                    static_cast<unsigned long long>(by_a_minute), static_cast<unsigned>(minute_areas), lit_pixels,
                    dimmed ? "yes" : "NO", dim_pixels, woken ? "yes" : "NO");
        // One rectangle (the time's), about 300,000 pixels at most, and nothing for the seconds.
        if (by_a_second != 0 || by_a_minute == 0 || by_a_minute > 300'000 || lit_pixels < 2000 || !dimmed ||
            dim_pixels < 2000 || !woken) {
            return 3;
        }
        // Chosen away from the clock, the clock is gone and the speakers are back.
        g_settings.view = iclforge::nowview::View::kSpeakers;
        g_now_us += 1'000'000;
        pump(6);
        g_settings.dim_after_minutes = 5;
    }

    // S14b: the dark view. Dark from the moment a song with no lyrics begins, however long nothing has
    // touched the panel; a touch lights the speakers for 30 s from the last touch, and a second touch
    // moves that on; then it is dark again. Checked as well as drawn, like S14.
    {
        g_settings.view = iclforge::nowview::View::kDark;
        g_now_us += 3600LL * 1'000'000;  // the panel has been left alone for an hour
        play_room("5.1", kDolbyFiveOne, kFiveOne);
        pump(6);
        const auto state = [] { return player::panel::screen_stats(); };
        const bool dark_at_start = state().dimmed && g_backlight == 0;
        tap(500, 300);
        pump(6);
        const bool woken = !state().dimmed && g_backlight == 80;
        g_now_us += 28'000'000;
        pump(6);
        const bool lit_at_28 = !state().dimmed && g_backlight == 80;
        g_now_us += 4'000'000;
        pump(6);
        const bool dark_at_32 = state().dimmed && g_backlight == 0;
        // A touch 20 s after the first moves the dark on by as much: lit 28 s after it, dark 32 s after.
        tap(500, 300);
        pump(6);
        g_now_us += 20'000'000;
        tap(500, 300);
        pump(6);
        g_now_us += 28'000'000;
        pump(6);
        const bool lit_after_second = !state().dimmed && g_backlight == 80;
        g_now_us += 4'000'000;
        pump(6);
        const bool dark_again = state().dimmed && g_backlight == 0;
        std::printf("%2d  %-26s dark at the start %s, a touch lights it %s, lit at 28 s %s, dark at 32 s %s, "
                    "a second touch moves it on %s, dark again %s\n",
                    ++number, "s14b-dark", dark_at_start ? "yes" : "NO", woken ? "yes" : "NO",
                    lit_at_28 ? "yes" : "NO", dark_at_32 ? "yes" : "NO", lit_after_second ? "yes" : "NO",
                    dark_again ? "yes" : "NO");
        if (!dark_at_start || !woken || !lit_at_28 || !dark_at_32 || !lit_after_second || !dark_again) {
            return 3;
        }
        // The sheet with Dark chosen: the dim row says what a touch does instead of offering dim times.
        tap(500, 300);
        pump(6);
        tap(1024 - 32 - 80, 600 - 64 + 8 + 24);
        pump(6);
        write_bmp(out / "s16-controls-dark.bmp");
        std::printf("%2d  %-26s sheet, dark view chosen\n", ++number, "s16-controls-dark");
        g_now_us += 9'000'000;  // the sheet puts itself away after eight seconds untouched
        pump(6);
        g_settings.view = iclforge::nowview::View::kSpeakers;
        pump(6);
    }

    // S9: the sheet, over a line being sung.
    hold(auld, 5'750);
    g_now_us += 1'000'000;
    pump(6);
    tap(1024 - 32 - 80, 600 - 64 + 8 + 24);
    pump(6);
    write_bmp(out / "s9-controls.bmp");
    std::printf("%2d  %-26s sheet\n", ++number, "s9-controls");

    // S13: the developer view, opened as a person opens it: three taps on the board's name.
    g_dev = healthy_board();
    g_log = kHealthyLog;
    g_board.name = g_dev.name;
    g_now_us += 10'000'000;
    pump(6);
    const int name_x = 100;
    const int name_y = 600 - 32;
    tap(name_x, name_y);
    tap(name_x + 6, name_y - 4);
    tap(name_x - 3, name_y + 5);
    g_now_us += 1'000'000;
    pump(6);
    write_bmp(out / "s15-dev-view.bmp");
    std::printf("%2d  %-26s developer view %s\n", ++number, "s15-dev-view",
                player::panel::screen_stats().dev_view ? "open" : "NOT OPEN");

    // A tap on a segment of the preference control asks first.
    tap(430, 236);  // Wi-Fi
    g_now_us += 600'000;
    pump(6);
    write_bmp(out / "s15-dev-confirm.bmp");
    std::printf("%2d  %-26s confirming the switch to Wi-Fi\n", ++number, "s15-dev-confirm");

    // Cancelling asks nothing of the board; going on does it and says so.
    tap(90, 342);  // Cancel
    g_now_us += 600'000;
    pump(6);
    std::printf("%2d  %-26s preference after Cancel: %s\n", ++number, "s13-cancel", g_dev.preference.c_str());
    tap(430, 236);
    tap(250, 342);  // Switch
    g_now_us += 600'000;
    pump(6);
    write_bmp(out / "s15-dev-switched.bmp");
    std::printf("%2d  %-26s preference after Switch: %s\n", ++number, "s15-dev-switched", g_dev.preference.c_str());

    // Nothing playing, no server connected: the stream's rows are quiet and the meters say so.
    g_dev = healthy_board();
    g_dev.stream = "idle";
    g_dev.peak_db.clear();
    g_dev.server.clear();
    g_dev.connections = 0;
    g_dev.clock_converged = false;
    g_now_us += 600'000;
    pump(6);
    write_bmp(out / "s15-dev-idle.bmp");
    std::printf("%2d  %-26s nothing playing\n", ++number, "s15-dev-idle");

    // The same board with a cable pulled, a weak radio, a stream that has been glitching, a panic
    // behind it and a core dump in flash.
    g_dev = healthy_board();
    g_dev.active = "wifi";
    g_dev.web_address = g_dev.wifi.address;
    // A server whose name is longer than its row: Music Assistant gives its id with it.
    g_dev.server = "Music Assistant (d5369777-music-assistant)";
    g_dev.ethernet.link = false;
    g_dev.ethernet.address.clear();
    g_dev.ethernet.speed_mbps.reset();
    g_dev.ethernet.full_duplex.reset();
    g_dev.wifi.rssi_dbm = -79;
    g_dev.reset_reason = "panic";
    g_dev.core_dump = true;
    g_dev.core_dump_bytes = 262'144;
    g_dev.uptime_ms = 94'000;
    g_dev.underruns = 3;
    g_dev.late = 1'200;
    g_dev.dropped = 2;
    g_dev.resyncs = 1;
    g_dev.clock_converged = false;
    g_dev.internal_low = 9'000;
    g_dev.late_cycles = 4;
    g_dev.played = true;
    g_dev.us_per_frame = 412;
    g_dev.worst_frame_us = 1'120;
    g_dev.sink_us_per_frame = 250;
    g_dev.render_us_per_frame = 80;
    g_now_us += 600'000;
    pump(6);
    write_bmp(out / "s15-dev-trouble.bmp");
    std::printf("%2d  %-26s cable out, glitches, a panic\n", ++number, "s15-dev-trouble");

    // A tap on the view anywhere else closes it.
    g_now_us += 2'000'000;
    tap(900, 300);
    pump(6);
    std::printf("%2d  %-26s closed by a tap: %s\n", ++number, "s15-dev-closed",
                player::panel::screen_stats().dev_view ? "STILL OPEN" : "yes");
    return 0;
}
