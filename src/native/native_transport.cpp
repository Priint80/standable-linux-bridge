#include "native_transport.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace standable::native {

NativeTransport::~NativeTransport() {
    Close();
}

bool NativeTransport::Open(
    std::uint16_t local_port,
    std::uint16_t remote_port,
    std::uint64_t session) {
    Close();

    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ < 0) {
        return false;
    }

    const int flags = ::fcntl(socket_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
        Close();
        return false;
    }

    int enabled = 1;
    static_cast<void>(::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(local_port);
    if (::bind(socket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        Close();
        return false;
    }

    remote_ = {};
    remote_.sin_family = AF_INET;
    remote_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    remote_.sin_port = htons(remote_port);
    remote_port_ = remote_port;
    session_ = session;
    next_sequence_ = 1;
    return true;
}

void NativeTransport::Close() {
    if (socket_ >= 0) {
        ::close(socket_);
    }
    socket_ = -1;
    remote_ = {};
    remote_port_ = 0;
    session_ = 0;
    next_sequence_ = 1;
}

bool NativeTransport::SendRaw(
    bridge::MessageType type,
    const void* payload,
    std::size_t payload_size) {
    if (socket_ < 0 || payload_size > bridge::kMaxPacketBytes - sizeof(bridge::PacketHeader)) {
        return false;
    }

    std::array<std::uint8_t, bridge::kMaxPacketBytes> datagram{};
    bridge::PacketHeader header{};
    header.magic = bridge::kMagic;
    header.version = bridge::kProtocolVersion;
    header.type = static_cast<std::uint16_t>(type);
    header.payload_size = static_cast<std::uint32_t>(payload_size);
    header.session = session_;
    header.sequence = next_sequence_++;
    header.checksum = bridge::Fnv1a(payload, payload_size);

    std::memcpy(datagram.data(), &header, sizeof(header));
    if (payload_size != 0) {
        std::memcpy(datagram.data() + sizeof(header), payload, payload_size);
    }

    const auto sent = ::sendto(
        socket_,
        datagram.data(),
        sizeof(header) + payload_size,
        0,
        reinterpret_cast<const sockaddr*>(&remote_),
        sizeof(remote_));
    return sent == static_cast<ssize_t>(sizeof(header) + payload_size);
}

bool NativeTransport::Receive(ReceivedPacket& packet) {
    if (socket_ < 0) {
        return false;
    }

    std::array<std::uint8_t, bridge::kMaxPacketBytes> datagram{};
    sockaddr_in source{};
    socklen_t source_size = sizeof(source);
    const auto received = ::recvfrom(
        socket_,
        datagram.data(),
        datagram.size(),
        0,
        reinterpret_cast<sockaddr*>(&source),
        &source_size);
    if (received < 0) {
        return false;
    }
    if (received < static_cast<ssize_t>(sizeof(bridge::PacketHeader))) {
        return false;
    }
    if (source.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || ntohs(source.sin_port) != remote_port_) {
        return false;
    }

    bridge::PacketHeader header{};
    std::memcpy(&header, datagram.data(), sizeof(header));
    if (!bridge::ValidateHeader(header, static_cast<std::size_t>(received), session_)) {
        return false;
    }

    const auto* payload = datagram.data() + sizeof(header);
    if (bridge::Fnv1a(payload, header.payload_size) != header.checksum) {
        return false;
    }

    packet.type = static_cast<bridge::MessageType>(header.type);
    packet.sequence = header.sequence;
    packet.payload_size = header.payload_size;
    if (header.payload_size != 0) {
        std::memcpy(packet.payload.data(), payload, header.payload_size);
    }
    return true;
}

} // namespace standable::native
