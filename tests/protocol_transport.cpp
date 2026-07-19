#include "bridge_protocol.hpp"
#include "native_transport.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include <unistd.h>

namespace {

[[noreturn]] void Fail(const char* message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

bool WaitForPacket(
    standable::native::NativeTransport& transport,
    standable::native::ReceivedPacket& packet) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (transport.Receive(packet)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

int main() {
    using namespace standable;
    constexpr std::uint64_t session = 0x91D82E7A11223344ULL;
    const auto base = static_cast<std::uint16_t>(43000 + (::getpid() % 5000) * 2);
    const auto peer = static_cast<std::uint16_t>(base + 1);

    native::NativeTransport left;
    native::NativeTransport right;
    if (!left.Open(base, peer, session) || !right.Open(peer, base, session)) {
        Fail("could not open loopback test sockets");
    }

    bridge::HelloPayload hello{};
    hello.role = static_cast<std::uint32_t>(bridge::EndpointRole::NativeDriver);
    hello.process_id = static_cast<std::uint32_t>(::getpid());
    bridge::CopyString(hello.build, "transport-test");
    if (!left.Send(bridge::MessageType::Hello, hello)) {
        Fail("could not send Hello packet");
    }

    native::ReceivedPacket received{};
    if (!WaitForPacket(right, received)) {
        Fail("Hello packet did not arrive");
    }
    if (received.type != bridge::MessageType::Hello ||
        received.payload_size != sizeof(bridge::HelloPayload)) {
        Fail("Hello packet metadata changed in transit");
    }
    bridge::HelloPayload decoded{};
    std::memcpy(&decoded, received.payload.data(), sizeof(decoded));
    if (decoded.role != hello.role || decoded.process_id != hello.process_id ||
        std::strcmp(decoded.build.data(), "transport-test") != 0) {
        Fail("Hello payload changed in transit");
    }

    bridge::HeartbeatPayload heartbeat{123456789ULL};
    if (!right.Send(bridge::MessageType::Heartbeat, heartbeat) ||
        !WaitForPacket(left, received) ||
        received.type != bridge::MessageType::Heartbeat ||
        received.sequence != 1) {
        Fail("bidirectional packet relay failed");
    }

    bridge::PacketHeader header{};
    header.magic = bridge::kMagic;
    header.version = bridge::kProtocolVersion;
    header.payload_size = sizeof(heartbeat);
    header.session = session;
    if (!bridge::ValidateHeader(header, sizeof(header) + sizeof(heartbeat), session)) {
        Fail("valid protocol header was rejected");
    }
    if (bridge::ValidateHeader(header, sizeof(header) + sizeof(heartbeat), session + 1)) {
        Fail("wrong-session protocol header was accepted");
    }
    if (bridge::Fnv1a("abc", 3) != 0x1A47E90BU) {
        Fail("FNV-1a checksum implementation is incorrect");
    }

    std::cout << "PASS: authenticated loopback bridge protocol and bidirectional UDP transport\n";
    return EXIT_SUCCESS;
}
