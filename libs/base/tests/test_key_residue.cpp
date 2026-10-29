// What a private key leaves behind in memory the library has freed.
//
// credentials.cpp wipes the buffers that hold a key (credentials.hpp promises it of the key it
// hands back, and the file and environment readers wipe their own copies), so that a key the
// operator supplied does not sit in freed heap after signing. Wiping the buffers the code allocates
// is not the same as leaving no copy: a stream keeps a buffer of its own, filled with the file and
// freed, unwiped, with the stream. This binary finds such copies. It replaces the global allocation
// functions so that every block carries its size and, while a test is watching, is searched for the
// key on its way to being freed. It is a binary of its own (CMakeLists.txt) because the replacement
// is global.
//
// The bytes searched for are the scalar of RFC 6979 A.2.5, a published test key, in each form a
// key file can take.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "crypto_test_support.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/pem.hpp"
#include "platform/process.hpp"

namespace {

namespace crypto = iclforge::base::crypto;
using iclforge::test::crypto::from_hex;

// ---- the allocation hook ---------------------------------------------------------------------

constexpr std::size_t kHeaderBytes = 32;  // keeps the 16-byte alignment operator new owes

struct Needle {
    std::string name;
    std::vector<std::byte> bytes;
};

std::atomic<bool> g_watching{false};
std::atomic<int> g_hits{0};
// Written only while no watching is in progress, read while it is.
std::vector<Needle>* g_needles = nullptr;
thread_local bool t_inside_hook = false;

bool contains(const unsigned char* block, std::size_t size, const std::vector<std::byte>& needle) {
    if (needle.empty() || size < needle.size()) {
        return false;
    }
    for (std::size_t at = 0; at + needle.size() <= size; ++at) {
        if (std::memcmp(block + at, needle.data(), needle.size()) == 0) {
            return true;
        }
    }
    return false;
}

void inspect(const unsigned char* block, std::size_t size) noexcept {
    if (!g_watching.load(std::memory_order_relaxed) || t_inside_hook || g_needles == nullptr) {
        return;
    }
    t_inside_hook = true;
    for (const Needle& needle : *g_needles) {
        if (contains(block, size, needle.bytes)) {
            g_hits.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    t_inside_hook = false;
}

// Watches for the needles while it lives, and counts the freed blocks that held one.
class Watch {
   public:
    explicit Watch(std::vector<Needle>& needles) {
        g_needles = &needles;
        g_hits = 0;
        g_watching = true;
    }
    ~Watch() {
        g_watching = false;
        g_needles = nullptr;
    }
    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;

    [[nodiscard]] static int hits() { return g_hits.load(); }
};

}  // namespace

void* operator new(std::size_t size) {
    if (size > static_cast<std::size_t>(PTRDIFF_MAX) - kHeaderBytes) {
        throw std::bad_alloc();
    }
    void* raw = std::malloc(size + kHeaderBytes);
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    *static_cast<std::size_t*>(raw) = size;
    return static_cast<unsigned char*>(raw) + kHeaderBytes;
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

void operator delete(void* block) noexcept {
    if (block == nullptr) {
        return;
    }
    unsigned char* raw = static_cast<unsigned char*>(block) - kHeaderBytes;
    inspect(static_cast<const unsigned char*>(block), *reinterpret_cast<std::size_t*>(raw));
    std::free(raw);
}

void operator delete(void* block, std::size_t) noexcept {
    operator delete(block);
}

void operator delete[](void* block) noexcept {
    operator delete(block);
}

void operator delete[](void* block, std::size_t) noexcept {
    operator delete(block);
}

namespace {

// ---- the key in each form a key file takes ----------------------------------------------------

constexpr const char* kScalarHex =
    "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721";
constexpr const char* kPkcs8Pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgya+p2EW6dRZrXCFX\n"
    "Z7HWk05Qw9s26JsSe4piKxIPZyGhRANCAARg/tS6JVqdMclh63TGNW1owEm4kjth\n"
    "+mzmaWIuYPKftnkD/hAIuLyZpBrp6VYovGTy8bIMLX6fUXejwpTURiKZ\n"
    "-----END PRIVATE KEY-----\n";
constexpr const char* kSec1Pem =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEIMmvqdhFunUWa1whV2ex1pNOUMPbNuibEnuKYisSD2choAoGCCqGSM49\n"
    "AwEHoUQDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7Yfps5mliLmDyn7Z5A/4QCLi8\n"
    "maQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ==\n"
    "-----END EC PRIVATE KEY-----\n";

std::vector<std::byte> to_bytes(std::string_view text) {
    std::vector<std::byte> out;
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

// What a stray copy of the key looks like in each form: the scalar itself, the base64 text of the
// DER that holds it, and the base64 of that text, which is the CI form.
std::vector<Needle> needles_for_the_key() {
    std::vector<Needle> needles;
    needles.push_back({"the raw scalar", from_hex(kScalarHex)});
    needles.push_back({"PKCS#8 text", to_bytes("MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgya+p2EW6")});
    needles.push_back({"SEC 1 text", to_bytes("MHcCAQEEIMmvqdhFunUWa1whV2ex1pNOUMPbNuibEnuKYisSD2cho")});
    // base64("-----BEGIN PRIVATE KEY-----") and base64("-----BEGIN EC PRIVATE KEY-----"): the
    // first line of either file, as the CI form carries it.
    needles.push_back({"base64 of a PEM file", to_bytes("LS0tLS1CRUdJTiBQUklWQVRFIEtFWS0tLS0t")});
    needles.push_back({"base64 of a SEC 1 PEM file", to_bytes("LS0tLS1CRUdJTiBFQyBQUklWQVRFIEtFWS0tLS0t")});
    return needles;
}

std::filesystem::path scratch_dir() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                         ("key_residue_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

std::string write_file(const std::string& name, std::span<const std::byte> bytes) {
    const std::filesystem::path path = scratch_dir() / name;
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path.string();
}

}  // namespace

TEST_CASE("the instrument sees a key left in a freed block", "[crypto][credentials][residue]") {
    // The control: a copy of the key freed without a wipe is counted. Without it the cases below
    // would pass whatever the hook did.
    auto needles = needles_for_the_key();
    const Watch watch{needles};
    auto* block = new unsigned char[96];
    std::memset(block, 0x5A, 96);
    std::memcpy(block + 17, needles.front().bytes.data(), needles.front().bytes.size());
    delete[] block;
    CHECK(Watch::hits() == 1);

    // A wiped block is not.
    block = new unsigned char[96];
    std::memset(block, 0x5A, 96);
    delete[] block;
    CHECK(Watch::hits() == 1);
}

TEST_CASE("a key file leaves no copy of the key in freed memory", "[crypto][credentials][residue]") {
    const std::string pem = kPkcs8Pem;
    const std::string sec1 = kSec1Pem;
    const auto scalar = from_hex(kScalarHex);

    struct Form {
        const char* name;
        std::vector<std::byte> file;
    };
    std::vector<Form> forms;
    forms.push_back({"a bare 32-byte scalar", scalar});
    forms.push_back({"PKCS#8 PEM", to_bytes(pem)});
    forms.push_back({"SEC 1 PEM", to_bytes(sec1)});
    forms.push_back({"base64 of a PEM file", to_bytes(crypto::base64_encode(to_bytes(pem)))});

    for (const Form& form : forms) {
        DYNAMIC_SECTION(form.name) {
            const std::string path = write_file("key.file", form.file);
            auto needles = needles_for_the_key();
            std::optional<crypto::P256PrivateKey> key;
            int leaks = 0;
            {
                const Watch watch{needles};
                auto loaded = crypto::load_content_key(path);
                if (loaded.has_value()) {
                    key = *loaded;
                }
                leaks = Watch::hits();
            }
            REQUIRE(key.has_value());
            CHECK(std::equal(scalar.begin(), scalar.end(), key->scalar().begin()));
            CHECK(leaks == 0);
        }
    }
}

TEST_CASE("a key in the environment leaves no copy of the key in freed memory",
          "[crypto][credentials][residue]") {
    const std::string pem = kPkcs8Pem;
    iclforge::test::platform::set_environment("ICLFORGE_CONTENT_KEY", pem);
    auto needles = needles_for_the_key();
    int leaks = 0;
    bool loaded = false;
    {
        const Watch watch{needles};
        const auto key = crypto::load_content_key("");
        loaded = key.has_value();
        leaks = Watch::hits();
    }
    iclforge::test::platform::unset_environment("ICLFORGE_CONTENT_KEY");
    CHECK(loaded);
    CHECK(leaks == 0);
}
