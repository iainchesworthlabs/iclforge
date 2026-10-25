#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "iclforge/firmware_update.hpp"

// Updates from GitHub releases (planning/esp32-ota.md, O10): a board looks at a small signed index
// of its channel, and installs the image it names when there is a newer one.
//
//   GET  /firmware/available     the flow's state (firmware_update.hpp's render_status)
//   POST /firmware/check         starts a check; the result is on GET /firmware/available
//   PUT  /firmware/from-release  body: the version a check offered. Enters flash mode, downloads
//   the
//                                image, checks it and restarts into it, on trial as an update is
//   PUT  /firmware/channel       body: stable or beta
//
// What it does, in order (the plan's "The flow on the board"): reads hearth-update-<channel>.json
// and its signature file over TLS, checks the signature against the trust anchors in the build (a
// chain to an anchor, a code-signing leaf with the channel's pinned name, the signature over the
// index file), decides by firmware_update.hpp's rules whether there is a newer image for this
// board, and on a request installs it through Firmware::install_pulled, which writes it as a pushed
// image is written. Nothing here is on the push path: PUT /firmware takes what it always took.
//
// A build without CONFIG_ICLFORGE_FIRMWARE_PULL links firmware_pull_none.cpp, which says so.

struct httpd_req;

namespace iclforge {

class Firmware;

struct PullHooks {
    // An address held now.
    std::function<bool()> network_up;
    // The time of day in unix seconds once the board knows it; nothing before.
    std::function<std::optional<std::int64_t>()> clock_unix;
    // Whether the board keeps its time with a time server (PUT /time-sync).
    std::function<bool()> time_sync_on;
    // Whether a TLS session fits beside what the board is doing: true when nothing plays, or when
    // the session's memory is PSRAM's.
    std::function<bool()> room_beside_stream;
};

struct PullConfig {
    // "https://github.com/<owner>/<repo>/releases/download": the index is
    // <base>/firmware-channels/hearth-update-<channel>.json and an image <base>/<tag>/<file>.
    std::string base_url;
    std::string default_channel = "beta";
    // The common names the signing leaf must have for each channel; empty accepts any leaf the
    // anchors vouch for.
    std::string leaf_stable;
    std::string leaf_beta;
    // The build's trust anchor file (PEM, with its terminating NUL counted); empty when it has
    // none, which is a build that cannot verify a pull and says so.
    std::string_view anchors_pem;
    bool has_network = true;
    bool has_clock = true;
    // The running image was built for a board with PSRAM (the index's psram field is matched to
    // it).
    bool psram = false;
    // CI's loopback test only: the base address may be http://, and a build with no clock judges no
    // dates.
    bool allow_http = false;
    bool judge_dates = true;
    // The pull task's stack, from internal RAM: it writes flash, and TLS runs on it.
    std::uint32_t task_stack_bytes = 12288;
    // A check older than this has to be made again before an install.
    std::uint32_t max_offer_age_s = 1800;
};

class FirmwarePull {
   public:
    FirmwarePull();
    ~FirmwarePull();
    FirmwarePull(const FirmwarePull&) = delete;
    FirmwarePull& operator=(const FirmwarePull&) = delete;

    // At boot, after Firmware::start. False only when it cannot allocate what it needs.
    [[nodiscard]] bool start(Firmware& firmware, PullHooks hooks, PullConfig config);

    // The state as GET /firmware/available reports it, and as the Preferences screen reads it.
    [[nodiscard]] update::UpdateStatus status() const;

    // Starting a check or an install without HTTP, for the screen. False with the code of why not
    // (firmware_update.hpp's error_text has words for it).
    [[nodiscard]] bool start_check(std::string& code);
    [[nodiscard]] bool start_install(std::string& code);
    [[nodiscard]] bool set_channel(std::string_view channel);

    int on_available(httpd_req* req);
    int on_check(httpd_req* req);
    int on_from_release(httpd_req* req);
    int on_channel(httpd_req* req);

    struct Impl;

   private:
    Impl* impl_ = nullptr;
};

}  // namespace iclforge
