// The cable: the ESP32-P4's EMAC through an RMII PHY. See ethernet.hpp.
//
// What the pins are is Kconfig's (main/Kconfig.projbuild, ICLFORGE_EXAMPLE_ETH_*), and the defaults
// there are those of Espressif's ESP32-P4X-Function-EV-Board: an IP101GR on SMI address 1, its
// reference clock in on GPIO50. ESP-IDF's generic 802.3 PHY driver drives it, the one its own
// examples/ethernet/basic uses on this board.

#include "ethernet.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "discovery.hpp"
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy_802_3.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "net/stack.hpp"

namespace player::net {
namespace {

// The interface's route priority, above the WiFi station's 100 (esp_netif_defaults.h): when both
// hold an address ESP-IDF's default route is the one with the higher number, and the cable is the
// one to prefer. ESP-IDF's own number for Ethernet is 50, below the station's, which would put
// a board with both on WiFi.
constexpr int kRoutePriority = 200;
// How often the driver asks the PHY whether a partner answers: the time a pulled cable takes to be
// noticed, and so the time before the board falls back to WiFi.
constexpr std::uint32_t kLinkCheckMs = 1000;

enum class Driver { not_made, made, unavailable };

constexpr EventBits_t kLinkBit = BIT0;
constexpr EventBits_t kAddressBit = BIT1;

// start and stop, and what they make. Held for the length of either.
std::mutex g_mutex;
Driver g_driver = Driver::not_made;
esp_eth_handle_t g_eth = nullptr;
esp_netif_t* g_netif = nullptr;
EventGroupHandle_t g_events = nullptr;

// Written by the event loop's task, read from any task.
std::atomic<bool> g_running{false};
std::atomic<bool> g_link{false};
std::atomic<bool> g_address{false};
std::atomic<int> g_speed_mbps{0};
std::atomic<bool> g_full_duplex{false};

void on_link_event(void*, esp_event_base_t, std::int32_t id, void*) {
    switch (id) {
        case ETHERNET_EVENT_START:
            g_running = true;
            break;
        case ETHERNET_EVENT_STOP:
            g_running = false;
            g_link = false;
            g_address = false;
            xEventGroupClearBits(g_events, kLinkBit | kAddressBit);
            break;
        case ETHERNET_EVENT_CONNECTED: {
            eth_speed_t speed = ETH_SPEED_10M;
            eth_duplex_t duplex = ETH_DUPLEX_HALF;
            (void)esp_eth_ioctl(g_eth, ETH_CMD_G_SPEED, &speed);
            (void)esp_eth_ioctl(g_eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
            g_speed_mbps = speed == ETH_SPEED_100M ? 100 : speed == ETH_SPEED_10M ? 10 : 1000;
            g_full_duplex = duplex == ETH_DUPLEX_FULL;
            g_link = true;
            std::printf("network: ethernet cable in, %d Mbit/s %s duplex\n", g_speed_mbps.load(),
                        g_full_duplex ? "full" : "half");
            xEventGroupSetBits(g_events, kLinkBit);
            break;
        }
        case ETHERNET_EVENT_DISCONNECTED:
            g_link = false;
            g_address = false;
            std::printf("network: ethernet cable out\n");
            xEventGroupClearBits(g_events, kLinkBit | kAddressBit);
            break;
        default:
            break;
    }
}

void on_ip_event(void*, esp_event_base_t, std::int32_t id, void* data) {
    if (id == IP_EVENT_ETH_GOT_IP) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(data);
        g_address = true;
        std::printf("network: ethernet address " IPSTR " via " IPSTR "\n",
                    IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
        xEventGroupSetBits(g_events, kAddressBit);
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        g_address = false;
        std::printf("network: ethernet lost its address\n");
        xEventGroupClearBits(g_events, kAddressBit);
    }
}

// Makes everything once. False, with the reason said, when the PHY does not answer.
bool make_driver() {
    stack_start();

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_ICLFORGE_EXAMPLE_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_ICLFORGE_EXAMPLE_ETH_PHY_RESET_GPIO;

    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = CONFIG_ICLFORGE_EXAMPLE_ETH_MDC_GPIO;
    emac.smi_gpio.mdio_num = CONFIG_ICLFORGE_EXAMPLE_ETH_MDIO_GPIO;
    emac.interface = EMAC_DATA_INTERFACE_RMII;
    // The PHY makes the 50 MHz reference clock and the MAC takes it in.
    emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac.clock_config.rmii.clock_gpio = CONFIG_ICLFORGE_EXAMPLE_ETH_RMII_CLK_GPIO;
    emac.emac_dataif_gpio.rmii.tx_en_num = CONFIG_ICLFORGE_EXAMPLE_ETH_RMII_TX_EN_GPIO;
    emac.emac_dataif_gpio.rmii.txd0_num = CONFIG_ICLFORGE_EXAMPLE_ETH_RMII_TXD0_GPIO;
    emac.emac_dataif_gpio.rmii.txd1_num = CONFIG_ICLFORGE_EXAMPLE_ETH_RMII_TXD1_GPIO;
    emac.emac_dataif_gpio.rmii.crs_dv_num = CONFIG_ICLFORGE_EXAMPLE_ETH_RMII_CRS_DV_GPIO;
    emac.emac_dataif_gpio.rmii.rxd0_num = CONFIG_ICLFORGE_EXAMPLE_ETH_RMII_RXD0_GPIO;
    emac.emac_dataif_gpio.rmii.rxd1_num = CONFIG_ICLFORGE_EXAMPLE_ETH_RMII_RXD1_GPIO;

    esp_eth_mac_t* const mac = esp_eth_mac_new_esp32(&emac, &mac_config);
    esp_eth_phy_t* const phy = esp_eth_phy_new_generic(&phy_config);
    if (mac == nullptr || phy == nullptr) {
        std::printf("error: ethernet: could not make the MAC or the PHY driver\n");
        if (mac != nullptr) {
            mac->del(mac);
        }
        if (phy != nullptr) {
            phy->del(phy);
        }
        return false;
    }
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    config.check_link_period_ms = kLinkCheckMs;
    // Installing resets the PHY and reads its ID over SMI, so a PHY that is not on the bus (the
    // pins are wrong, or the board has none) is refused here.
    if (const esp_err_t err = esp_eth_driver_install(&config, &g_eth); err != ESP_OK) {
        std::printf("error: ethernet: no PHY answered on the SMI bus (%s); the cable is not used\n",
                    esp_err_to_name(err));
        mac->del(mac);
        phy->del(phy);
        g_eth = nullptr;
        return false;
    }

    // Who answered, so a wrong pin or address shows as an ID of all ones or zeros and not as a
    // cable that is never seen. The IP101GR's is 0x0243 0x0C54 (IEEE registers 2 and 3).
    {
        std::uint32_t phy_address = 0;
        std::uint32_t id_high = 0;
        std::uint32_t id_low = 0;
        esp_eth_phy_reg_rw_data_t reg{.reg_addr = ETH_PHY_IDR1_REG_ADDR, .reg_value_p = &id_high};
        (void)esp_eth_ioctl(g_eth, ETH_CMD_G_PHY_ADDR, &phy_address);
        (void)esp_eth_ioctl(g_eth, ETH_CMD_READ_PHY_REG, &reg);
        reg.reg_addr = ETH_PHY_IDR2_REG_ADDR;
        reg.reg_value_p = &id_low;
        (void)esp_eth_ioctl(g_eth, ETH_CMD_READ_PHY_REG, &reg);
        std::printf("network: ethernet PHY at SMI address %u, ID %04x %04x\n",
                    static_cast<unsigned>(phy_address), static_cast<unsigned>(id_high),
                    static_cast<unsigned>(id_low));
    }

    esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
    inherent.route_prio = kRoutePriority;
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    netif_config.base = &inherent;
    g_netif = esp_netif_new(&netif_config);
    if (g_netif == nullptr) {
        std::printf("error: ethernet: could not make the network interface\n");
        return false;
    }
    // The name DHCP registers, as the station does (station.cpp): the board's own, not "espressif".
    const std::string host = discovery_host_name();
    if (const esp_err_t err = esp_netif_set_hostname(g_netif, host.c_str()); err != ESP_OK) {
        std::printf("warning: could not set the DHCP host name '%s' on the cable (%s)\n",
                    host.c_str(), esp_err_to_name(err));
    }
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(g_eth);
    if (glue == nullptr || esp_netif_attach(g_netif, glue) != ESP_OK) {
        std::printf("error: ethernet: could not attach the driver to its interface\n");
        return false;
    }

    g_events = xEventGroupCreate();
    ESP_ERROR_CHECK(
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_link_event, nullptr));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_ip_event, nullptr));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, &on_ip_event, nullptr));
    return true;
}

}  // namespace

bool ethernet_start() {
    const std::lock_guard lock(g_mutex);
    if (g_driver == Driver::unavailable) {
        return false;
    }
    if (g_driver == Driver::not_made) {
        g_driver = make_driver() ? Driver::made : Driver::unavailable;
        if (g_driver != Driver::made) {
            return false;
        }
    }
    if (g_running) {
        return true;
    }
    if (const esp_err_t err = esp_eth_start(g_eth); err != ESP_OK) {
        std::printf("error: ethernet: could not start the driver (%s)\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

void ethernet_stop() {
    const std::lock_guard lock(g_mutex);
    if (g_driver != Driver::made || !g_running) {
        return;
    }
    if (const esp_err_t err = esp_eth_stop(g_eth); err != ESP_OK) {
        std::printf("warning: ethernet: could not stop the driver (%s)\n", esp_err_to_name(err));
        return;
    }
    // The STOP event clears the rest; this is what a caller that asks straight after sees.
    g_running = false;
    g_link = false;
    g_address = false;
    std::printf("network: the ethernet driver is switched off\n");
}

bool ethernet_running() {
    return g_running;
}

bool ethernet_link() {
    return g_link;
}

bool ethernet_ready() {
    return g_address;
}

std::string ethernet_address() {
    if (!g_address || g_netif == nullptr) {
        return {};
    }
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(g_netif, &info) != ESP_OK || info.ip.addr == 0) {
        return {};
    }
    std::array<char, 16> text{};
    (void)std::snprintf(text.data(), text.size(), IPSTR, IP2STR(&info.ip));
    return std::string(text.data());
}

EthernetInfo ethernet_info() {
    EthernetInfo info;
    info.running = g_running;
    info.link = g_link;
    info.address = ethernet_address();
    if (g_link) {
        info.speed_mbps = g_speed_mbps.load();
        info.full_duplex = g_full_duplex.load();
    }
    // g_eth is written once, under the lock, before anything can be running.
    if (g_running && g_eth != nullptr) {
        std::array<std::uint8_t, 6> mac{};
        if (esp_eth_ioctl(g_eth, ETH_CMD_G_MAC_ADDR, mac.data()) == ESP_OK) {
            std::array<char, 18> text{};
            (void)std::snprintf(text.data(), text.size(), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0],
                                mac[1], mac[2], mac[3], mac[4], mac[5]);
            info.mac = text.data();
        }
    }
    return info;
}

bool ethernet_wait(unsigned link_ms, unsigned address_ms) {
    if (g_events == nullptr) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(g_events, kLinkBit | kAddressBit, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(link_ms));
    if ((bits & kAddressBit) != 0) {
        return true;
    }
    if ((bits & kLinkBit) == 0) {
        return false;
    }
    bits = xEventGroupWaitBits(g_events, kAddressBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(address_ms));
    return (bits & kAddressBit) != 0;
}

}  // namespace player::net
