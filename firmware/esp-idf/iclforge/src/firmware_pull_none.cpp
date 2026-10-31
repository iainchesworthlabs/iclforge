// The update from GitHub releases for a build that does not carry it (CONFIG_ICLFORGE_FIRMWARE_PULL
// off): the routes exist, GET /firmware/available says why there is nothing to do, and the rest
// answer 404, as a route a firmware lacks does. See firmware_pull.cpp for the one that does.

#include "iclforge/firmware_pull.hpp"

#include <string>

#include "esp_http_server.h"

#include "iclforge/firmware.hpp"

namespace iclforge {

struct FirmwarePull::Impl {
    Firmware* firmware = nullptr;
};

namespace {

esp_err_t reply(httpd_req_t* req, const char* status, const char* type, const std::string& body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
}

esp_err_t not_built(httpd_req_t* req) {
    return reply(req, "404 Not Found", "text/plain; charset=utf-8",
                 "this firmware cannot update itself from GitHub\n");
}

}  // namespace

FirmwarePull::FirmwarePull() = default;

FirmwarePull::~FirmwarePull() {
    delete impl_;
}

bool FirmwarePull::start(Firmware& firmware, PullHooks /*hooks*/, PullConfig /*config*/) {
    if (impl_ == nullptr) {
        impl_ = new Impl{};
    }
    impl_->firmware = &firmware;
    return true;
}

update::UpdateStatus FirmwarePull::status() const {
    update::UpdateStatus s;
    s.supported = false;
    s.why = "This firmware has no room for a secure connection.";
    if (impl_ != nullptr && impl_->firmware != nullptr) {
        s.running = impl_->firmware->facts().version;
    }
    return s;
}

bool FirmwarePull::start_check(std::string& code) {
    code = "not_supported";
    return false;
}

bool FirmwarePull::start_install(std::string& code) {
    code = "not_supported";
    return false;
}

bool FirmwarePull::set_channel(std::string_view /*channel*/) {
    return false;
}

int FirmwarePull::on_available(httpd_req* req) {
    return reply(req, "200 OK", "application/json", update::render_status(status()));
}

int FirmwarePull::on_check(httpd_req* req) {
    return not_built(req);
}

int FirmwarePull::on_from_release(httpd_req* req) {
    return not_built(req);
}

int FirmwarePull::on_channel(httpd_req* req) {
    return not_built(req);
}

}  // namespace iclforge
