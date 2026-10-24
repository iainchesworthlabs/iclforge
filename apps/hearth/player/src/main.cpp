// hearth-player: a Sendspin player that plays through this computer's speakers (docs/hearth/player.md).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "frame_device.hpp"
#include "iclforge/sendspin/firewall.hpp"
#include "hearth_player.hpp"

namespace {

namespace player = iclforge::hearth::player;
using namespace std::chrono_literals;

constexpr std::string_view kUsage = R"(usage: hearth-player [options]

A Sendspin player for this computer. AC-3 and E-AC-3 (Dolby Atmos objects included) arrive
undecoded over _iclforge_player@v1 and are decoded here, rendered to stereo; PCM, FLAC and Opus
arrive over player@v1. Both play through the output device.

  --list-devices         print the output devices and exit
  --device ID            the output device (default: the default endpoint); "null" plays to
                         nowhere, in real time
  --gain-db DB           gain on everything played, zero or below (default 0)
  --name NAME            the name servers show (default hearth-pc-<computer name>)
  --address ADDRESS      the address to listen on (default 0.0.0.0; 127.0.0.1 for this computer
                         only, which Windows never asks the firewall about)
  --port PORT            the port to listen on (default 8928; 0 for any)
  --state DIRECTORY      identity, pairing PSK and pairing records (default: hearth-player
                         under the user's application data)
  --pair METHOD          the pairing code offered: dynamic (default, printed here), static or
                         none; the pairing token is always offered
  --static-code DIGITS   the eight-digit code for --pair static
  --unpaired-access      admit servers that have not paired
  --no-mdns              do not advertise _sendspin._tcp
  --mdns-interface ADDR  advertise on this IPv4 interface only; repeatable
  --status SECONDS       print a status line this often while a stream plays (default 2; 0 for
                         none)
  --run-for SECONDS      exit after this long instead of waiting for 'quit'

Commands on standard input: window (open the static code's pairing window), reset (reset the
dynamic code's round limit), cancel (cancel pairing), status, quit.
)";

class ConsoleLog final : public player::PlayerLog {
   public:
    void line(std::string_view text) override {
        const std::lock_guard lock(mutex_);
        std::cout << text << std::endl;
    }

   private:
    std::mutex mutex_;
};

[[nodiscard]] std::optional<std::uint16_t> parse_number(std::string_view text) {
    unsigned value = 0;
    if (text.empty() || text.size() > 5) {
        return std::nullopt;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + static_cast<unsigned>(c - '0');
    }
    if (value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
    // The elevated relaunch for a Windows Firewall rule (libs/sendspin/firewall.hpp).
    iclforge::sendspin::firewall::maybe_run_as_firewall_helper_and_exit(argc, argv);

    player::PlayerOptions options;
    options.state_directory = player::default_state_directory();
    std::optional<std::chrono::seconds> run_for;
    std::chrono::seconds status_every = 2s;

    const std::vector<std::string_view> arguments(argv + 1, argv + argc);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        if (argument == "--help" || argument == "-h") {
            std::cout << kUsage;
            return EXIT_SUCCESS;
        }
        if (argument == "--list-devices") {
            const std::vector<player::DeviceEntry> devices = player::list_devices();
            if (devices.empty()) {
                std::cout << "no output devices\n";
            }
            for (const player::DeviceEntry& device : devices) {
                std::cout << (device.is_default ? "* " : "  ") << device.id << "  " << device.name << "\n";
            }
            return EXIT_SUCCESS;
        }
        if (argument == "--unpaired-access") {
            options.unpaired_access = true;
            continue;
        }
        if (argument == "--no-mdns") {
            options.advertise = false;
            continue;
        }
        if (i + 1 >= arguments.size()) {
            std::cerr << "unknown option or missing value: " << argument << "\n\n" << kUsage;
            return EXIT_FAILURE;
        }
        const std::string_view value = arguments[++i];
        if (argument == "--device") {
            options.device = value;
        } else if (argument == "--gain-db") {
            char* end = nullptr;
            const std::string text(value);
            options.gain_db = std::strtod(text.c_str(), &end);
            if (end == text.c_str() || *end != 0 || options.gain_db > 0.0) {
                std::cerr << "--gain-db takes a number of dB, zero or below\n";
                return EXIT_FAILURE;
            }
        } else if (argument == "--name") {
            options.name = value;
        } else if (argument == "--address") {
            options.address = value;
        } else if (argument == "--port") {
            const std::optional<std::uint16_t> port = parse_number(value);
            if (!port) {
                std::cerr << "--port takes a number from 0 to 65535\n";
                return EXIT_FAILURE;
            }
            options.port = *port;
        } else if (argument == "--state") {
            options.state_directory = std::filesystem::path(value);
        } else if (argument == "--pair") {
            if (value == "dynamic") {
                options.code_method = player::CodeMethod::kDynamic;
            } else if (value == "static") {
                options.code_method = player::CodeMethod::kStatic;
            } else if (value == "none") {
                options.code_method = player::CodeMethod::kNone;
            } else {
                std::cerr << "--pair takes dynamic, static or none\n";
                return EXIT_FAILURE;
            }
        } else if (argument == "--static-code") {
            options.static_code = value;
        } else if (argument == "--mdns-interface") {
            options.mdns_interfaces.emplace_back(value);
        } else if (argument == "--status") {
            const std::optional<std::uint16_t> seconds = parse_number(value);
            if (!seconds) {
                std::cerr << "--status takes a number of seconds\n";
                return EXIT_FAILURE;
            }
            status_every = std::chrono::seconds(*seconds);
        } else if (argument == "--run-for") {
            const std::optional<std::uint16_t> seconds = parse_number(value);
            if (!seconds) {
                std::cerr << "--run-for takes a number of seconds up to 65535\n";
                return EXIT_FAILURE;
            }
            run_for = std::chrono::seconds(*seconds);
        } else {
            std::cerr << "unknown option " << argument << "\n\n" << kUsage;
            return EXIT_FAILURE;
        }
    }
    if (options.name.empty()) {
        options.name = player::default_player_name();
    }

    ConsoleLog log;
    auto started = player::Player::start(options, log);
    if (!started) {
        std::cerr << "hearth-player: " << started.error() << "\n";
        return EXIT_FAILURE;
    }
    player::Player& hearth = **started;
    log.line("listening on " + options.address + ":" + std::to_string(hearth.port()) + " as \"" + options.name + "\"");
    log.line("client_id " + hearth.client_id());
    log.line("pairing token " + hearth.pairing_token());

    std::atomic<bool> quit{false};
    std::thread reader([&] {
        std::string command;
        while (std::getline(std::cin, command)) {
            if (command == "quit" || command == "exit") {
                break;
            }
            if (command == "window") {
                hearth.open_window();
            } else if (command == "reset") {
                hearth.reset_rounds();
            } else if (command == "cancel") {
                hearth.cancel_pairing();
            } else if (command == "status") {
                log.line(hearth.status_line());
            } else if (!command.empty()) {
                log.line("commands: window, reset, cancel, status, quit");
            }
        }
        // Standard input closed, as it is when run in the background: keep playing until killed.
        if (std::cin.eof() && !run_for) {
            while (true) {
                std::this_thread::sleep_for(1h);
            }
        }
        quit = true;
    });
    reader.detach();

    const auto began = std::chrono::steady_clock::now();
    auto last_state = hearth.status().state;
    auto last_printed = began;
    while (!quit.load()) {
        std::this_thread::sleep_for(100ms);
        const auto now = std::chrono::steady_clock::now();
        if (run_for && now - began >= *run_for) {
            break;
        }
        const player::PlayerStatus status = hearth.status();
        const bool playing = status.state == "playing";
        if (status_every.count() > 0 && ((playing && now - last_printed >= status_every) || status.state != last_state)) {
            log.line(hearth.status_line());
            last_printed = now;
        }
        last_state = status.state;
    }
    log.line("final: " + hearth.status_line());
    return EXIT_SUCCESS;
}
