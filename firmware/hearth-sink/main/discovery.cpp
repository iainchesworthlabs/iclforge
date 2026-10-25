// mDNS: the board's name and its Sendspin service. See discovery.hpp.

#include "discovery.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

#include "mdns.h"

#include "iclforge/device_name.hpp"

#include "network.hpp"
#include "sendspin.hpp"
#include "settings.hpp"

namespace player {
namespace {

// planning/hearth-sendspin-extension.md, row T4: a player advertises
// _sendspin._tcp with the path the server opens its WebSocket on, and the
// PLAYER column's port, kSendspinPort (sendspin.hpp), which the Sendspin
// player listens on.
constexpr const char* kService = "_sendspin";
constexpr const char* kProtocol = "_tcp";
constexpr const char* kPath = "/sendspin";

// mDNS hostnames take letters, digits and hyphens. A name someone typed into
// the page may have anything in it, so it is filtered rather than refused:
// "Sitting Room" becomes "sitting-room" and still answers, and a Latin letter
// with an accent becomes the letter it is spelt with (iclforge::devname::mdns_label).
std::string hostname_from(const char* name) { return iclforge::devname::mdns_label(name); }

// mdns_init() has run, and the Sendspin service has been added.
bool g_started = false;
bool g_service_added = false;

// The Sendspin service under `name`: its instance name and its `name` text.
bool add_service(const char* name) {
    const std::array<mdns_txt_item_t, 2> txt{{
        {"path", kPath},
        {"name", name},
    }};
    g_service_added = mdns_service_add(name, kService, kProtocol, kSendspinPort,
                                       const_cast<mdns_txt_item_t*>(txt.data()), txt.size()) == ESP_OK;
    return g_service_added;
}

}  // namespace

void discovery_start() {
    if (g_started) {
        return;
    }
    if (!network_ready()) {
        std::printf("mdns: no network, so nothing to advertise\n");
        return;
    }
    if (mdns_init() != ESP_OK) {
        std::printf("warning: mdns did not start; this sink will not be found by name\n");
        return;
    }
    g_started = true;

    const char* name = settings().name.data();
    const std::string host = hostname_from(name);
    (void)mdns_hostname_set(host.c_str());
    (void)mdns_instance_name_set(name);

    // A build without the Sendspin player answers to its name and advertises
    // no service it would not answer.
    if (!sendspin_built()) {
        std::printf("mdns: %s.local; no Sendspin player in this build\n", host.c_str());
        return;
    }
    if (!add_service(name)) {
        std::printf("warning: mdns could not advertise %s.%s\n", kService, kProtocol);
        return;
    }
    std::printf("mdns: %s.local, %s.%s on %u, path %s\n", host.c_str(), kService, kProtocol,
                static_cast<unsigned>(kSendspinPort), kPath);
}

void discovery_rename() {
    if (!g_started) {
        return;
    }
    const char* name = settings().name.data();
    const std::string host = hostname_from(name);
    // The host name is probed again under the new label; the old one is released. A name a server has
    // cached for the old one lapses with its record's lifetime.
    if (mdns_hostname_set(host.c_str()) != ESP_OK) {
        std::printf("warning: mdns would not take the host name %s\n", host.c_str());
    }
    (void)mdns_instance_name_set(name);
    // The service is withdrawn and added again under the new name, rather than renamed in place: the same two
    // calls discovery_withdraw() and discovery_start() make, which the image links already, where renaming
    // the instance and its text in place is another 1,250 bytes of the responder.
    if (g_service_added) {
        (void)mdns_service_remove(kService, kProtocol);
        if (!add_service(name)) {
            std::printf("warning: mdns could not rename %s.%s\n", kService, kProtocol);
        }
    }
    std::printf("mdns: renamed; %s.local, %s.%s as '%s'\n", host.c_str(), kService, kProtocol, name);
}

void discovery_withdraw() {
    if (!sendspin_built()) {
        return;
    }
    // Harmless when mDNS never started or the service was never added: the
    // call then finds nothing to remove.
    if (mdns_service_remove(kService, kProtocol) == ESP_OK) {
        std::printf("mdns: %s.%s withdrawn; the host name stays\n", kService, kProtocol);
    }
}

std::string discovery_host_name() { return hostname_from(settings().name.data()); }

}  // namespace player
