// The title card's store of what a server says is playing (iclforge/title_card.hpp), on the host.
//
// What is held here is what the panel would show: a title card that keeps the last track's artist, loses
// its title on a pause, or runs the song's clock from the wrong moment is a screen that says what nobody
// said. The two servers it meets send metadata@v1 differently (the specification a whole state each
// time, aiosendspin 9.1.1 only what changed: planning/hearth-sendspin-extension.md, C36), and each test
// names the one it is about.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

#include "iclforge/title_card.hpp"

namespace nv = iclforge::nowview;
using nv::Say;

namespace {

nv::TitleUpdate::Text set(std::string_view value) { return {.say = Say::kSet, .value = value}; }
constexpr nv::TitleUpdate::Text kNull{.say = Say::kClear, .value = {}};

nv::TitleUpdate::Progress progress(std::int64_t position_ms, std::int64_t duration_ms, std::int32_t speed = 1000) {
    return {.say = Say::kSet, .position_ms = position_ms, .duration_ms = duration_ms, .speed = speed};
}

constexpr std::int64_t kSecond = 1'000'000;

}  // namespace

TEST_CASE("title card: nothing is known until a title arrives", "[title_card]") {
    nv::TitleStore store;
    CHECK_FALSE(store.known());
    CHECK(store.revision() == 0);
    CHECK_FALSE(store.position_ms(10 * kSecond).has_value());
    CHECK_FALSE(store.length_ms().has_value());

    // An artist with no title is not a card.
    nv::TitleUpdate update;
    update.artist = set("Miles Davis");
    store.apply(update);
    CHECK_FALSE(store.known());
    CHECK(store.artist() == "Miles Davis");

    update.title = set("Blue in Green");
    store.apply(update);
    CHECK(store.known());
    CHECK(store.title() == "Blue in Green");
}

TEST_CASE("title card: the specification's object is the whole state, so what it leaves out is cleared",
          "[title_card][spec]") {
    nv::TitleStore store;
    nv::TitleUpdate first;
    first.whole = true;
    first.timestamp_us = 100 * kSecond;
    first.title = set("Blue in Green");
    first.artist = set("Miles Davis");
    first.album = set("Kind of Blue");
    first.year = {.say = Say::kSet, .value = 1959};
    first.progress = progress(0, 337'000);
    store.apply(first);
    CHECK(store.album() == "Kind of Blue");
    CHECK(store.year() == 1959);
    REQUIRE(store.length_ms().has_value());
    CHECK(*store.length_ms() == 337'000);

    // The next track names a title alone: its artist, album, year and position are gone, not the last
    // track's.
    nv::TitleUpdate second;
    second.whole = true;
    second.timestamp_us = 440 * kSecond;
    second.title = set("Evening News");
    store.apply(second);
    CHECK(store.title() == "Evening News");
    CHECK(store.artist().empty());
    CHECK(store.album().empty());
    CHECK_FALSE(store.year().has_value());
    CHECK_FALSE(store.progress().has_value());
    CHECK_FALSE(store.position_ms(450 * kSecond).has_value());
}

TEST_CASE("title card: aiosendspin 9.1.1 sends only what changed, so what it leaves out stays", "[title_card][aiosendspin]") {
    nv::TitleStore store;
    nv::TitleUpdate first;
    first.timestamp_us = 100 * kSecond;
    first.title = set("Blue in Green");
    first.artist = set("Miles Davis");
    first.album = set("Kind of Blue");
    first.progress = progress(0, 337'000);
    store.apply(first);

    SECTION("a pause is a progress and nothing else: the title stays") {
        nv::TitleUpdate pause;
        pause.timestamp_us = 160 * kSecond;
        pause.progress = progress(60'000, 337'000, 0);
        const std::uint32_t before = store.revision();
        store.apply(pause);
        CHECK(store.title() == "Blue in Green");
        CHECK(store.artist() == "Miles Davis");
        CHECK(store.album() == "Kind of Blue");
        CHECK(store.revision() == before);
        // Paused at a minute, whatever the time.
        CHECK(store.position_ms(400 * kSecond) == 60'000);
    }

    SECTION("a cleared field is null, and only that field goes") {
        nv::TitleUpdate next;
        next.timestamp_us = 440 * kSecond;
        next.title = set("Evening News");
        next.artist = kNull;
        next.progress = progress(0, 1'800'000);
        store.apply(next);
        CHECK(store.title() == "Evening News");
        CHECK(store.artist().empty());
        // Not named, so not changed: the album of the last track is what the server still says.
        CHECK(store.album() == "Kind of Blue");
        REQUIRE(store.length_ms().has_value());
        CHECK(*store.length_ms() == 1'800'000);
    }

    SECTION("a null progress clears the position and keeps the words") {
        nv::TitleUpdate stop;
        stop.timestamp_us = 200 * kSecond;
        stop.progress = {.say = Say::kClear};
        store.apply(stop);
        CHECK(store.known());
        CHECK_FALSE(store.position_ms(210 * kSecond).has_value());
    }

    SECTION("a year comes and goes") {
        nv::TitleUpdate year;
        year.year = {.say = Say::kSet, .value = 1959};
        store.apply(year);
        CHECK(store.year() == 1959);
        year.year = {.say = Say::kClear};
        store.apply(year);
        CHECK_FALSE(store.year().has_value());
    }
}

TEST_CASE("title card: the whole object null is nothing playing", "[title_card]") {
    nv::TitleStore store;
    nv::TitleUpdate first;
    first.title = set("Blue in Green");
    first.progress = progress(0, 337'000);
    store.apply(first);
    REQUIRE(store.known());

    nv::TitleUpdate none;
    none.clear_all = true;
    store.apply(none);
    CHECK_FALSE(store.known());
    CHECK_FALSE(store.progress().has_value());

    // Said again, it changes nothing a second time.
    const std::uint32_t revision = store.revision();
    store.apply(none);
    CHECK(store.revision() == revision);
}

TEST_CASE("title card: the position runs on the server's clock from where the progress was told", "[title_card][position]") {
    nv::TitleStore store;
    nv::TitleUpdate first;
    first.timestamp_us = 100 * kSecond;
    first.title = set("Blue in Green");
    first.progress = progress(20'000, 337'000);
    store.apply(first);
    CHECK(store.position_ms(100 * kSecond) == 20'000);
    CHECK(store.position_ms(105 * kSecond) == 25'000);

    // A later object that names only a word does not move the song: the progress is anchored where it
    // was said, not at the last object's time.
    nv::TitleUpdate retitled;
    retitled.timestamp_us = 130 * kSecond;
    retitled.title = set("Blue in Green (take 2)");
    store.apply(retitled);
    CHECK(store.position_ms(140 * kSecond) == 60'000);

    SECTION("it is held between nought and the duration") {
        CHECK(store.position_ms(50 * kSecond) == 0);
        CHECK(store.position_ms(1000 * kSecond) == 337'000);
    }

    SECTION("a speed other than one moves it faster or slower") {
        nv::TitleUpdate fast;
        fast.timestamp_us = 200 * kSecond;
        fast.progress = progress(0, 337'000, 2000);
        store.apply(fast);
        CHECK(store.position_ms(210 * kSecond) == 20'000);
        nv::TitleUpdate paused;
        paused.timestamp_us = 220 * kSecond;
        paused.progress = progress(10'000, 337'000, 0);
        store.apply(paused);
        CHECK(store.position_ms(900 * kSecond) == 10'000);
    }

    SECTION("no duration is no ceiling, and no length") {
        nv::TitleUpdate radio;
        radio.timestamp_us = 300 * kSecond;
        radio.progress = progress(0, 0);
        store.apply(radio);
        CHECK(store.position_ms(4000 * kSecond) == 3'700'000);
        CHECK_FALSE(store.length_ms().has_value());
    }

    SECTION("a negative figure from a server is held at nought") {
        nv::TitleUpdate odd;
        odd.timestamp_us = 300 * kSecond;
        odd.progress = progress(-5000, -1, -3);
        store.apply(odd);
        CHECK(store.position_ms(310 * kSecond) == 0);
        CHECK_FALSE(store.length_ms().has_value());
    }
}

TEST_CASE("title card: the revision moves when a word does, and not when the same words arrive again", "[title_card]") {
    nv::TitleStore store;
    nv::TitleUpdate update;
    update.title = set("Blue in Green");
    update.artist = set("Miles Davis");
    store.apply(update);
    const std::uint32_t first = store.revision();
    CHECK(first > 0);
    store.apply(update);
    CHECK(store.revision() == first);
    update.artist = set("Bill Evans");
    store.apply(update);
    CHECK(store.revision() == first + 1);
}

TEST_CASE("title card: a long text is cut at a character, not in the middle of one", "[title_card][limits]") {
    nv::TitleStore store;
    // Three bytes a character: 256 falls inside one, so the cut backs up to 255.
    std::string long_title;
    for (int i = 0; i < 100; ++i) {
        long_title += "\xE6\x97\xA5";
    }
    REQUIRE(long_title.size() == 300);
    // The update holds views, so the texts it points at are named: a temporary would be gone by apply().
    const std::string long_artist(500, 'a');
    nv::TitleUpdate update;
    update.title = set(long_title);
    update.artist = set(long_artist);
    store.apply(update);
    CHECK(store.title().size() == 255);
    CHECK(store.title() == long_title.substr(0, 255));
    CHECK(store.artist().size() == nv::TitleStore::kArtistBytes);

    // A text that fits is kept as it is, up to the byte.
    const std::string fits(nv::TitleStore::kTitleBytes, 'b');
    update.title = set(fits);
    store.apply(update);
    CHECK(store.title().size() == nv::TitleStore::kTitleBytes);
}

TEST_CASE("title card: the strings keep their capacity, so applying an update allocates nothing", "[title_card][limits]") {
    nv::TitleStore store;
    const char* const title_at = store.title().data();
    const char* const artist_at = store.artist().data();
    const char* const album_at = store.album().data();
    CHECK(store.title().capacity() >= nv::TitleStore::kTitleBytes);
    CHECK(store.artist().capacity() >= nv::TitleStore::kArtistBytes);
    CHECK(store.album().capacity() >= nv::TitleStore::kAlbumBytes);

    const std::string long_title(1000, 't');
    const std::string long_artist(1000, 'a');
    const std::string long_album(1000, 'l');
    nv::TitleUpdate update;
    update.title = set(long_title);
    update.artist = set(long_artist);
    update.album = set(long_album);
    store.apply(update);
    update.title = {.say = Say::kClear};
    store.apply(update);
    update.title = set("Short");
    store.apply(update);
    CHECK(store.title().data() == title_at);
    CHECK(store.artist().data() == artist_at);
    CHECK(store.album().data() == album_at);
}
