// The kit's way of reporting: its RJ45 port, one TCP port, and the way back to the Hearth image.
// net_report.hpp has the reasoning.

#include "net_report.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy_802_3.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace bench {
namespace {

constexpr int kPort = 8080;
constexpr std::int64_t kDeadlineUs = 15LL * 60 * 1000 * 1000;
constexpr TickType_t kAddressWait = pdMS_TO_TICKS(60 * 1000);
// After a pass has been fetched once the board stays up this long for another fetch, then goes back.
constexpr TickType_t kLinger = pdMS_TO_TICKS(3 * 60 * 1000);

// The kit's pins: the Kconfig defaults of firmware/hearth-sink/main/Kconfig.projbuild
// (ICLFORGE_EXAMPLE_ETH_*), which is what its image runs the port with.
constexpr int kMdcGpio = 31;
constexpr int kMdioGpio = 52;
constexpr int kPhyResetGpio = 51;
constexpr int kPhyAddress = 1;
constexpr int kRmiiClockGpio = 50;
constexpr int kTxEnGpio = 49;
constexpr int kTxd0Gpio = 34;
constexpr int kTxd1Gpio = 35;
constexpr int kCrsDvGpio = 28;
constexpr int kRxd0Gpio = 29;
constexpr int kRxd1Gpio = 30;

constexpr EventBits_t kAddressBit = BIT0;
EventGroupHandle_t g_events = nullptr;

// The image has never marked itself valid, so this takes the board back to the one it came from;
// the restart below is the same thing if that call is refused.
[[noreturn]] void go_back() {
    std::printf("net_report: returning to the previous image\n");
    std::fflush(stdout);
    (void)esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart();
}

void on_deadline(void*) {
    go_back();
}

void on_address(void*, esp_event_base_t, std::int32_t, void* data) {
    const auto* event = static_cast<const ip_event_got_ip_t*>(data);
    std::printf("net_report: address " IPSTR "\n", IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(g_events, kAddressBit);
}

// The port as the Hearth sink opens it (firmware/hearth-sink/main/net/wired/ethernet.cpp), without
// its link events, routing priority or discovery: an address is all this needs.
bool start_ethernet() {
    if (esp_netif_init() != ESP_OK) {
        return false;
    }
    const esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        return false;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = kPhyAddress;
    phy_config.reset_gpio_num = kPhyResetGpio;

    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = kMdcGpio;
    emac.smi_gpio.mdio_num = kMdioGpio;
    emac.interface = EMAC_DATA_INTERFACE_RMII;
    // The PHY makes the 50 MHz reference clock and the MAC takes it in.
    emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac.clock_config.rmii.clock_gpio = kRmiiClockGpio;
    emac.emac_dataif_gpio.rmii.tx_en_num = kTxEnGpio;
    emac.emac_dataif_gpio.rmii.txd0_num = kTxd0Gpio;
    emac.emac_dataif_gpio.rmii.txd1_num = kTxd1Gpio;
    emac.emac_dataif_gpio.rmii.crs_dv_num = kCrsDvGpio;
    emac.emac_dataif_gpio.rmii.rxd0_num = kRxd0Gpio;
    emac.emac_dataif_gpio.rmii.rxd1_num = kRxd1Gpio;

    esp_eth_mac_t* const mac = esp_eth_mac_new_esp32(&emac, &mac_config);
    esp_eth_phy_t* const phy = esp_eth_phy_new_generic(&phy_config);
    if (mac == nullptr || phy == nullptr) {
        return false;
    }
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = nullptr;
    if (esp_eth_driver_install(&config, &eth) != ESP_OK) {
        return false;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* const netif = esp_netif_new(&netif_config);
    if (netif == nullptr) {
        return false;
    }
    (void)esp_netif_set_hostname(netif, "crypto-bench");
    esp_eth_netif_glue_handle_t const glue = esp_eth_new_netif_glue(eth);
    if (glue == nullptr || esp_netif_attach(netif, glue) != ESP_OK) {
        return false;
    }

    g_events = xEventGroupCreate();
    if (esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_address, nullptr) != ESP_OK) {
        return false;
    }
    return esp_eth_start(eth) == ESP_OK;
}

bool send_all(int client, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const int n = send(client, data + sent, size - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// `GET /` answers with the pass as plain text (HTTP/1.0, so curl takes it); `GET /restore` sends the
// board back at once. Nothing else is read.
[[noreturn]] void serve(const char* text, std::size_t size) {
    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(kPort);
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        std::printf("net_report: could not listen on port %d\n", kPort);
        go_back();
    }

    bool fetched = false;
    TickType_t fetched_at = 0;
    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval wait{.tv_sec = 5, .tv_usec = 0};
        const int ready = select(listener + 1, &readable, nullptr, nullptr, &wait);
        if (fetched && xTaskGetTickCount() - fetched_at > kLinger) {
            go_back();
        }
        if (ready <= 0) {
            continue;
        }
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            continue;
        }
        timeval limit{.tv_sec = 2, .tv_usec = 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
        char request[96] = {};
        const int got = recv(client, request, sizeof(request) - 1, 0);
        if (got > 0 && std::strstr(request, "/restore") != nullptr) {
            static constexpr char kBye[] = "HTTP/1.0 200 OK\r\n\r\nrestoring\n";
            send_all(client, kBye, sizeof(kBye) - 1);
            close(client);
            vTaskDelay(pdMS_TO_TICKS(200));
            go_back();
        }
        static constexpr char kHead[] = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n";
        if (send_all(client, kHead, sizeof(kHead) - 1) && send_all(client, text, size)) {
            fetched = true;
            fetched_at = xTaskGetTickCount();
        }
        close(client);
    }
}

}  // namespace

void arm_return_to_previous_image() {
    esp_timer_create_args_t args{};
    args.callback = &on_deadline;
    args.name = "bench-return";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        (void)esp_timer_start_once(timer, kDeadlineUs);
    }
}

void report_over_network(const char* text, std::size_t size) {
    if (!start_ethernet()) {
        std::printf("net_report: the Ethernet port did not start\n");
        go_back();
    }
    if ((xEventGroupWaitBits(g_events, kAddressBit, pdFALSE, pdFALSE, kAddressWait) & kAddressBit) == 0) {
        std::printf("net_report: no address within a minute\n");
        go_back();
    }
    serve(text, size);
}

}  // namespace bench
