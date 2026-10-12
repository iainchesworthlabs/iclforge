#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "iclforge/audio/udp_sender.hpp"
#include "udp_socket.hpp"

// UdpSender, the public sending half of the socket layer: a datagram goes out over real loopback and
// is received by a second UdpSocket the test binds on an ephemeral port, so it never races a fixed
// port number.

namespace {

using iclforge::audio::UdpSender;
using iclforge::audio::UdpSocket;

}  // namespace

TEST_CASE("UdpSender: a datagram sent to a loopback listener arrives whole", "[audio][udp]") {
    UdpSocket listener;
    REQUIRE(listener.bind("127.0.0.1", 0).has_value());
    const std::uint16_t port = listener.local_port();
    REQUIRE(port != 0);

    const std::array<std::byte, 8> datagram{std::byte{'/'}, std::byte{'a'}, std::byte{0}, std::byte{0},
                                            std::byte{','}, std::byte{'i'}, std::byte{0}, std::byte{0}};
    UdpSender sender;
    REQUIRE(sender.send_to("127.0.0.1", port, datagram));

    std::array<std::byte, 64> buffer{};
    const std::optional<std::size_t> got = listener.recv(buffer, 2000);
    REQUIRE(got.has_value());
    REQUIRE(*got == datagram.size());
    for (std::size_t i = 0; i < datagram.size(); ++i) {
        CHECK(buffer[i] == datagram[i]);
    }

    // Another datagram from the same sender is another datagram: the socket is kept.
    REQUIRE(sender.send_to("127.0.0.1", port, datagram));
    CHECK(listener.recv(buffer, 2000).has_value());
}

TEST_CASE("UdpSender: an address that is not an IPv4 literal is refused, not resolved", "[audio][udp]") {
    UdpSender sender;
    const std::array<std::byte, 4> datagram{};
    CHECK_FALSE(sender.send_to("localhost", 9000, datagram));
    CHECK_FALSE(sender.send_to("", 9000, datagram));
    CHECK_FALSE(sender.send_to("300.1.1.1", 9000, datagram));
}
