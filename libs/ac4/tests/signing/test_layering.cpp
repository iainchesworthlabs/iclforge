// Signing is a tool's step around the codec, not part of it (planning/ac4-content-signing.md,
// "Module layout"). Three properties keep it that way, and each is checked here by reading the
// sources, because the compiler would accept a violation of any of them:
//
//   1. No encoder, decoder or table-of-contents source includes a signing header or
//   iclforge::base's
//      cryptography.
//   2. The minimum-footprint archive (libs/ac4/minimal.cmake) lists no signing source.
//   3. The signing sources are listed in the full library's CMakeLists.txt, so a change that
//      forgot to build them is caught by this case before it is caught by a link error.
//
// ICLFORGE_AC4_SOURCE_DIR is the libs/ac4 directory (libs/ac4/tests/CMakeLists.txt).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The `#include` lines of a source, which are the only place a dependency is spelled.
std::vector<std::string> include_lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] != '#') {
            continue;
        }
        const std::size_t word = line.find_first_not_of(" \t", first + 1);
        if (word != std::string::npos && line.compare(word, 7, "include") == 0) {
            out.push_back(line);
        }
    }
    return out;
}

bool names_signing(const std::string& include_line) {
    return include_line.find("ac4/signing/") != std::string::npos ||
           include_line.find("base/crypto/") != std::string::npos ||
           include_line.find("\"signing/") != std::string::npos;
}

}  // namespace

TEST_CASE("no codec source includes signing or cryptography", "[ac4][signing][layering]") {
    const fs::path root = fs::path(ICLFORGE_AC4_SOURCE_DIR);
    REQUIRE(fs::is_directory(root / "src"));

    std::size_t scanned = 0;
    std::vector<std::string> offenders;
    for (const char* directory :
         {"src/core", "src/decoder", "src/encoder", "src/io", "src/meta", "src/oba", "src/iclforge",
          "include/iclforge/ac4/core", "include/iclforge/ac4/decoder",
          "include/iclforge/ac4/encoder", "include/iclforge/ac4/io"}) {
        const fs::path dir = root / directory;
        if (!fs::is_directory(dir)) {
            continue;
        }
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            const std::string extension = entry.path().extension().string();
            if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".hpp")) {
                continue;
            }
            ++scanned;
            for (const std::string& line : include_lines(read_text(entry.path()))) {
                if (names_signing(line)) {
                    offenders.push_back(entry.path().string() + ": " + line);
                }
            }
        }
    }
    // The scan saw the codec and not an empty directory.
    REQUIRE(scanned > 100);
    INFO("sources that include signing or cryptography:");
    for (const std::string& offender : offenders) {
        INFO(offender);
    }
    REQUIRE(offenders.empty());
}

TEST_CASE("the minimum-footprint archive lists no signing source", "[ac4][signing][layering]") {
    const fs::path root = fs::path(ICLFORGE_AC4_SOURCE_DIR);
    const std::string minimal = read_text(root / "minimal.cmake");
    REQUIRE(minimal.find("_ac4_decoder_sources") != std::string::npos);  // it is the file we mean
    // The source lists are variables; the archive's list names the decoder's, the meta, the
    // inspector's and the kernels'. Signing has a variable of its own that it must not name.
    REQUIRE(minimal.find("_ac4_signing_sources") == std::string::npos);
    REQUIRE(minimal.find("src/signing/") == std::string::npos);
}

TEST_CASE("the full library lists the signing sources", "[ac4][signing][layering]") {
    const fs::path root = fs::path(ICLFORGE_AC4_SOURCE_DIR);
    const std::string cmake = read_text(root / "CMakeLists.txt");
    for (const char* source : {"src/signing/carrier.cpp", "src/signing/content_signing.cpp"}) {
        INFO(source);
        REQUIRE(cmake.find(source) != std::string::npos);
        REQUIRE(fs::exists(root / source));
    }
    // And the variable reaches the full library's iclforge_add_library() call.
    const std::size_t full = cmake.find("${_ac4_signing_sources}");
    REQUIRE(full != std::string::npos);
}
