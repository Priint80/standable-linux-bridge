#include "windows_transport.hpp"

#include <cstring>

namespace standable::windows {

WindowsTransport::~WindowsTransport() {
    Close();
}

bool WindowsTransport::Open(
    std::uint16_t local_port,
    std::uint16_t remote_port,
    std::uint64_t session) {
    Close();

    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }
    winsock_started_ = true;

    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        Close();
        return false;
    }

    u_long nonblocking = 1;
    if (::ioctlsocket(socket_, FIONBIO, &nonblocking) != 0) {
        Close();
        return false;
    }

    BOOL reuse = TRUE;
    static_cast<void>(::setsockopt(
        socket_,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse),
        sizeof(reuse)));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(local_port);
    if (::bind(socket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        Close();
        return false;
    }

    remote_ = {};
    remote_.sin_family = AF_INET;
    remote_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    remote_.sin_port = htons(remote_port);
    remote_port_ = remote_port;
    session_ = session;
    next_sequence_.store(1, std::memory_order_release);
    return true;
}

void WindowsTransport::Close() {
    if (socket_ != INVALID_SOCKET) {
        ::closesocket(socket_);
    }
    socket_ = INVALID_SOCKET;
    if (winsock_started_) {
        ::WSACleanup();
    }
    winsock_started_ = false;
    remote_ = {};
    remote_port_ = 0;
    session_ = 0;
}

bool WindowsTransport::SendRaw(
    bridge::MessageType type,
    const void* payload,
    std::size_t payload_size) {
    if (socket_ == INVALID_SOCKET ||
        payload_size > bridge::kMaxPacketBytes - sizeof(bridge::PacketHeader)) {
        return false;
    }

    std::lock_guard lock(send_mutex_);
    std::array<std::uint8_t, bridge::kMaxPacketBytes> datagram{};
    bridge::PacketHeader header{};
    header.magic = bridge::kMagic;
    header.version = bridge::kProtocolVersion;
    header.type = static_cast<std::uint16_t>(type);
    header.payload_size = static_cast<std::uint32_t>(payload_size);
    header.session = session_;
    header.sequence = next_sequence_.fetch_add(1, std::memory_order_acq_rel);
    header.checksum = bridge::Fnv1a(payload, payload_size);

    std::memcpy(datagram.data(), &header, sizeof(header));
    if (payload_size != 0) {
        std::memcpy(datagram.data() + sizeof(header), payload, payload_size);
    }

    const auto size = static_cast<int>(sizeof(header) + payload_size);
    const int sent = ::sendto(
        socket_,
        reinterpret_cast<const char*>(datagram.data()),
        size,
        0,
        reinterpret_cast<const sockaddr*>(&remote_),
        sizeof(remote_));
    return sent == size;
}

bool WindowsTransport::Receive(ReceivedPacket& packet) {
    if (socket_ == INVALID_SOCKET) {
        return false;
    }

    std::array<std::uint8_t, bridge::kMaxPacketBytes> datagram{};
    sockaddr_in source{};
    int source_size = sizeof(source);
    const int received = ::recvfrom(
        socket_,
        reinterpret_cast<char*>(datagram.data()),
        static_cast<int>(datagram.size()),
        0,
        reinterpret_cast<sockaddr*>(&source),
        &source_size);
    if (received == SOCKET_ERROR) {
        return false;
    }
    if (received < static_cast<int>(sizeof(bridge::PacketHeader))) {
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

} // namespace standable::windows
