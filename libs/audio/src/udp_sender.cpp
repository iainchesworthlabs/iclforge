#include "iclforge/audio/udp_sender.hpp"

#include "net/udp_socket.hpp"

namespace iclforge::audio {

struct UdpSender::Impl {
    UdpSocket socket;
};

UdpSender::UdpSender() : impl_(std::make_unique<Impl>()) {}

UdpSender::~UdpSender() = default;

bool UdpSender::send_to(std::string_view ipv4_address, std::uint16_t port, std::span<const std::byte> datagram) {
    return impl_->socket.send_to(ipv4_address, port, datagram);
}

}  // namespace iclforge::audio
