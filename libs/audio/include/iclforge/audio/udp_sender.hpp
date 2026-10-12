#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

// A UDP datagram out: the sending half of the socket layer LivePositionSource listens on
// (src/net/udp_socket.hpp, which is internal), published so that a program that has something to
// tell a lighting rig or a show-control desk over OSC does not carry a second socket
// implementation. Crucible's lyric events are the first such program.
//
// IPv4 only, and the address is a dotted-quad literal, never a hostname: no resolver, nothing that
// can block on DNS from a thread that is sending a lyric event. Fire-and-forget: UDP has no
// delivery, and a send that the operating system accepted is all this can say.

namespace iclforge::audio {

class UdpSender {
public:
    UdpSender();
    ~UdpSender();
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;
    UdpSender(UdpSender&&) = delete;
    UdpSender& operator=(UdpSender&&) = delete;

    // Sends one datagram to `ipv4_address`:`port` ("127.0.0.1", "192.168.1.20"). True when the
    // operating system accepted it; false for an address that does not parse, a socket that could
    // not be made, or a refused send. The socket is made on the first send, with an ephemeral
    // local port.
    [[nodiscard]] bool send_to(std::string_view ipv4_address, std::uint16_t port, std::span<const std::byte> datagram);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iclforge::audio
