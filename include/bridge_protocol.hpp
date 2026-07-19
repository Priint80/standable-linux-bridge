#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace standable::bridge {

constexpr std::uint32_t kMagic = 0x45464253U; // "SBFE" on little-endian hosts.
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::uint16_t kDefaultNativePort = 42470;
constexpr std::uint16_t kDefaultHelperPort = 42471;
constexpr std::size_t kMaxPacketBytes = 4096;
constexpr std::size_t kMaxPropertyBytes = 1024;
constexpr std::size_t kMaxEventDataBytes = 64;
constexpr std::size_t kMaxLogBytes = 768;
constexpr std::size_t kMaxTrackerSerialBytes = 128;

enum class MessageType : std::uint16_t {
    Hello = 1,
    Heartbeat = 2,
    PhysicalPose = 3,
    PhysicalProperty = 4,
    PhysicalDisconnected = 5,
    VrEvent = 6,
    TrackerAdded = 20,
    TrackerProperty = 21,
    TrackerPose = 22,
    ProviderStatus = 23,
    Log = 24,
    Shutdown = 25,
};

enum class EndpointRole : std::uint32_t {
    NativeDriver = 1,
    WindowsHelper = 2,
};

enum class ProviderState : std::int32_t {
    Starting = 0,
    Ready = 1,
    AuthenticationFailed = 2,
    DriverLoadFailed = 3,
    ProviderInitFailed = 4,
    Stopping = 5,
};

#pragma pack(push, 1)

struct PacketHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t type;
    std::uint32_t payload_size;
    std::uint64_t session;
    std::uint64_t sequence;
    std::uint32_t checksum;
};

struct HelloPayload {
    std::uint32_t role;
    std::uint32_t process_id;
    std::array<char, 64> build;
};

struct HeartbeatPayload {
    std::uint64_t monotonic_milliseconds;
};

struct WireTrackedPose {
    std::array<float, 12> device_to_absolute;
    std::array<float, 3> velocity;
    std::array<float, 3> angular_velocity;
    std::int32_t tracking_result;
    std::uint8_t pose_is_valid;
    std::uint8_t device_is_connected;
};

struct PhysicalPosePayload {
    std::uint32_t device_index;
    std::int32_t device_class;
    std::int32_t controller_role;
    WireTrackedPose pose;
};

struct PhysicalDisconnectedPayload {
    std::uint32_t device_index;
};

struct WireDriverPose {
    double pose_time_offset;
    std::array<double, 4> world_from_driver_rotation;
    std::array<double, 3> world_from_driver_translation;
    std::array<double, 4> driver_from_head_rotation;
    std::array<double, 3> driver_from_head_translation;
    std::array<double, 3> position;
    std::array<double, 3> velocity;
    std::array<double, 3> acceleration;
    std::array<double, 4> rotation;
    std::array<double, 3> angular_velocity;
    std::array<double, 3> angular_acceleration;
    std::int32_t tracking_result;
    std::uint8_t pose_is_valid;
    std::uint8_t will_drift_in_yaw;
    std::uint8_t should_apply_head_model;
    std::uint8_t device_is_connected;
};

struct PropertyPayload {
    std::uint32_t device_index;
    std::int32_t property;
    std::int32_t write_type;
    std::int32_t set_error;
    std::uint32_t tag;
    std::uint32_t data_size;
    std::array<std::uint8_t, kMaxPropertyBytes> data;
};

struct EventPayload {
    std::uint32_t event_type;
    std::uint32_t tracked_device_index;
    float event_age_seconds;
    std::uint32_t data_size;
    std::array<std::uint8_t, kMaxEventDataBytes> data;
};

struct TrackerAddedPayload {
    std::uint32_t remote_device_index;
    std::int32_t device_class;
    std::array<char, kMaxTrackerSerialBytes> serial;
};

struct TrackerPosePayload {
    std::uint32_t remote_device_index;
    WireDriverPose pose;
};

struct ProviderStatusPayload {
    std::int32_t state;
    std::int32_t openvr_error;
    std::array<char, 256> detail;
};

struct LogPayload {
    std::array<char, kMaxLogBytes> text;
};

struct ShutdownPayload {
    std::int32_t reason;
};

#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 32);
static_assert(sizeof(HelloPayload) == 72);
static_assert(sizeof(HeartbeatPayload) == 8);
static_assert(sizeof(WireTrackedPose) == 78);
static_assert(sizeof(PhysicalPosePayload) == 90);
static_assert(sizeof(PhysicalDisconnectedPayload) == 4);
static_assert(sizeof(WireDriverPose) == 280);
static_assert(sizeof(PropertyPayload) == 1048);
static_assert(sizeof(EventPayload) == 80);
static_assert(sizeof(TrackerAddedPayload) == 136);
static_assert(sizeof(TrackerPosePayload) == 284);
static_assert(sizeof(ProviderStatusPayload) == 264);
static_assert(sizeof(LogPayload) == 768);
static_assert(sizeof(ShutdownPayload) == 4);
static_assert(sizeof(PropertyPayload) + sizeof(PacketHeader) < kMaxPacketBytes);
static_assert(std::endian::native == std::endian::little);
static_assert(std::is_trivially_copyable_v<PacketHeader>);
static_assert(std::is_trivially_copyable_v<WireTrackedPose>);
static_assert(std::is_trivially_copyable_v<WireDriverPose>);

inline std::uint32_t Fnv1a(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t hash = 2166136261U;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 16777619U;
    }
    return hash;
}

inline bool ValidateHeader(
    const PacketHeader& header,
    std::size_t datagram_size,
    std::uint64_t expected_session) noexcept {
    if (header.magic != kMagic || header.version != kProtocolVersion) {
        return false;
    }
    if (header.session != expected_session) {
        return false;
    }
    if (header.payload_size > kMaxPacketBytes - sizeof(PacketHeader)) {
        return false;
    }
    return datagram_size == sizeof(PacketHeader) + header.payload_size;
}

template <std::size_t Size>
inline void CopyString(std::array<char, Size>& destination, const char* source) noexcept {
    destination.fill('\0');
    if (source == nullptr || Size == 0) {
        return;
    }
    std::strncpy(destination.data(), source, Size - 1);
}

} // namespace standable::bridge
