#include "bridge_protocol.hpp"

#include <openvr_driver.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using DriverFactory = void* (*)(const char*, int*);

[[noreturn]] void Fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

class TestRuntime final :
    public vr::IVRDriverContext,
    public vr::IVRServerDriverHost,
    public vr::IVRProperties,
    public vr::IVRSettings,
    public vr::IVRDriverLog,
    public vr::IVRDriverManager,
    public vr::IVRResources {
public:
    void* GetGenericInterface(const char* version, vr::EVRInitError* error) override {
        if (error != nullptr) {
            *error = vr::VRInitError_None;
        }
        if (version == nullptr) {
            return Missing(error);
        }
        if (std::strcmp(version, vr::IVRServerDriverHost_Version) == 0) {
            return static_cast<vr::IVRServerDriverHost*>(this);
        }
        if (std::strcmp(version, vr::IVRProperties_Version) == 0) {
            return static_cast<vr::IVRProperties*>(this);
        }
        if (std::strcmp(version, vr::IVRSettings_Version) == 0) {
            return static_cast<vr::IVRSettings*>(this);
        }
        if (std::strcmp(version, vr::IVRDriverLog_Version) == 0) {
            return static_cast<vr::IVRDriverLog*>(this);
        }
        if (std::strcmp(version, vr::IVRDriverManager_Version) == 0) {
            return static_cast<vr::IVRDriverManager*>(this);
        }
        if (std::strcmp(version, vr::IVRResources_Version) == 0) {
            return static_cast<vr::IVRResources*>(this);
        }
        return Missing(error);
    }

    vr::DriverHandle_t GetDriverHandle() override { return 1; }

    bool TrackedDeviceAdded(
        const char* serial,
        vr::ETrackedDeviceClass device_class,
        vr::ITrackedDeviceServerDriver* driver) override {
        serial_ = serial == nullptr ? "" : serial;
        device_class_ = device_class;
        driver_ = driver;
        if (driver_ == nullptr) {
            return false;
        }
        return driver_->Activate(kObjectId) == vr::VRInitError_None;
    }

    void TrackedDevicePoseUpdated(
        std::uint32_t device,
        const vr::DriverPose_t& pose,
        std::uint32_t pose_size) override {
        if (device == kObjectId && pose_size >= sizeof(pose)) {
            last_pose_ = pose;
            pose_seen_ = true;
        }
    }

    void VsyncEvent(double) override {}
    void VendorSpecificEvent(
        std::uint32_t,
        vr::EVREventType,
        const vr::VREvent_Data_t&,
        double) override {}
    bool IsExiting() override { return false; }
    bool PollNextEvent(vr::VREvent_t*, std::uint32_t) override { return false; }

    void GetRawTrackedDevicePoses(
        float,
        vr::TrackedDevicePose_t* poses,
        std::uint32_t count) override {
        if (poses != nullptr) {
            std::fill_n(poses, count, vr::TrackedDevicePose_t{});
        }
    }

    void RequestRestart(const char*, const char*, const char*, const char*) override {}
    std::uint32_t GetFrameTimings(vr::Compositor_FrameTiming*, std::uint32_t) override { return 0; }
    void SetDisplayEyeToHead(std::uint32_t, const vr::HmdMatrix34_t&, const vr::HmdMatrix34_t&) override {}
    void SetDisplayProjectionRaw(std::uint32_t, const vr::HmdRect2_t&, const vr::HmdRect2_t&) override {}
    void SetRecommendedRenderTargetSize(std::uint32_t, std::uint32_t, std::uint32_t) override {}

    vr::ETrackedPropertyError ReadPropertyBatch(
        vr::PropertyContainerHandle_t,
        vr::PropertyRead_t* batch,
        std::uint32_t count) override {
        for (std::uint32_t index = 0; batch != nullptr && index < count; ++index) {
            batch[index].eError = vr::TrackedProp_UnknownProperty;
            batch[index].unRequiredBufferSize = 0;
        }
        return vr::TrackedProp_UnknownProperty;
    }

    vr::ETrackedPropertyError WritePropertyBatch(
        vr::PropertyContainerHandle_t,
        vr::PropertyWrite_t* batch,
        std::uint32_t count) override {
        for (std::uint32_t index = 0; batch != nullptr && index < count; ++index) {
            auto& write = batch[index];
            write.eError = vr::TrackedProp_Success;
            if (write.prop == vr::Prop_ControllerRoleHint_Int32 &&
                write.writeType == vr::PropertyWrite_Set &&
                write.pvBuffer != nullptr && write.unBufferSize == sizeof(std::int32_t)) {
                std::memcpy(&role_hint_, write.pvBuffer, sizeof(role_hint_));
            }
        }
        return vr::TrackedProp_Success;
    }

    const char* GetPropErrorNameFromEnum(vr::ETrackedPropertyError) override { return "test"; }
    vr::PropertyContainerHandle_t TrackedDeviceToPropertyContainer(std::uint32_t device) override {
        return static_cast<vr::PropertyContainerHandle_t>(device) + 1;
    }

    const char* GetSettingsErrorNameFromEnum(vr::EVRSettingsError) override { return "test"; }
    void SetBool(const char*, const char*, bool, vr::EVRSettingsError* error) override { SettingsOk(error); }
    void SetInt32(const char*, const char*, std::int32_t, vr::EVRSettingsError* error) override { SettingsOk(error); }
    void SetFloat(const char*, const char*, float, vr::EVRSettingsError* error) override { SettingsOk(error); }
    void SetString(const char*, const char*, const char*, vr::EVRSettingsError* error) override { SettingsOk(error); }
    bool GetBool(const char*, const char*, vr::EVRSettingsError* error) override { SettingsOk(error); return false; }
    std::int32_t GetInt32(const char*, const char*, vr::EVRSettingsError* error) override { SettingsOk(error); return 0; }
    float GetFloat(const char*, const char*, vr::EVRSettingsError* error) override { SettingsOk(error); return 0.0F; }
    void GetString(const char*, const char*, char* value, std::uint32_t size, vr::EVRSettingsError* error) override {
        if (value != nullptr && size != 0) {
            value[0] = '\0';
        }
        SettingsOk(error);
    }
    void RemoveSection(const char*, vr::EVRSettingsError* error) override { SettingsOk(error); }
    void RemoveKeyInSection(const char*, const char*, vr::EVRSettingsError* error) override { SettingsOk(error); }

    void Log(const char* message) override {
        if (message != nullptr) {
            log_ += message;
            log_ += '\n';
        }
    }

    std::uint32_t GetDriverCount() const override { return 1; }
    std::uint32_t GetDriverName(vr::DriverId_t driver, char* value, std::uint32_t size) override {
        constexpr char name[] = "standable";
        if (driver != 0 || value == nullptr || size == 0) {
            return 0;
        }
        std::strncpy(value, name, size - 1);
        value[size - 1] = '\0';
        return sizeof(name);
    }
    vr::DriverHandle_t GetDriverHandle(const char* name) override {
        return name != nullptr && std::strcmp(name, "standable") == 0 ? 1 : 0;
    }
    bool IsEnabled(vr::DriverId_t driver) const override { return driver == 0; }

    std::uint32_t LoadSharedResource(const char*, char*, std::uint32_t) override { return 0; }
    std::uint32_t GetResourceFullPath(const char*, const char*, char* value, std::uint32_t size) override {
        if (value != nullptr && size != 0) {
            value[0] = '\0';
        }
        return 1;
    }

    [[nodiscard]] bool RelayWasCorrect() const {
        return serial_ == "STANDABLE-RELAY-TEST" &&
            device_class_ == vr::TrackedDeviceClass_GenericTracker &&
            driver_ != nullptr && role_hint_ == 3 && pose_seen_ &&
            last_pose_.deviceIsConnected && last_pose_.poseIsValid &&
            last_pose_.result == vr::TrackingResult_Running_OK &&
            last_pose_.vecPosition[0] == 1.25 &&
            last_pose_.vecPosition[1] == 2.5 &&
            last_pose_.vecPosition[2] == -3.75;
    }

private:
    static constexpr std::uint32_t kObjectId = 7;

    static void* Missing(vr::EVRInitError* error) {
        if (error != nullptr) {
            *error = vr::VRInitError_Init_InterfaceNotFound;
        }
        return nullptr;
    }

    static void SettingsOk(vr::EVRSettingsError* error) {
        if (error != nullptr) {
            *error = vr::VRSettingsError_None;
        }
    }

    std::string serial_;
    std::string log_;
    vr::ETrackedDeviceClass device_class_{vr::TrackedDeviceClass_Invalid};
    vr::ITrackedDeviceServerDriver* driver_{};
    vr::DriverPose_t last_pose_{};
    std::int32_t role_hint_{};
    bool pose_seen_{false};
};

int OpenPeer(std::uint16_t port) {
    const int socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket < 0) {
        Fail("socket failed");
    }
    timeval timeout{};
    timeout.tv_sec = 1;
    if (::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        Fail("setsockopt failed");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        Fail("bind failed");
    }
    return socket;
}

standable::bridge::PacketHeader ReceiveNativeHello(int socket) {
    std::array<std::uint8_t, standable::bridge::kMaxPacketBytes> data{};
    const auto size = ::recv(socket, data.data(), data.size(), 0);
    if (size < static_cast<ssize_t>(sizeof(standable::bridge::PacketHeader))) {
        Fail("native Hello did not arrive");
    }
    standable::bridge::PacketHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.magic != standable::bridge::kMagic ||
        header.version != standable::bridge::kProtocolVersion ||
        header.type != static_cast<std::uint16_t>(standable::bridge::MessageType::Hello) ||
        header.session == 0) {
        Fail("native Hello header was invalid");
    }
    const auto* payload = data.data() + sizeof(header);
    if (standable::bridge::Fnv1a(payload, header.payload_size) != header.checksum) {
        Fail("native Hello checksum was invalid");
    }
    return header;
}

template <typename Payload>
void SendPacket(
    int socket,
    std::uint16_t native_port,
    std::uint64_t session,
    std::uint64_t sequence,
    standable::bridge::MessageType type,
    const Payload& payload) {
    std::array<std::uint8_t, standable::bridge::kMaxPacketBytes> data{};
    standable::bridge::PacketHeader header{};
    header.magic = standable::bridge::kMagic;
    header.version = standable::bridge::kProtocolVersion;
    header.type = static_cast<std::uint16_t>(type);
    header.payload_size = sizeof(payload);
    header.session = session;
    header.sequence = sequence;
    header.checksum = standable::bridge::Fnv1a(&payload, sizeof(payload));
    std::memcpy(data.data(), &header, sizeof(header));
    std::memcpy(data.data() + sizeof(header), &payload, sizeof(payload));

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(native_port);
    const auto size = sizeof(header) + sizeof(payload);
    if (::sendto(
            socket,
            data.data(),
            size,
            0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination)) != static_cast<ssize_t>(size)) {
        Fail("sendto failed");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        Fail("usage: provider_relay_smoke /path/to/driver_standable.so");
    }

    const auto native_port = static_cast<std::uint16_t>(45000 + (::getpid() % 4000) * 2);
    const auto helper_port = static_cast<std::uint16_t>(native_port + 1);
    const std::string native_text = std::to_string(native_port);
    const std::string helper_text = std::to_string(helper_port);
    ::setenv("STANDABLE_BRIDGE_NATIVE_PORT", native_text.c_str(), 1);
    ::setenv("STANDABLE_BRIDGE_HELPER_PORT", helper_text.c_str(), 1);
    ::setenv("STANDABLE_BRIDGE_AUTOSTART", "0", 1);

    const int peer = OpenPeer(helper_port);
    void* library = ::dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        Fail(std::string("dlopen failed: ") + ::dlerror());
    }
    auto* factory = reinterpret_cast<DriverFactory>(::dlsym(library, "HmdDriverFactory"));
    if (factory == nullptr) {
        Fail("HmdDriverFactory was missing");
    }
    int factory_error = 0;
    auto* provider = static_cast<vr::IServerTrackedDeviceProvider*>(
        factory(vr::IServerTrackedDeviceProvider_Version, &factory_error));
    if (provider == nullptr) {
        Fail("factory rejected provider interface");
    }

    TestRuntime runtime;
    const auto init_error = provider->Init(static_cast<vr::IVRDriverContext*>(&runtime));
    if (init_error != vr::VRInitError_None) {
        Fail("provider Init failed with " + std::to_string(static_cast<int>(init_error)));
    }
    const auto hello_header = ReceiveNativeHello(peer);

    standable::bridge::HelloPayload helper_hello{};
    helper_hello.role = static_cast<std::uint32_t>(standable::bridge::EndpointRole::WindowsHelper);
    standable::bridge::CopyString(helper_hello.build, "relay-smoke");
    SendPacket(peer, native_port, hello_header.session, 1, standable::bridge::MessageType::Hello, helper_hello);

    standable::bridge::TrackerAddedPayload added{};
    added.remote_device_index = 48;
    added.device_class = vr::TrackedDeviceClass_GenericTracker;
    standable::bridge::CopyString(added.serial, "STANDABLE-RELAY-TEST");
    SendPacket(peer, native_port, hello_header.session, 2, standable::bridge::MessageType::TrackerAdded, added);

    standable::bridge::PropertyPayload property{};
    property.device_index = added.remote_device_index;
    property.property = vr::Prop_ControllerRoleHint_Int32;
    property.write_type = vr::PropertyWrite_Set;
    property.tag = vr::k_unInt32PropertyTag;
    property.data_size = sizeof(std::int32_t);
    const std::int32_t expected_role = 3;
    std::memcpy(property.data.data(), &expected_role, sizeof(expected_role));
    SendPacket(peer, native_port, hello_header.session, 3, standable::bridge::MessageType::TrackerProperty, property);

    standable::bridge::TrackerPosePayload pose{};
    pose.remote_device_index = added.remote_device_index;
    pose.pose.world_from_driver_rotation[0] = 1.0;
    pose.pose.driver_from_head_rotation[0] = 1.0;
    pose.pose.rotation[0] = 1.0;
    pose.pose.position = {1.25, 2.5, -3.75};
    pose.pose.tracking_result = vr::TrackingResult_Running_OK;
    pose.pose.pose_is_valid = 1;
    pose.pose.device_is_connected = 1;
    SendPacket(peer, native_port, hello_header.session, 4, standable::bridge::MessageType::TrackerPose, pose);

    provider->RunFrame();
    if (!runtime.RelayWasCorrect()) {
        Fail("tracker registration, property, or pose relay did not match");
    }

    provider->Cleanup();
    ::close(peer);
    if (::dlclose(library) != 0) {
        Fail("dlclose failed");
    }
    ::unsetenv("STANDABLE_BRIDGE_NATIVE_PORT");
    ::unsetenv("STANDABLE_BRIDGE_HELPER_PORT");
    ::unsetenv("STANDABLE_BRIDGE_AUTOSTART");

    std::cout << "PASS: provider initialization, tracker registration, property relay, and exact pose relay\n";
    return EXIT_SUCCESS;
}
