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

#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/stage.hpp"
#include "iclforge/now_view.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/room_plan.hpp"
#include "panel/lvgl/board_info.hpp"
#include "panel/lvgl/fonts.hpp"
#include "panel/lvgl/lyric_screen.hpp"
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
// The no-lyrics screen's data: what the player says is playing, and what the settings say to show.
player::panel::NowPlaying g_now;
bool g_now_playing = false;
player::panel::NowSettings g_settings;
int g_backlight = 80;

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
void play_room(const char* layout, const iclforge::nowview::StreamFacts& facts, const std::vector<Level>& levels) {
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

bool read_now_playing(NowPlaying& out) {
    if (!g_now_playing) {
        return false;
    }
    out = g_now;
    return true;
}

NowSettings read_now_settings() { return g_settings; }

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

    // S9: the sheet, over a line being sung.
    hold(auld, 5'750);
    g_now_us += 1'000'000;
    pump(6);
    tap(1024 - 32 - 80, 600 - 64 + 8 + 24);
    pump(6);
    write_bmp(out / "s9-controls.bmp");
    std::printf("%2d  %-26s sheet\n", ++number, "s9-controls");
    return 0;
}
