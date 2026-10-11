#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/sendspin/json.hpp"

// iclforge::sendspin::json, the reader and writer every Sendspin message goes
// through. The reader is the first thing a peer's bytes reach, on the server and
// on a sink, so most of what is here is about refusing: every malformed shape a
// careless or hostile peer could send, and the limits that keep its cost bounded.

namespace {

using iclforge::sendspin::json::count_tokens;
using iclforge::sendspin::json::Document;
using iclforge::sendspin::json::Error;
using iclforge::sendspin::json::Limits;
using iclforge::sendspin::json::Member;
using iclforge::sendspin::json::ParseResult;
using iclforge::sendspin::json::Token;
using iclforge::sendspin::json::TokenStorage;
using iclforge::sendspin::json::Type;
using iclforge::sendspin::json::Value;
using iclforge::sendspin::json::Writer;

// "[1,1,...]" of `values` elements: values + 1 tokens.
std::string array_of(std::size_t values) {
    std::string text = "[";
    for (std::size_t i = 0; i < values; ++i) {
        text += i == 0 ? "1" : ",1";
    }
    return text + "]";
}

// {"k0":1,"k1":1,...} of `members` members: 2 * members + 1 tokens.
std::string object_of(std::size_t members) {
    std::string text = "{";
    for (std::size_t i = 0; i < members; ++i) {
        text += (i == 0 ? "\"k" : ",\"k") + std::to_string(i) + "\":1";
    }
    return text + "}";
}

// A document with its own storage, for tests that only need the result.
struct Parsed {
    std::vector<Token> tokens;
    Document doc;
    ParseResult result;

    explicit Parsed(std::string_view text, const Limits& limits = {}) {
        result = doc.parse(text, tokens, 100000, limits);
    }
};

Error error_of(std::string_view text, const Limits& limits = {}) {
    return Parsed(text, limits).result.error;
}

}  // namespace

TEST_CASE("json: scalars at the top level", "[sendspin][json]") {
    CHECK(Parsed("null").doc.root().is_null());
    CHECK(Parsed("true").doc.root().as_bool() == true);
    CHECK(Parsed("false").doc.root().as_bool() == false);
    CHECK(Parsed("42").doc.root().as_int() == 42);
    CHECK(Parsed("-7").doc.root().as_int() == -7);
    CHECK(Parsed("\"hi\"").doc.root().as_string() == "hi");
    CHECK(Parsed("  \t\r\n 1 \n").doc.root().as_int() == 1);
}

TEST_CASE("json: a Sendspin message reads member by member", "[sendspin][json]") {
    const Parsed parsed(R"({
        "type": "stream/start",
        "payload": {
            "server_transmitted": 1234567890,
            "player": {"codec": "opus", "sample_rate": 48000, "channels": 2, "bit_depth": 16},
            "_iclforge_player": {"data_type": "eac3", "sample_rate": 48000}
        }
    })");
    REQUIRE(parsed.result);
    const Value root = parsed.doc.root();
    CHECK(root["type"].equals("stream/start"));
    const Value payload = root["payload"];
    CHECK(payload["server_transmitted"].as_int() == 1234567890);
    CHECK(payload["player"]["codec"].equals("opus"));
    CHECK(payload["player"]["sample_rate"].as_int() == 48000);
    CHECK(payload["_iclforge_player"]["data_type"].equals("eac3"));
    CHECK(payload.size() == 3);
}

TEST_CASE("json: missing members and wrong types read as absent", "[sendspin][json]") {
    const Parsed parsed(R"({"a": {"b": [1, 2, 3]}, "s": "text"})");
    REQUIRE(parsed.result);
    const Value root = parsed.doc.root();
    CHECK_FALSE(root["missing"].exists());
    CHECK_FALSE(root["missing"]["deeper"].exists());
    CHECK_FALSE(root["s"]["not an object"].exists());
    CHECK_FALSE(root["a"]["b"].at(3).exists());
    CHECK_FALSE(root["s"].as_int().has_value());
    CHECK_FALSE(root["a"].as_string().has_value());
    CHECK_FALSE(root["a"]["b"].equals("1"));
    CHECK(root["a"]["b"].at(2).as_int() == 3);
    CHECK(root["s"].size() == 0);
}

TEST_CASE("json: arrays and objects iterate in document order", "[sendspin][json]") {
    const Parsed parsed(R"({"roles": ["_iclforge_player@v1", "player@v1"], "n": {"x": 1, "y": [2]}})");
    REQUIRE(parsed.result);
    std::vector<std::string> roles;
    for (const Value role : parsed.doc.root()["roles"].elements()) {
        roles.push_back(role.as_string().value_or(""));
    }
    CHECK(roles == std::vector<std::string>{"_iclforge_player@v1", "player@v1"});

    std::vector<std::string> keys;
    for (const Member member : parsed.doc.root()["n"].members()) {
        keys.push_back(member.key.as_string().value_or(""));
    }
    CHECK(keys == std::vector<std::string>{"x", "y"});
    CHECK(parsed.doc.root()["n"]["y"].source() == "[2]");
}

TEST_CASE("json: empty containers", "[sendspin][json]") {
    const Parsed parsed(R"({"a": [], "o": {}, "nested": [[], {}]})");
    REQUIRE(parsed.result);
    CHECK(parsed.doc.root()["a"].is_array());
    CHECK(parsed.doc.root()["a"].size() == 0);
    CHECK(parsed.doc.root()["o"].is_object());
    CHECK(parsed.doc.root()["o"].size() == 0);
    CHECK(parsed.doc.root()["nested"].size() == 2);
    // The message-2 Noise payload is these two bytes exactly.
    CHECK(Parsed("{}").doc.root().is_object());
}

// The texts with \u escapes are ordinary literals with doubled backslashes: MSVC
// expands \u inside raw string literals, which would hand the reader the
// characters instead of the escapes.
TEST_CASE("json: string escapes decode", "[sendspin][json]") {
    const Parsed parsed(
        "[\"a\\\"b\", \"\\\\\", \"\\/\", \"\\b\\f\\n\\r\\t\", \"\\u0041\\u00e9\\u20ac\", "
        "\"\\ud83c\\udfb5\"]");
    REQUIRE(parsed.result);
    const Value root = parsed.doc.root();
    CHECK(root.at(4).source() == "\"\\u0041\\u00e9\\u20ac\"");
    CHECK(root.at(0).as_string() == "a\"b");
    CHECK(root.at(1).as_string() == "\\");
    CHECK(root.at(2).as_string() == "/");
    CHECK(root.at(3).as_string() == "\b\f\n\r\t");
    CHECK(root.at(4).as_string() == "A\xC3\xA9\xE2\x82\xAC");
    CHECK(root.at(5).as_string() == "\xF0\x9F\x8E\xB5");
    CHECK(root.at(4).equals("A\xC3\xA9\xE2\x82\xAC"));
    CHECK_FALSE(root.at(4).equals("A\xC3\xA9"));
    CHECK_FALSE(root.at(4).equals("A\xC3\xA9\xE2\x82\xAC!"));
    CHECK_FALSE(root.at(4).raw().has_value());
}

TEST_CASE("json: unpaired surrogate escapes decode to U+FFFD", "[sendspin][json]") {
    const Parsed parsed("[\"\\ud800\", \"\\udc00x\", \"\\ud800\\u0041\"]");
    REQUIRE(parsed.result);
    CHECK(parsed.doc.root().at(0).as_string() == "\xEF\xBF\xBD");
    CHECK(parsed.doc.root().at(1).as_string() == "\xEF\xBF\xBDx");
    CHECK(parsed.doc.root().at(2).as_string() == "\xEF\xBF\xBD" "A");
}

TEST_CASE("json: an unescaped string is a view into the text", "[sendspin][json]") {
    const std::string text = R"({"k":"plain value"})";
    const Parsed parsed(text);
    REQUIRE(parsed.result);
    const std::optional<std::string_view> raw = parsed.doc.root()["k"].raw();
    REQUIRE(raw.has_value());
    CHECK(*raw == "plain value");
    CHECK(raw->data() >= text.data());
    CHECK(raw->data() < text.data() + text.size());
}

TEST_CASE("json: raw UTF-8 in strings is accepted when well formed", "[sendspin][json]") {
    CHECK(error_of("\"caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xB5\"") == Error::kNone);
    // Overlong forms, encoded surrogates, above U+10FFFF, stray continuation
    // bytes and a truncated sequence.
    CHECK(error_of("\"\xC0\xAF\"") == Error::kBadString);
    CHECK(error_of("\"\xE0\x80\xAF\"") == Error::kBadString);
    CHECK(error_of("\"\xED\xA0\x80\"") == Error::kBadString);
    CHECK(error_of("\"\xF4\x90\x80\x80\"") == Error::kBadString);
    CHECK(error_of("\"\x80\"") == Error::kBadString);
    CHECK(error_of("\"\xE2\x82\"") == Error::kBadString);
    CHECK(error_of("\"\xFF\"") == Error::kBadString);
}

TEST_CASE("json: control characters and bad escapes are refused", "[sendspin][json]") {
    CHECK(error_of("\"a\nb\"") == Error::kBadString);
    CHECK(error_of(std::string_view("\"a\0b\"", 5)) == Error::kBadString);
    CHECK(error_of("\"\\x\"") == Error::kBadString);
    CHECK(error_of("\"\\u12G4\"") == Error::kBadString);
    CHECK(error_of("\"\\u12\"") == Error::kBadString);
    CHECK(error_of("\"\\u12") == Error::kTruncated);
    CHECK(error_of("\"\\'\"") == Error::kBadString);
}

TEST_CASE("json: integers read exactly across the int64 range", "[sendspin][json]") {
    CHECK(Parsed("9223372036854775807").doc.root().as_int() ==
          std::numeric_limits<std::int64_t>::max());
    CHECK(Parsed("-9223372036854775808").doc.root().as_int() ==
          std::numeric_limits<std::int64_t>::min());
    CHECK_FALSE(Parsed("9223372036854775808").doc.root().as_int().has_value());
    CHECK_FALSE(Parsed("-9223372036854775809").doc.root().as_int().has_value());
    CHECK_FALSE(Parsed("1.0").doc.root().as_int().has_value());
    CHECK_FALSE(Parsed("1e3").doc.root().as_int().has_value());
    CHECK(Parsed("0").doc.root().as_int() == 0);
    CHECK(Parsed("-0").doc.root().as_int() == 0);
}

TEST_CASE("json: numbers read as double", "[sendspin][json]") {
    const auto dbl = [](std::string_view text) { return Parsed(text).doc.root().as_double(); };
    CHECK(dbl("0.5") == 0.5);
    CHECK(dbl("-3.25") == -3.25);
    CHECK(dbl("1e3") == 1000.0);
    CHECK(dbl("1E-3") == 0.001);
    CHECK(dbl("2.5e+2") == 250.0);
    CHECK(dbl("-12.34") == -12.34);
    CHECK(dbl("0.1") == 0.1);
    CHECK(dbl("123456789012345") == 123456789012345.0);
    CHECK(dbl("48000") == 48000.0);
    CHECK(dbl("0.000") == 0.0);
    CHECK(std::signbit(*dbl("-0.0")));
    // Past the exact path: close.
    const std::optional<double> large = dbl("1.5e308");
    REQUIRE(large.has_value());
    CHECK(std::isfinite(*large));
    CHECK(std::fabs(*large / 1.5e308 - 1.0) < 1e-12);
    const std::optional<double> small = dbl("2.5e-300");
    REQUIRE(small.has_value());
    CHECK(std::fabs(*small / 2.5e-300 - 1.0) < 1e-12);
    const std::optional<double> long_digits = dbl("3.14159265358979323846264338327950288");
    REQUIRE(long_digits.has_value());
    CHECK(std::fabs(*long_digits - 3.141592653589793) < 1e-14);
    CHECK_FALSE(dbl("1e999").has_value());
    CHECK(dbl("1e-999") == 0.0);
}

TEST_CASE("json: malformed numbers are refused", "[sendspin][json]") {
    CHECK(error_of("01") == Error::kBadNumber);
    CHECK(error_of("-") == Error::kTruncated);
    CHECK(error_of("-a") == Error::kBadNumber);
    CHECK(error_of("1.") == Error::kTruncated);
    CHECK(error_of("1.e5") == Error::kBadNumber);
    CHECK(error_of("1e") == Error::kTruncated);
    CHECK(error_of("1e+") == Error::kTruncated);
    CHECK(error_of("[1e+]") == Error::kBadNumber);
    CHECK(error_of("+1") == Error::kSyntax);
    CHECK(error_of(".5") == Error::kSyntax);
    CHECK(error_of("[01]") == Error::kBadNumber);
}

TEST_CASE("json: syntax errors say where", "[sendspin][json]") {
    CHECK(error_of("") == Error::kEmpty);
    CHECK(error_of("   ") == Error::kEmpty);
    CHECK(error_of("[1,]") == Error::kSyntax);
    CHECK(error_of("{\"a\":1,}") == Error::kSyntax);
    CHECK(error_of("[1 2]") == Error::kSyntax);
    CHECK(error_of("{\"a\" 1}") == Error::kSyntax);
    CHECK(error_of("{1:2}") == Error::kSyntax);
    CHECK(error_of("[}") == Error::kSyntax);
    CHECK(error_of("{]") == Error::kSyntax);
    CHECK(error_of("[1}") == Error::kSyntax);
    CHECK(error_of("tru") == Error::kTruncated);
    CHECK(error_of("trux") == Error::kSyntax);
    CHECK(error_of("nul") == Error::kTruncated);
    CHECK(error_of("[") == Error::kTruncated);
    CHECK(error_of("{\"a\":") == Error::kTruncated);
    CHECK(error_of("\"open") == Error::kTruncated);
    CHECK(error_of("1 2") == Error::kTrailing);
    CHECK(error_of("{} {}") == Error::kTrailing);
    CHECK(error_of("nullx") == Error::kTrailing);
    CHECK(error_of("'single'") == Error::kSyntax);

    const Parsed parsed("[1, 2, @]");
    CHECK(parsed.result.error == Error::kSyntax);
    CHECK(parsed.result.offset == 7);
    CHECK(parsed.doc.empty());
}

TEST_CASE("json: duplicate keys are refused, escaped spellings included", "[sendspin][json]") {
    CHECK(error_of(R"({"a":1,"a":2})") == Error::kDuplicateKey);
    CHECK(error_of("{\"a\":1,\"\\u0061\":2}") == Error::kDuplicateKey);
    CHECK(error_of("{\"\\u0061\":1,\"a\":2}") == Error::kDuplicateKey);
    CHECK(error_of("{\"\\u0061\":1,\"\\u0062\":2}") == Error::kNone);
    CHECK(error_of(R"({"x":{"a":1,"b":2,"a":3}})") == Error::kDuplicateKey);
    CHECK(error_of(R"({"a":1,"b":{"a":2}})") == Error::kNone);
    CHECK(error_of(R"([{"a":1},{"a":2}])") == Error::kNone);
    CHECK(error_of(R"({"ab":1,"a":2,"b":3})") == Error::kNone);
}

TEST_CASE("json: depth is limited without recursion", "[sendspin][json]") {
    const std::string deep = std::string(32, '[') + std::string(32, ']');
    CHECK(error_of(deep) == Error::kNone);
    CHECK(error_of(std::string(33, '[') + std::string(33, ']')) == Error::kDepth);
    CHECK(error_of(std::string(10, '['), Limits{.max_depth = 4, .max_members = 1024}) ==
          Error::kDepth);
    // A request above the hard cap is held to it.
    const Limits generous{.max_depth = 1000, .max_members = 1024};
    const std::string at_cap = std::string(64, '[') + std::string(64, ']');
    CHECK(error_of(at_cap, generous) == Error::kNone);
    CHECK(error_of(std::string(65, '[') + std::string(65, ']'), generous) == Error::kDepth);
    // A hostile opening run costs nothing but the scan.
    CHECK(error_of(std::string(100000, '[')) == Error::kDepth);
}

TEST_CASE("json: token storage is never exceeded", "[sendspin][json]") {
    std::array<Token, 4> storage{};
    Document doc;
    CHECK(doc.parse("[1,2,3]", storage).error == Error::kNone);
    CHECK(doc.token_count() == 4);
    CHECK(doc.parse("[1,2,3,4]", storage).error == Error::kTooManyTokens);
    CHECK(doc.empty());
    CHECK(doc.parse(R"({"a":1,"b":2})", storage).error == Error::kTooManyTokens);

    std::vector<Token> grown;
    CHECK(doc.parse("[1,2,3,4,5,6,7,8]", grown, 5).error == Error::kTooManyTokens);
    CHECK(doc.parse("[1,2,3,4,5,6,7,8]", grown, 100).error == Error::kNone);
    CHECK(grown.size() <= 9);
}

TEST_CASE("json: count_tokens finds the tokens parse stores", "[sendspin][json]") {
    for (const std::string_view text :
         {"0", "\"s\"", "[]", "{}", "[[],{}]", "[[[[]]]]", R"({"a":{"b":{"c":[1,[2,[3]]]}}})",
          R"({"type":"server/time","payload":{"x":1,"y":2.5,"z":[true,false,null]}})",
          "  [ 1 , 2 ]  "}) {
        INFO(text);
        const Parsed parsed(text);
        REQUIRE(parsed.result);
        const auto counted = count_tokens(text, 100000);
        CHECK(counted.result);
        CHECK(counted.tokens == parsed.doc.token_count());
    }
    for (const std::size_t n : {0U, 1U, 63U, 64U, 65U, 500U}) {
        CHECK(count_tokens(array_of(n), 100000).tokens == n + 1);
        CHECK(count_tokens(object_of(n), 100000).tokens == (2 * n) + 1);
    }
}

TEST_CASE("json: count_tokens refuses what parse refuses, in the same place", "[sendspin][json]") {
    for (const std::string_view text :
         {"", "   ", "[", "[1,", "[1 2]", "[1,]", "{\"a\"}", "{\"a\":}", "{1:2}", "01", "1.",
          "\"\\q\"", "tru", "[1] 2", "[\"\x01\"]", "\"\xff\""}) {
        INFO(text);
        const Parsed parsed(text);
        const auto counted = count_tokens(text, 100000);
        REQUIRE_FALSE(parsed.result);
        CHECK(counted.result.error == parsed.result.error);
        CHECK(counted.result.offset == parsed.result.offset);
        CHECK(counted.tokens == 0);
    }
    const Limits shallow{.max_depth = 4, .max_members = 1024};
    CHECK(count_tokens("[[[[[]]]]]", 100, shallow).result.error == Error::kDepth);
    const Limits narrow{.max_depth = 32, .max_members = 2};
    CHECK(count_tokens(R"({"a":1,"b":2,"c":3})", 100, narrow).result.error ==
          Error::kTooManyMembers);
    // The cap is the tokens it counts, however long the text is.
    CHECK(count_tokens("[1,2,3,4]", 5).tokens == 5);
    CHECK(count_tokens("[1,2,3,4,5]", 5).result.error == Error::kTooManyTokens);
    CHECK(count_tokens(array_of(100000), 100).result.error == Error::kTooManyTokens);
    // Duplicate keys are the one refusal it leaves to parse(): they need the tokens.
    CHECK(count_tokens(R"({"a":1,"a":2})", 100).tokens == 5);
    CHECK(error_of(R"({"a":1,"a":2})") == Error::kDuplicateKey);
}

TEST_CASE("json: token storage keeps an ordinary message in the object", "[sendspin][json]") {
    TokenStorage storage;
    Document doc;
    const std::string fits = array_of(TokenStorage::kInlineTokens - 1);
    REQUIRE(doc.parse(fits, storage, 4096));
    CHECK(doc.token_count() == TokenStorage::kInlineTokens);
    CHECK(storage.heap_tokens() == 0);
    CHECK(doc.root().size() == TokenStorage::kInlineTokens - 1);

    // One more value needs a block, of exactly the tokens the text holds.
    const std::string spills = array_of(TokenStorage::kInlineTokens);
    REQUIRE(doc.parse(spills, storage, 4096));
    CHECK(doc.token_count() == TokenStorage::kInlineTokens + 1);
    CHECK(storage.heap_tokens() == TokenStorage::kInlineTokens + 1);
    CHECK(doc.root().at(TokenStorage::kInlineTokens - 1).as_int() == 1);

    // Reusing the storage for a small message hands the block back.
    REQUIRE(doc.parse("[1]", storage, 4096));
    CHECK(storage.heap_tokens() == 0);
    CHECK(doc.root().at(0).as_int() == 1);
}

TEST_CASE("json: token storage is sized by the values, not the text", "[sendspin][json]") {
    // 70 values spread over 8 KB: the vector overload would reserve 4,097 tokens.
    std::string text = "[1";
    for (int i = 0; i < 69; ++i) {
        text += std::string(120, ' ') + ",1";
    }
    text += "]";
    REQUIRE(text.size() > 8000);
    TokenStorage storage;
    Document doc;
    REQUIRE(doc.parse(text, storage, 4096));
    CHECK(doc.token_count() == 71);
    CHECK(storage.heap_tokens() == 71);
    CHECK(doc.root().size() == 70);
}

TEST_CASE("json: token storage refuses before it allocates", "[sendspin][json]") {
    TokenStorage storage;
    Document doc;
    // A run of openers is refused at the depth limit, inside the object.
    CHECK(doc.parse(std::string(4000, '['), storage, 4096).error == Error::kDepth);
    CHECK(storage.heap_tokens() == 0);
    // More values than the cap: counted, refused, no block.
    CHECK(doc.parse(array_of(500), storage, 100).error == Error::kTooManyTokens);
    CHECK(storage.heap_tokens() == 0);
    // Malformed after the object's tokens run out: no block.
    CHECK(doc.parse(array_of(100) + "]", storage, 4096).error == Error::kTrailing);
    CHECK(storage.heap_tokens() == 0);
    std::string trailing_comma = array_of(100);
    trailing_comma.insert(trailing_comma.size() - 1, ",");
    CHECK(doc.parse(trailing_comma, storage, 4096).error == Error::kSyntax);
    CHECK(storage.heap_tokens() == 0);
    CHECK(doc.empty());
    // A cap below what the object holds is the cap.
    CHECK(doc.parse("[1,2,3,4]", storage, 3).error == Error::kTooManyTokens);
    CHECK(doc.parse("[1,2]", storage, 3).error == Error::kNone);
    // What only the tokens show: a repeated key past the object's capacity.
    std::string repeated = object_of(40);
    repeated.back() = ',';
    repeated += "\"k0\":2}";
    CHECK(doc.parse(repeated, storage, 4096).error == Error::kDuplicateKey);
    CHECK(doc.empty());
}

TEST_CASE("json: token storage and a vector read the same document", "[sendspin][json]") {
    for (std::size_t n = 0; n < 140; n += 7) {
        for (const std::string& text : {array_of(n), object_of(n)}) {
            INFO(text);
            const Parsed expected(text);
            TokenStorage storage;
            Document doc;
            const ParseResult got = doc.parse(text, storage, 100000);
            CHECK(got.error == expected.result.error);
            CHECK(got.offset == expected.result.offset);
            CHECK(doc.token_count() == expected.doc.token_count());
            CHECK(doc.root().source() == expected.doc.root().source());
            CHECK(doc.root().size() == expected.doc.root().size());
        }
    }
}

TEST_CASE("json: the storage bound n/2 + 1 holds for dense texts", "[sendspin][json]") {
    for (const std::string_view text :
         {"0", "[]", "[0]", "[0,0]", "[[],[]]", "{\"\":0}", "[0,0,0,0,0]", "[[[[]]]]"}) {
        std::vector<Token> storage;
        Document doc;
        INFO(text);
        CHECK(doc.parse(text, storage, std::numeric_limits<std::size_t>::max()).error ==
              Error::kNone);
    }
}

TEST_CASE("json: members per object are limited", "[sendspin][json]") {
    std::string text = "{";
    for (int i = 0; i < 20; ++i) {
        text += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":0";
    }
    text += "}";
    CHECK(error_of(text, Limits{.max_depth = 32, .max_members = 20}) == Error::kNone);
    CHECK(error_of(text, Limits{.max_depth = 32, .max_members = 19}) == Error::kTooManyMembers);
}

TEST_CASE("json: writer produces what the reader reads back", "[sendspin][json]") {
    std::string out;
    Writer w(out);
    w.begin_object()
        .member("type", "client/hello")
        .key("payload")
        .begin_object()
        .member("name", "Kitchen \"sink\"")
        .key("supported_roles")
        .begin_array()
        .string("_iclforge_player@v1")
        .string("player@v1")
        .end_array()
        .member("available", true)
        .member("volume", 100)
        .key("nothing")
        .null()
        .key("empty")
        .begin_array()
        .end_array()
        .key("peak_db")
        .number(-12.25, 1)
        .end_object()
        .end_object();
    CHECK(w.complete());
    CHECK(out ==
          R"({"type":"client/hello","payload":{"name":"Kitchen \"sink\"","supported_roles":["_iclforge_player@v1","player@v1"],"available":true,"volume":100,"nothing":null,"empty":[],"peak_db":-12.3}})");

    const Parsed parsed(out);
    REQUIRE(parsed.result);
    CHECK(parsed.doc.root()["payload"]["name"].as_string() == "Kitchen \"sink\"");
    CHECK(parsed.doc.root()["payload"]["supported_roles"].size() == 2);
}

TEST_CASE("json: writer escapes control characters and repairs invalid UTF-8",
          "[sendspin][json]") {
    std::string out;
    Writer(out).string(std::string_view("a\x01\x1F\t\\/\"\xC3\xA9\xFF", 10));
    CHECK(out == "\"a\\u0001\\u001f\\t\\\\/\\\"\xC3\xA9\xEF\xBF\xBD\"");
    const Parsed parsed(out);
    REQUIRE(parsed.result);
    CHECK(parsed.doc.root().as_string() == "a\x01\x1F\t\\/\"\xC3\xA9\xEF\xBF\xBD");
}

TEST_CASE("json: writer numbers", "[sendspin][json]") {
    const auto write = [](double value, int decimals) {
        std::string out;
        Writer(out).number(value, decimals);
        return out;
    };
    CHECK(write(0.0, 3) == "0");
    CHECK(write(1.5, 3) == "1.5");
    CHECK(write(-0.25, 1) == "-0.3");
    CHECK(write(-0.04, 1) == "0");
    CHECK(write(80.0, 1) == "80");
    CHECK(write(0.001, 3) == "0.001");
    CHECK(write(123.456789, 4) == "123.4568");
    CHECK(write(-24.0, 0) == "-24");
    CHECK(write(std::numeric_limits<double>::infinity(), 2) == "null");
    CHECK(write(std::numeric_limits<double>::quiet_NaN(), 2) == "null");
    CHECK(write(1e300, 2) == "null");

    std::string out;
    Writer(out).begin_array().integer(std::numeric_limits<std::int64_t>::min()).unsigned_integer(
        std::numeric_limits<std::uint64_t>::max()).end_array();
    CHECK(out == "[-9223372036854775808,18446744073709551615]");
}
