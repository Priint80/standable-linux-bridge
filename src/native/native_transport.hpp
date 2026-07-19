#pragma once

#include "bridge_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include <netinet/in.h>

namespace standable::native {

struct ReceivedPacket {
    bridge::MessageType type{};
    std::uint64_t sequence{};
    std::uint32_t payload_size{};
    std::array<std::uint8_t, bridge::kMaxPacketBytes - sizeof(bridge::PacketHeader)> payload{};
};

class NativeTransport {
public:
    NativeTransport() = default;
    NativeTransport(const NativeTransport&) = delete;
    NativeTransport& operator=(const NativeTransport&) = delete;
    ~NativeTransport();

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
    [[nodiscard]] bool IsOpen() const noexcept { return socket_ >= 0; }
    [[nodiscard]] std::uint64_t Session() const noexcept { return session_; }

private:
    bool SendRaw(bridge::MessageType type, const void* payload, std::size_t payload_size);

    int socket_{-1};
    sockaddr_in remote_{};
    std::uint16_t remote_port_{};
    std::uint64_t session_{};
    std::uint64_t next_sequence_{1};
};

} // namespace standable::native
