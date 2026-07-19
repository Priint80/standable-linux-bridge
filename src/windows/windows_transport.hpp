#pragma once

#include "bridge_protocol.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace standable::windows {

struct ReceivedPacket {
    bridge::MessageType type{};
    std::uint64_t sequence{};
    std::uint32_t payload_size{};
    std::array<std::uint8_t, bridge::kMaxPacketBytes - sizeof(bridge::PacketHeader)> payload{};
};

class WindowsTransport {
public:
    WindowsTransport() = default;
    WindowsTransport(const WindowsTransport&) = delete;
    WindowsTransport& operator=(const WindowsTransport&) = delete;
    ~WindowsTransport();

    bool Open(std::uint16_t local_port, std::uint16_t remote_port, std::uint64_t session);
    void Close();

    template <typename Payload>
    bool Send(bridge::MessageType type, const Payload& payload) {
        static_assert(std::is_trivially_copyable_v<Payload>);
        return SendRaw(type, &payload, sizeof(payload));
    }

    bool SendEmpty(bridge::MessageType type) {
        return SendRaw(type, nullptr, 0);
    }

    bool Receive(ReceivedPacket& packet);
    [[nodiscard]] bool IsOpen() const noexcept { return socket_ != INVALID_SOCKET; }

private:
    bool SendRaw(bridge::MessageType type, const void* payload, std::size_t payload_size);

    SOCKET socket_{INVALID_SOCKET};
    sockaddr_in remote_{};
    std::uint16_t remote_port_{};
    std::uint64_t session_{};
    std::atomic<std::uint64_t> next_sequence_{1};
    std::mutex send_mutex_;
    bool winsock_started_{false};
};

} // namespace standable::windows
