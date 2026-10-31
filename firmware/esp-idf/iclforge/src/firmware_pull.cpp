// Updates from GitHub releases. See ../include/iclforge/firmware_pull.hpp, planning/esp32-ota.md
// (O10) and libs/device/include/iclforge/firmware_update.hpp, which holds every rule that needs no
// ESP-IDF.

#include "iclforge/firmware_pull.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sdkconfig.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/error.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "iclforge/firmware.hpp"
#include "iclforge/firmware_update.hpp"

namespace iclforge {
namespace {

using update::Latest;
using update::State;
using update::TrustOutcome;
using update::UpdateStatus;

constexpr const char* kNamespace = "firmware";
constexpr const char* kKeyChannel = "fw_chan";  // "stable" or "beta"
constexpr const char* kKeySeqStable =
    "fw_seq_s";  // the highest index sequence accepted, per channel
constexpr const char* kKeySeqBeta = "fw_seq_b";
constexpr int kMaxRedirects = 3;
constexpr int kNetworkTimeoutMs = 15'000;

std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out += kDigits[static_cast<std::size_t>(b >> 4)];
        out += kDigits[static_cast<std::size_t>(b & 0x0F)];
    }
    return out;
}

bool unhex(std::string_view text, std::span<std::uint8_t> out) {
    if (text.size() != out.size() * 2) {
        return false;
    }
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
    };
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = nibble(text[2 * i]);
        const int low = nibble(text[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

std::optional<std::array<std::uint8_t, 32>> sha256(std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, 32> out{};
    std::size_t length = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, data.data(), data.size(), out.data(), out.size(),
                         &length) != PSA_SUCCESS ||
        length != out.size()) {
        return std::nullopt;
    }
    return out;
}

// --- NVS -------------------------------------------------------------------------------------

std::string nvs_string(const char* key) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return {};
    }
    std::size_t length = 0;
    std::string value;
    if (nvs_get_str(handle, key, nullptr, &length) == ESP_OK && length > 0) {
        value.resize(length);
        if (nvs_get_str(handle, key, value.data(), &length) == ESP_OK) {
            value.resize(length - 1);
        } else {
            value.clear();
        }
    }
    nvs_close(handle);
    return value;
}

std::uint32_t nvs_number(const char* key) {
    nvs_handle_t handle = 0;
    std::uint32_t value = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u32(handle, key, &value);
        nvs_close(handle);
    }
    return value;
}

void nvs_store_string(const char* key, std::string_view value) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    const std::string text(value);
    (void)nvs_set_str(handle, key, text.c_str());
    (void)nvs_commit(handle);
    nvs_close(handle);
}

void nvs_store_number(const char* key, std::uint32_t value) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    (void)nvs_set_u32(handle, key, value);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

// --- replies ---------------------------------------------------------------------------------

esp_err_t reply(httpd_req_t* req, const char* status, const char* type, std::string_view body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
}

esp_err_t reply_text(httpd_req_t* req, const char* status, std::string_view text) {
    std::string body(text);
    body += '\n';
    return reply(req, status, "text/plain; charset=utf-8", body);
}

// A refusal as the plan's routes give it: the status, and the word with its words.
esp_err_t refuse(httpd_req_t* req, const char* status, std::string_view code) {
    return reply_text(req, status, update::error_text(code));
}

std::string read_body(httpd_req_t* req, std::size_t limit) {
    std::string body;
    const std::size_t want = std::min<std::size_t>(req->content_len, limit);
    body.resize(want);
    std::size_t got = 0;
    while (got < want) {
        const int n = httpd_req_recv(req, body.data() + got, want - got);
        if (n <= 0) {
            break;
        }
        got += static_cast<std::size_t>(n);
    }
    body.resize(got);
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) {
        body.pop_back();
    }
    return body;
}

// --- the connection --------------------------------------------------------------------------

bool is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// One GET, following the redirects GitHub makes to its CDN, to https only (to http only in CI's
// loopback test). Nothing but the board's own settings chooses a host: the index names files, never
// addresses.
class Download {
   public:
    Download(const PullConfig& config, std::string url) : config_(config), url_(std::move(url)) {}
    ~Download() { close(); }
    Download(const Download&) = delete;
    Download& operator=(const Download&) = delete;

    std::string code;
    std::string text;

    [[nodiscard]] bool open() {
        const bool secure = url_.rfind("https://", 0) == 0;
        if (!secure && !(config_.allow_http && url_.rfind("http://", 0) == 0)) {
            return fail("tls", "The update address is not a secure one.");
        }
        esp_http_client_config_t cfg = {};
        cfg.url = url_.c_str();
        cfg.timeout_ms = kNetworkTimeoutMs;
        // GitHub's redirect to its CDN is a Location of about 900 bytes: the default 512 cannot
        // hold it.
        cfg.buffer_size = 2048;
        cfg.buffer_size_tx = 1024;
        cfg.disable_auto_redirect = true;
        cfg.keep_alive_enable = false;
        if (secure) {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }
        client_ = esp_http_client_init(&cfg);
        if (client_ == nullptr) {
            return fail("no_memory", update::error_text("no_memory"));
        }
        (void)esp_http_client_set_header(client_, "User-Agent", "iclforge-hearth");
        for (int hop = 0; hop <= kMaxRedirects; ++hop) {
            const esp_err_t opened = esp_http_client_open(client_, 0);
            if (opened != ESP_OK) {
                return failed_to_connect(opened);
            }
            length_ = esp_http_client_fetch_headers(client_);
            const int status = esp_http_client_get_status_code(client_);
            if (is_redirect(status)) {
                if (esp_http_client_set_redirection(client_) != ESP_OK) {
                    return fail("unreachable",
                                "GitHub sent the player somewhere it could not follow.");
                }
                char next[16];
                // Only the scheme is wanted; a URL longer than the buffer is cut, and its start is
                // the same.
                if (esp_http_client_get_url(client_, next, sizeof(next)) != ESP_OK ||
                    !(std::strncmp(next, "https://", 8) == 0 ||
                      (config_.allow_http && std::strncmp(next, "http://", 7) == 0))) {
                    return fail(
                        "tls",
                        "GitHub redirected the player to an address that is not a secure one.");
                }
                (void)esp_http_client_flush_response(client_, nullptr);
                continue;
            }
            if (status == 200) {
                return true;
            }
            return fail("unreachable", status == 404
                                           ? "The file is not there (404)."
                                           : "GitHub answered " + std::to_string(status) + ".");
        }
        return fail("unreachable", "GitHub sent the player round in circles.");
    }

    [[nodiscard]] std::int64_t length() const { return length_; }

    // Bytes read, 0 at the end of the body, negative on an error.
    int read(std::uint8_t* into, std::size_t n) {
        return esp_http_client_read(client_, reinterpret_cast<char*>(into), static_cast<int>(n));
    }

    void close() {
        if (client_ != nullptr) {
            (void)esp_http_client_close(client_);
            (void)esp_http_client_cleanup(client_);
            client_ = nullptr;
        }
    }

   private:
    bool fail(const char* c, std::string t) {
        code = c;
        text = std::move(t);
        return false;
    }

    bool failed_to_connect(esp_err_t err) {
        int tls_error = 0;
        int tls_flags = 0;
        (void)esp_http_client_get_and_clear_last_tls_error(client_, &tls_error, &tls_flags);
        std::printf("firmware: update connection failed: %s (tls %d, flags 0x%x)\n",
                    esp_err_to_name(err), tls_error, static_cast<unsigned>(tls_flags));
        if (tls_flags != 0 || tls_error == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            return fail("tls", update::error_text("tls"));
        }
        return fail("unreachable", update::error_text("unreachable"));
    }

    const PullConfig& config_;
    std::string url_;
    esp_http_client_handle_t client_ = nullptr;
    std::int64_t length_ = -1;
};

// A whole small file into memory: the index or its signature. Nothing past `limit` is read.
bool fetch_small(const PullConfig& config, const std::string& url, std::size_t limit,
                 std::string& out, std::string& code, std::string& text) {
    Download download(config, url);
    if (!download.open()) {
        code = download.code;
        text = download.text;
        return false;
    }
    if (download.length() > static_cast<std::int64_t>(limit)) {
        code = "bad_index";
        text = "A file in the update list is longer than the player reads.";
        return false;
    }
    out.clear();
    std::array<std::uint8_t, 512> chunk{};
    while (true) {
        const int n = download.read(chunk.data(), chunk.size());
        if (n < 0) {
            code = "unreachable";
            text = update::error_text("unreachable");
            return false;
        }
        if (n == 0) {
            return true;
        }
        if (out.size() + static_cast<std::size_t>(n) > limit) {
            code = "bad_index";
            text = "A file in the update list is longer than the player reads.";
            return false;
        }
        out.append(reinterpret_cast<const char*>(chunk.data()), static_cast<std::size_t>(n));
    }
}

// --- trust -----------------------------------------------------------------------------------

struct Certificates {
    mbedtls_x509_crt crt;
    Certificates() { mbedtls_x509_crt_init(&crt); }
    ~Certificates() { mbedtls_x509_crt_free(&crt); }
    Certificates(const Certificates&) = delete;
    Certificates& operator=(const Certificates&) = delete;
};

struct VerifyContext {
    const mbedtls_x509_crt* anchors = nullptr;
    bool judge_dates = true;
};

// Called for each certificate of the chain after mbedTLS has judged it (planning/esp32-ota.md,
// "Expiry and the clock"): the anchor's own dates are not looked at, because it is trusted by being
// in the build, and in a build with no clock no certificate's are.
int on_verify(void* arg, mbedtls_x509_crt* crt, int /*depth*/, std::uint32_t* flags) {
    const auto* ctx = static_cast<const VerifyContext*>(arg);
    bool is_anchor = false;
    for (const mbedtls_x509_crt* a = ctx->anchors; a != nullptr; a = a->next) {
        if (a == crt) {
            is_anchor = true;
        }
    }
    if (is_anchor || !ctx->judge_dates) {
        *flags &=
            ~static_cast<std::uint32_t>(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    }
    return 0;
}

std::string common_name(const mbedtls_x509_crt& crt) {
    std::string found;
    for (const mbedtls_x509_name* name = &crt.subject; name != nullptr; name = name->next) {
        if (name->oid.len == MBEDTLS_OID_SIZE(MBEDTLS_OID_AT_CN) &&
            std::memcmp(name->oid.p, MBEDTLS_OID_AT_CN, name->oid.len) == 0) {
            found.assign(reinterpret_cast<const char*>(name->val.p), name->val.len);
        }
    }
    return found;
}

// An ECDSA signature as the signing tool writes it (r || s, 32 bytes each) in the DER form mbedTLS
// reads.
std::vector<std::uint8_t> ecdsa_der(std::span<const std::uint8_t> raw) {
    const auto integer = [](std::span<const std::uint8_t> value) {
        std::size_t skip = 0;
        while (skip + 1 < value.size() && value[skip] == 0) {
            ++skip;
        }
        std::vector<std::uint8_t> out{0x02, 0};
        if (value[skip] & 0x80) {
            out.push_back(0x00);
        }
        out.insert(out.end(), value.begin() + static_cast<std::ptrdiff_t>(skip), value.end());
        out[1] = static_cast<std::uint8_t>(out.size() - 2);
        return out;
    };
    const std::vector<std::uint8_t> r = integer(raw.first(raw.size() / 2));
    const std::vector<std::uint8_t> s = integer(raw.last(raw.size() / 2));
    std::vector<std::uint8_t> out{0x30, static_cast<std::uint8_t>(r.size() + s.size())};
    out.insert(out.end(), r.begin(), r.end());
    out.insert(out.end(), s.begin(), s.end());
    return out;
}

// What index_verify.py checks, with mbedTLS: the chain to an anchor of this build, the leaf's usage
// and pinned name, and the signature over the index file's exact bytes.
TrustOutcome verify_trust(std::string_view index_bytes, const update::SignatureFile& file,
                          std::string_view anchors_pem, std::string_view leaf_name,
                          bool judge_dates) {
    TrustOutcome outcome;
    outcome.code = "untrusted";
    Certificates anchors;
    if (anchors_pem.empty() ||
        mbedtls_x509_crt_parse(&anchors.crt,
                               reinterpret_cast<const unsigned char*>(anchors_pem.data()),
                               anchors_pem.size()) != 0) {
        return outcome;
    }
    Certificates chain;
    for (const std::vector<std::uint8_t>& der : file.chain) {
        if (mbedtls_x509_crt_parse_der(&chain.crt, der.data(), der.size()) != 0) {
            return outcome;
        }
    }
    VerifyContext ctx{&anchors.crt, judge_dates};
    std::uint32_t flags = 0;
    const int verified = mbedtls_x509_crt_verify_with_profile(&chain.crt, &anchors.crt, nullptr,
                                                              &mbedtls_x509_crt_profile_default,
                                                              nullptr, &flags, &on_verify, &ctx);
    if (verified != 0) {
        if (flags & MBEDTLS_X509_BADCERT_EXPIRED) {
            outcome.code = "cert_expired";
        } else if (flags & MBEDTLS_X509_BADCERT_FUTURE) {
            outcome.code = "cert_not_yet_valid";
        } else if (flags & (MBEDTLS_X509_BADCERT_BAD_KEY | MBEDTLS_X509_BADCERT_BAD_MD |
                            MBEDTLS_X509_BADCERT_BAD_PK)) {
            outcome.code = "weak_key";
        }
        std::printf("firmware: the update list's chain was not trusted (mbedTLS %d, flags 0x%x)\n",
                    verified, static_cast<unsigned>(flags));
        return outcome;
    }
    const mbedtls_x509_crt& leaf = chain.crt;
    // A signing certificate: not a CA, for digital signatures (the extension present, which
    // mbedTLS's own check forgives the absence of) and for code signing.
    bool code_signing = false;
    for (const mbedtls_x509_sequence* usage = &leaf.ext_key_usage; usage != nullptr;
         usage = usage->next) {
        if (usage->buf.tag == MBEDTLS_ASN1_OID &&
            usage->buf.len == MBEDTLS_OID_SIZE(MBEDTLS_OID_CODE_SIGNING) &&
            std::memcmp(usage->buf.p, MBEDTLS_OID_CODE_SIGNING, usage->buf.len) == 0) {
            code_signing = true;
        }
    }
    if (mbedtls_x509_crt_get_ca_istrue(&leaf) > 0 ||
        !mbedtls_x509_crt_has_ext_type(&leaf, MBEDTLS_X509_EXT_KEY_USAGE) ||
        mbedtls_x509_crt_check_key_usage(&leaf, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0 ||
        !code_signing) {
        outcome.code = "leaf_usage";
        return outcome;
    }
    outcome.leaf_name = common_name(leaf);
    if (!leaf_name.empty() && outcome.leaf_name != leaf_name) {
        outcome.code = "leaf_name";
        outcome.leaf_name.clear();
        return outcome;
    }

    const auto digest =
        sha256({reinterpret_cast<const std::uint8_t*>(index_bytes.data()), index_bytes.size()});
    if (!digest) {
        outcome.code = "bad_signature";
        outcome.leaf_name.clear();
        return outcome;
    }
    mbedtls_pk_sigalg_t type = MBEDTLS_PK_SIGALG_RSA_PKCS1V15;
    std::vector<std::uint8_t> der;
    std::span<const std::uint8_t> signature(file.signature);
    if (file.algorithm == update::SignatureAlgorithm::kRsaPssSha256) {
        type = MBEDTLS_PK_SIGALG_RSA_PSS;
    } else if (file.algorithm == update::SignatureAlgorithm::kEcdsaP256Sha256) {
        type = MBEDTLS_PK_SIGALG_ECDSA;
        if (file.signature.size() != 64) {
            outcome.code = "bad_signature";
            outcome.leaf_name.clear();
            return outcome;
        }
        der = ecdsa_der(file.signature);
        signature = der;
    }
    const int checked =
        mbedtls_pk_verify_ext(type, const_cast<mbedtls_pk_context*>(&leaf.pk), MBEDTLS_MD_SHA256,
                              digest->data(), digest->size(), signature.data(), signature.size());
    if (checked != 0) {
        std::printf("firmware: the update list's signature did not verify (mbedTLS %d)\n", checked);
        outcome.code = "bad_signature";
        outcome.leaf_name.clear();
        return outcome;
    }
    outcome.ok = true;
    outcome.code.clear();
    return outcome;
}

// How many certificates the anchor file holds, and a short SHA-256 of each, for GET
// /firmware/available.
void describe_anchors(std::string_view pem, int& count, std::vector<std::string>& fingerprints) {
    count = 0;
    fingerprints.clear();
    if (pem.empty()) {
        return;
    }
    Certificates anchors;
    if (mbedtls_x509_crt_parse(&anchors.crt, reinterpret_cast<const unsigned char*>(pem.data()),
                               pem.size()) != 0) {
        return;
    }
    for (const mbedtls_x509_crt* a = &anchors.crt; a != nullptr && a->raw.p != nullptr;
         a = a->next) {
        ++count;
        if (const auto digest = sha256({a->raw.p, a->raw.len})) {
            fingerprints.push_back(hex(std::span<const std::uint8_t>(digest->data(), 4)));
        }
    }
}

}  // namespace

struct FirmwarePull::Impl {
    Firmware* firmware = nullptr;
    PullHooks hooks;
    PullConfig config;
    int anchor_count = 0;
    std::vector<std::string> anchor_prints;

    mutable std::mutex mutex;  // guards what follows
    State state = State::kIdle;
    std::string channel;
    std::optional<std::int64_t> checked;
    std::optional<Latest> latest;
    std::optional<update::IndexImage> offered;  // the verified index's image for this board
    std::string offered_version;
    std::int64_t offered_at_us = 0;
    std::string signed_by;
    std::string error_code;
    std::string error_text;
    bool task_running = false;
    bool in_flash_mode_by_pull = false;

    [[nodiscard]] update::SupportFacts support_facts(const FirmwareFacts& facts) const;
    [[nodiscard]] update::BlockFacts block_facts(const FirmwareFacts& facts,
                                                 bool for_install) const;
    [[nodiscard]] update::BoardTarget target(const FirmwareFacts& facts) const;
    [[nodiscard]] std::string leaf_pin(std::string_view which) const {
        return which == "stable" ? config.leaf_stable : config.leaf_beta;
    }
    [[nodiscard]] std::string base_problem() const;
    void fail(std::string_view code, std::string text);
    void run_check();
    void run_install();
    static void check_task(void* arg);
    static void install_task(void* arg);
    bool begin_task(TaskFunction_t task, const char* name, std::string& code);
};

update::SupportFacts FirmwarePull::Impl::support_facts(const FirmwareFacts& facts) const {
    update::SupportFacts s;
    s.pull_built = base_problem().empty();
    s.trust_anchors = anchor_count;
    s.has_network = config.has_network;
    s.has_clock = config.has_clock || !config.judge_dates;
    s.has_second_slot = facts.has_other_slot;
    return s;
}

// The base address, when it is not one a pull may use.
std::string FirmwarePull::Impl::base_problem() const {
    const std::string& base = config.base_url;
    if (base.rfind("https://", 0) == 0 || (config.allow_http && base.rfind("http://", 0) == 0)) {
        return {};
    }
    return "This firmware's update address is not a secure one.";
}

update::BlockFacts FirmwarePull::Impl::block_facts(const FirmwareFacts& facts,
                                                   bool for_install) const {
    update::BlockFacts b;
    b.network_up = hooks.network_up ? hooks.network_up() : true;
    b.clock_set = !config.judge_dates || (hooks.clock_unix && hooks.clock_unix().has_value());
    b.time_sync_on = hooks.time_sync_on ? hooks.time_sync_on() : true;
    b.on_trial = facts.on_trial;
    b.trial_minutes_left = (facts.trial_remaining_ms + 59'999U) / 60'000U;
    // A board in flash mode because a pull of its own put it there is not waiting for an upload: it
    // can check again, and try again.
    b.flash_mode = facts.flash_mode && !in_flash_mode_by_pull;
    b.update_running = facts.busy || task_running;
    b.room_beside_stream =
        for_install || !hooks.room_beside_stream || hooks.room_beside_stream() || facts.flash_mode;
    return b;
}

update::BoardTarget FirmwarePull::Impl::target(const FirmwareFacts& facts) const {
    update::BoardTarget t;
    t.project = facts.project;
    t.chip_id = facts.chip_id;
    t.revision_full = facts.revision_full;
    t.flash_mb = facts.flash_mb;
    t.psram = config.psram;
    t.slot_bytes = facts.slot_bytes;
    return t;
}

void FirmwarePull::Impl::fail(std::string_view code, std::string text) {
    std::printf("firmware: update %.*s: %s\n", static_cast<int>(code.size()), code.data(),
                text.c_str());
    const std::lock_guard lock(mutex);
    state = State::kFailed;
    error_code = std::string(code);
    error_text = text.empty() ? update::error_text(code) : std::move(text);
    task_running = false;
}

bool FirmwarePull::Impl::begin_task(TaskFunction_t task, const char* name, std::string& code) {
    {
        const std::lock_guard lock(mutex);
        if (task_running) {
            code = "busy";
            return false;
        }
        task_running = true;
    }
    if (xTaskCreate(task, name, config.task_stack_bytes, this, tskIDLE_PRIORITY + 4, nullptr) !=
        pdPASS) {
        const std::lock_guard lock(mutex);
        task_running = false;
        code = "no_memory";
        return false;
    }
    return true;
}

// The check: both small files, the signature, and what to do with them.
void FirmwarePull::Impl::run_check() {
    std::string channel_now;
    {
        const std::lock_guard lock(mutex);
        channel_now = channel;
    }
    const FirmwareFacts facts = firmware->facts();
    std::string index_text;
    std::string sig_text;
    std::string code;
    std::string text;
    if (!fetch_small(config, update::index_url(config.base_url, channel_now, false),
                     update::kMaxIndexBytes, index_text, code, text) ||
        !fetch_small(config, update::index_url(config.base_url, channel_now, true),
                     update::kMaxSignatureBytes, sig_text, code, text)) {
        fail(code, text);
        return;
    }
    const auto digest =
        sha256({reinterpret_cast<const std::uint8_t*>(index_text.data()), index_text.size()});
    if (!digest) {
        fail("bad_index", {});
        return;
    }
    const update::ParsedSignature signature = update::parse_signature_file(sig_text);
    if (!signature.file) {
        fail(signature.code, {});
        return;
    }
    if (!update::signature_is_for(*signature.file, hex(*digest))) {
        fail("hash_mismatch", {});
        return;
    }
    const TrustOutcome trust = verify_trust(index_text, *signature.file, config.anchors_pem,
                                            leaf_pin(channel_now), config.judge_dates);
    if (!trust.ok) {
        fail(trust.code, {});
        return;
    }
    const update::ParsedIndex parsed = update::parse_index(index_text);
    if (!parsed.index) {
        fail("bad_index", parsed.why);
        return;
    }
    const update::UpdateIndex& index = *parsed.index;
    update::PullFacts pull;
    pull.channel = channel_now;
    pull.running_version = facts.version;
    pull.running_secure_version = facts.secure_version;
    pull.last_sequence = nvs_number(channel_now == "stable" ? kKeySeqStable : kKeySeqBeta);
    if (facts.last_update && !facts.last_update->version.empty() &&
        (facts.last_update->result == "rolled back" || facts.last_update->result == "failed" ||
         facts.last_update->result == "refused" || facts.last_update->result == "interrupted")) {
        pull.skipped_version = facts.last_update->version;
    }
    const update::Decision decision = update::decide(index, target(facts), pull);
    if (decision.verdict == update::Verdict::kRefused) {
        fail(decision.code, {});
        return;
    }
    // The pair verified and was not a replay: its sequence is the new floor.
    nvs_store_number(channel_now == "stable" ? kKeySeqStable : kKeySeqBeta,
                     std::max(pull.last_sequence, index.sequence));

    const std::lock_guard lock(mutex);
    checked = hooks.clock_unix ? hooks.clock_unix() : std::nullopt;
    signed_by = trust.leaf_name;
    error_code.clear();
    error_text.clear();
    task_running = false;
    if (decision.verdict == update::Verdict::kUpToDate) {
        state = State::kUpToDate;
        latest.reset();
        offered.reset();
        return;
    }
    state = State::kAvailable;
    Latest l;
    l.version = index.version;
    l.released = index.released;
    l.size = decision.image->size;
    l.notes = update::notes_url(config.base_url, index.version);
    l.image = decision.image->name;
    latest = std::move(l);
    offered = *decision.image;
    offered_version = index.version;
    offered_at_us = esp_timer_get_time();
    std::printf("firmware: %s is available (%s); signed by %s\n", index.version.c_str(),
                decision.image->name.c_str(), trust.leaf_name.c_str());
}

void FirmwarePull::Impl::check_task(void* arg) {
    auto* im = static_cast<Impl*>(arg);
    im->run_check();
    {
        const std::lock_guard lock(im->mutex);
        im->task_running = false;
    }
    vTaskDelete(nullptr);
}

// The install: the image the check offered, downloaded as Firmware::install_pulled writes it.
void FirmwarePull::Impl::run_install() {
    update::IndexImage image;
    std::string version;
    {
        const std::lock_guard lock(mutex);
        if (!offered) {
            task_running = false;
            return;
        }
        image = *offered;
        version = offered_version;
        state = State::kInstalling;
        error_code.clear();
        error_text.clear();
        in_flash_mode_by_pull = true;
    }
    std::string url;
    if (const auto built = update::image_url(config.base_url, version, image.file)) {
        url = *built;
    } else {
        fail("bad_index", {});
        return;
    }
    Download download(config, url);
    bool opened = false;
    PulledImage pulled;
    pulled.version = version;
    pulled.length = static_cast<std::size_t>(image.size);
    if (!unhex(image.sha256, pulled.sha256)) {
        fail("bad_index", {});
        return;
    }
    pulled.read = [&](std::span<std::uint8_t> into) -> bool {
        if (!opened) {
            if (!download.open()) {
                pulled.code = download.code;
                pulled.text = download.text;
                return false;
            }
            opened = true;
            if (download.length() != static_cast<std::int64_t>(image.size)) {
                pulled.code = "bad_index";
                pulled.text = "The download's size is not the update list's.";
                return false;
            }
        }
        std::size_t got = 0;
        while (got < into.size()) {
            const int n = download.read(into.data() + got, into.size() - got);
            if (n <= 0) {
                pulled.code = "unreachable";
                pulled.text =
                    n < 0 ? update::error_text("unreachable") : "The download ended early.";
                return false;
            }
            got += static_cast<std::size_t>(n);
        }
        return true;
    };
    std::printf("firmware: installing %s from %s\n", version.c_str(), url.c_str());
    const PullInstall result = firmware->install_pulled(pulled);
    // Back here only when nothing was installed.
    fail(result.code, result.text);
}

void FirmwarePull::Impl::install_task(void* arg) {
    auto* im = static_cast<Impl*>(arg);
    im->run_install();
    {
        const std::lock_guard lock(im->mutex);
        im->task_running = false;
    }
    vTaskDelete(nullptr);
}

// --- FirmwarePull ----------------------------------------------------------------------------

FirmwarePull::FirmwarePull() = default;

FirmwarePull::~FirmwarePull() {
    delete impl_;
}

bool FirmwarePull::start(Firmware& firmware, PullHooks hooks, PullConfig config) {
    if (impl_ != nullptr) {
        return true;
    }
    auto* im = new Impl{};
    im->firmware = &firmware;
    im->hooks = std::move(hooks);
    im->config = std::move(config);
    describe_anchors(im->config.anchors_pem, im->anchor_count, im->anchor_prints);
    std::string stored = nvs_string(kKeyChannel);
    im->channel = (stored == "stable" || stored == "beta") ? stored : im->config.default_channel;
    if (im->channel != "stable" && im->channel != "beta") {
        im->channel = "beta";
    }
    impl_ = im;
    std::printf("firmware: updates from GitHub: channel %s, %d trust anchor(s)\n",
                im->channel.c_str(), im->anchor_count);
    return true;
}

UpdateStatus FirmwarePull::status() const {
    UpdateStatus s;
    if (impl_ == nullptr) {
        s.why = update::error_text("not_supported");
        return s;
    }
    Impl& im = *impl_;
    const FirmwareFacts facts = im.firmware->facts();
    std::string why = im.base_problem();
    if (why.empty()) {
        why = update::unsupported_reason(im.support_facts(facts));
    }
    s.supported = why.empty();
    s.why = why;
    s.running = facts.version;
    s.trust.anchors = im.anchor_count;
    s.trust.fingerprints = im.anchor_prints;
    const std::lock_guard lock(im.mutex);
    s.channel = im.channel;
    s.state = im.state;
    s.checked = im.checked;
    s.latest = im.latest;
    s.signed_by = im.signed_by;
    s.error_code = im.error_code;
    s.error_text = im.error_text;
    if (s.supported) {
        s.blocked = update::blocked_reason(im.block_facts(facts, false));
    }
    if (facts.last_update && !facts.last_update->version.empty() &&
        (facts.last_update->result == "rolled back" || facts.last_update->result == "failed" ||
         facts.last_update->result == "refused" || facts.last_update->result == "interrupted")) {
        s.skipped = facts.last_update->version;
    }
    if (s.state == State::kInstalling && facts.upload) {
        const FirmwareUpload& up = *facts.upload;
        if (up.stage == "checking" || up.stage == "restarting") {
            s.stage = up.stage;
        } else {
            s.stage = "downloading";
            if (up.total > 0) {
                s.percent = static_cast<int>(up.received * 100 / up.total);
            }
        }
    } else if (s.state == State::kInstalling) {
        s.stage = "downloading";
    }
    return s;
}

bool FirmwarePull::start_check(std::string& code) {
    if (impl_ == nullptr) {
        code = "not_supported";
        return false;
    }
    Impl& im = *impl_;
    const FirmwareFacts facts = im.firmware->facts();
    if (!im.base_problem().empty() ||
        !update::unsupported_reason(im.support_facts(facts)).empty()) {
        code = "not_supported";
        return false;
    }
    const update::BlockFacts block = im.block_facts(facts, false);
    if (!update::blocked_reason(block).empty()) {
        if (block.update_running) {
            code = "busy";
        } else if (block.on_trial) {
            code = "on_trial";
        } else if (!block.network_up) {
            code = "no_network";
        } else if (!block.clock_set || !block.time_sync_on) {
            code = "no_clock";
        } else {
            code = block.flash_mode ? "busy" : "no_memory";
        }
        return false;
    }
    {
        const std::lock_guard lock(im.mutex);
        im.state = State::kChecking;
        im.error_code.clear();
        im.error_text.clear();
    }
    if (!im.begin_task(&Impl::check_task, "fw_pull_chk", code)) {
        const std::lock_guard lock(im.mutex);
        im.state = State::kIdle;
        return false;
    }
    return true;
}

bool FirmwarePull::start_install(std::string& code) {
    if (impl_ == nullptr) {
        code = "not_supported";
        return false;
    }
    Impl& im = *impl_;
    const FirmwareFacts facts = im.firmware->facts();
    if (!im.base_problem().empty() ||
        !update::unsupported_reason(im.support_facts(facts)).empty()) {
        code = "not_supported";
        return false;
    }
    {
        const std::lock_guard lock(im.mutex);
        const bool fresh = esp_timer_get_time() - im.offered_at_us <
                           static_cast<std::int64_t>(im.config.max_offer_age_s) * 1'000'000;
        if (!im.offered || (im.state != State::kAvailable && im.state != State::kFailed)) {
            code = "nothing_offered";
            return false;
        }
        if (!fresh) {
            code = "offer_stale";
            return false;
        }
    }
    if (facts.on_trial) {
        code = "on_trial";
        return false;
    }
    if (!im.begin_task(&Impl::install_task, "fw_pull", code)) {
        return false;
    }
    return true;
}

bool FirmwarePull::set_channel(std::string_view channel) {
    if (impl_ == nullptr || (channel != "stable" && channel != "beta")) {
        return false;
    }
    Impl& im = *impl_;
    const std::lock_guard lock(im.mutex);
    if (im.task_running || im.state == State::kInstalling) {
        return false;
    }
    if (im.channel != channel) {
        im.channel = std::string(channel);
        nvs_store_string(kKeyChannel, channel);
        // What was offered was another channel's.
        im.state = State::kIdle;
        im.latest.reset();
        im.offered.reset();
        im.signed_by.clear();
        im.error_code.clear();
        im.error_text.clear();
    }
    return true;
}

int FirmwarePull::on_available(httpd_req* req) {
    if (impl_ == nullptr) {
        return reply_text(req, "404 Not Found", "this board takes no firmware updates");
    }
    return reply(req, "200 OK", "application/json", update::render_status(status()));
}

int FirmwarePull::on_check(httpd_req* req) {
    if (impl_ == nullptr) {
        return reply_text(req, "404 Not Found", "this board takes no firmware updates");
    }
    {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->state == State::kChecking) {
            return reply(req, "202 Accepted", "application/json", "{\"state\":\"checking\"}\n");
        }
    }
    std::string code;
    if (!start_check(code)) {
        const char* status =
            code == "not_supported" || code == "no_network" || code == "no_clock"
                ? "412 Precondition Failed"
                : (code == "no_memory" ? "503 Service Unavailable" : "409 Conflict");
        return refuse(req, status, code);
    }
    return reply(req, "202 Accepted", "application/json", "{\"state\":\"checking\"}\n");
}

int FirmwarePull::on_from_release(httpd_req* req) {
    if (impl_ == nullptr) {
        return reply_text(req, "404 Not Found", "this board takes no firmware updates");
    }
    if (!impl_->firmware->host_allowed(req)) {
        return reply_text(req, "403 Forbidden",
                          "firmware changes are taken only on the board's own address or name "
                          "(planning/esp32-ota.md)");
    }
    const std::string asked = read_body(req, 64);
    {
        const std::lock_guard lock(impl_->mutex);
        if (!impl_->offered || asked != impl_->offered_version) {
            return refuse(req, "409 Conflict", "nothing_offered");
        }
    }
    std::string code;
    if (!start_install(code)) {
        const char* status =
            code == "not_supported"
                ? "412 Precondition Failed"
                : (code == "no_memory" ? "503 Service Unavailable" : "409 Conflict");
        return refuse(req, status, code);
    }
    return reply(req, "202 Accepted", "application/json", "{\"state\":\"installing\"}\n");
}

int FirmwarePull::on_channel(httpd_req* req) {
    if (impl_ == nullptr) {
        return reply_text(req, "404 Not Found", "this board takes no firmware updates");
    }
    const std::string asked = read_body(req, 16);
    if (asked != "stable" && asked != "beta") {
        return reply_text(req, "400 Bad Request", "PUT /firmware/channel wants stable or beta");
    }
    if (!set_channel(asked)) {
        return refuse(req, "409 Conflict", "busy");
    }
    return reply(req, "200 OK", "application/json", update::render_status(status()));
}

}  // namespace iclforge
