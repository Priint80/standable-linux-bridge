#include "bridge_protocol.hpp"
#include "native_transport.hpp"
#include "relay_tracker.hpp"

#include <openvr_driver.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <dlfcn.h>
#include <spawn.h>
#include <unistd.h>

extern char** environ;

namespace standable::native {

namespace {

#ifndef STANDABLE_BRIDGE_VERSION
#define STANDABLE_BRIDGE_VERSION "development"
#endif

constexpr char kBuildVersion[] = STANDABLE_BRIDGE_VERSION;
constexpr std::size_t kMaxRelayTrackers = 16;

using Clock = std::chrono::steady_clock;

void Log(const std::string& message) {
    const std::string decorated = "[standable-linux] " + message;
    if (vr::VRDriverLog() != nullptr) {
        vr::VRDriverLog()->Log(decorated.c_str());
    }
}

std::uint16_t PortFromEnvironment(const char* name, std::uint16_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }

    std::uint32_t parsed = 0;
    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return fallback;
        }
        const auto digit = static_cast<std::uint32_t>(*cursor - '0');
        if (parsed > (65535U - digit) / 10U) {
            return fallback;
        }
        parsed = parsed * 10U + digit;
    }
    if (parsed == 0) {
        return fallback;
    }
    return static_cast<std::uint16_t>(parsed);
}

std::uint64_t CreateSessionId() {
    timespec now{};
    static_cast<void>(clock_gettime(CLOCK_MONOTONIC, &now));
    const auto address = reinterpret_cast<std::uintptr_t>(&now);
    std::uint64_t value = static_cast<std::uint64_t>(now.tv_sec) << 32U;
    value ^= static_cast<std::uint64_t>(now.tv_nsec);
    value ^= static_cast<std::uint64_t>(::getpid()) << 17U;
    value ^= static_cast<std::uint64_t>(address);
    value ^= value >> 29U;
    value *= 0x9E3779B185EBCA87ULL;
    return value == 0 ? 1 : value;
}

std::filesystem::path DiscoverDriverRoot() {
    Dl_info info{};
    if (::dladdr(reinterpret_cast<const void*>(&DiscoverDriverRoot), &info) == 0 || info.dli_fname == nullptr) {
        return {};
    }
    std::error_code error;
    auto binary = std::filesystem::weakly_canonical(info.dli_fname, error);
    if (error) {
        binary = info.dli_fname;
    }
    // <driver-root>/bin/linux64/driver_standable.so
    return binary.parent_path().parent_path().parent_path();
}

bridge::WireTrackedPose ToWire(const vr::TrackedDevicePose_t& pose) {
    bridge::WireTrackedPose wire{};
    std::size_t offset = 0;
    for (const auto& row : pose.mDeviceToAbsoluteTracking.m) {
        for (float value : row) {
            wire.device_to_absolute[offset++] = value;
        }
    }
    std::copy(std::begin(pose.vVelocity.v), std::end(pose.vVelocity.v), wire.velocity.begin());
    std::copy(std::begin(pose.vAngularVelocity.v), std::end(pose.vAngularVelocity.v), wire.angular_velocity.begin());
    wire.tracking_result = static_cast<std::int32_t>(pose.eTrackingResult);
    wire.pose_is_valid = pose.bPoseIsValid ? 1 : 0;
    wire.device_is_connected = pose.bDeviceIsConnected ? 1 : 0;
    return wire;
}

template <typename Payload>
bool ReadPayload(const ReceivedPacket& packet, Payload& payload) {
    if (packet.payload_size != sizeof(Payload)) {
        return false;
    }
    std::memcpy(&payload, packet.payload.data(), sizeof(payload));
    return true;
}

constexpr std::array<vr::ETrackedDeviceProperty, 51> kMirroredProperties{
    vr::Prop_TrackingSystemName_String,
    vr::Prop_ModelNumber_String,
    vr::Prop_SerialNumber_String,
    vr::Prop_RenderModelName_String,
    vr::Prop_WillDriftInYaw_Bool,
    vr::Prop_ManufacturerName_String,
    vr::Prop_TrackingFirmwareVersion_String,
    vr::Prop_HardwareRevision_String,
    vr::Prop_DeviceIsWireless_Bool,
    vr::Prop_DeviceIsCharging_Bool,
    vr::Prop_DeviceBatteryPercentage_Float,
    vr::Prop_Firmware_UpdateAvailable_Bool,
    vr::Prop_Firmware_ManualUpdate_Bool,
    vr::Prop_HardwareRevision_Uint64,
    vr::Prop_FirmwareVersion_Uint64,
    vr::Prop_BlockServerShutdown_Bool,
    vr::Prop_CanUnifyCoordinateSystemWithHmd_Bool,
    vr::Prop_ContainsProximitySensor_Bool,
    vr::Prop_DeviceProvidesBatteryStatus_Bool,
    vr::Prop_DeviceCanPowerOff_Bool,
    vr::Prop_DeviceClass_Int32,
    vr::Prop_DriverVersion_String,
    vr::Prop_ParentDriver_Uint64,
    vr::Prop_ResourceRoot_String,
    vr::Prop_RegisteredDeviceType_String,
    vr::Prop_InputProfilePath_String,
    vr::Prop_NeverTracked_Bool,
    vr::Prop_Identifiable_Bool,
    vr::Prop_ManufacturerSerialNumber_String,
    vr::Prop_ComputedSerialNumber_String,
    vr::Prop_ActualTrackingSystemName_String,
    vr::Prop_DisplayFrequency_Float,
    vr::Prop_UserIpdMeters_Float,
    vr::Prop_CurrentUniverseId_Uint64,
    vr::Prop_IsOnDesktop_Bool,
    vr::Prop_UserHeadToEyeDepthMeters_Float,
    vr::Prop_HmdTrackingStyle_Int32,
    vr::Prop_AttachedDeviceId_String,
    vr::Prop_SupportedButtons_Uint64,
    vr::Prop_Axis0Type_Int32,
    vr::Prop_Axis1Type_Int32,
    vr::Prop_Axis2Type_Int32,
    vr::Prop_Axis3Type_Int32,
    vr::Prop_Axis4Type_Int32,
    vr::Prop_ControllerRoleHint_Int32,
    vr::Prop_ModeLabel_String,
    vr::Prop_ParentContainer,
    vr::Prop_OverrideContainer_Uint64,
    vr::Prop_ControllerType_String,
    vr::Prop_ControllerHandSelectionPriority_Int32,
    vr::Prop_ExpectedControllerType_String,
};

} // namespace

class DeviceProvider final : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext* driver_context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(driver_context);
        root_ = DiscoverDriverRoot();
        session_ = CreateSessionId();
        native_port_ = PortFromEnvironment("STANDABLE_BRIDGE_NATIVE_PORT", bridge::kDefaultNativePort);
        helper_port_ = PortFromEnvironment("STANDABLE_BRIDGE_HELPER_PORT", bridge::kDefaultHelperPort);

        if (!transport_.Open(native_port_, helper_port_, session_)) {
            Log("Could not bind the local bridge socket on 127.0.0.1:" + std::to_string(native_port_));
            VR_CLEANUP_SERVER_DRIVER_CONTEXT();
            return vr::VRInitError_Driver_Failed;
        }

        initialized_ = true;
        last_hello_ = Clock::time_point{};
        last_heartbeat_ = Clock::time_point{};
        last_property_refresh_ = Clock::time_point{};
        last_helper_packet_ = Clock::time_point{};
        SendHello();
        StartHelper();
        Log("Native bridge initialized; waiting for the licensed Windows provider");
        return vr::VRInitError_None;
    }

    void Cleanup() override {
        if (!initialized_) {
            return;
        }
        bridge::ShutdownPayload shutdown{};
        transport_.Send(bridge::MessageType::Shutdown, shutdown);
        for (auto& tracker : trackers_) {
            if (tracker.IsConfigured()) {
                tracker.MarkDisconnected();
            }
        }
        transport_.Close();
        initialized_ = false;
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char* const* GetInterfaceVersions() override {
        return vr::k_InterfaceVersions;
    }

    void RunFrame() override {
        if (!initialized_) {
            return;
        }

        ReceiveFromHelper();
        SendPhysicalState();
        SendEvents();
        SendPeriodicControl();
        CheckHelperTimeout();
    }

    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    void SendHello() {
        bridge::HelloPayload hello{};
        hello.role = static_cast<std::uint32_t>(bridge::EndpointRole::NativeDriver);
        hello.process_id = static_cast<std::uint32_t>(::getpid());
        bridge::CopyString(hello.build, kBuildVersion);
        transport_.Send(bridge::MessageType::Hello, hello);
        last_hello_ = Clock::now();
    }

    void StartHelper() {
        const char* autostart = std::getenv("STANDABLE_BRIDGE_AUTOSTART");
        if (autostart != nullptr && std::strcmp(autostart, "0") == 0) {
            Log("Helper autostart disabled by STANDABLE_BRIDGE_AUTOSTART=0");
            return;
        }
        if (root_.empty()) {
            Log("Could not determine the Standable driver root; helper must be started manually");
            return;
        }

        const auto launcher = root_ / "scripts" / "standable-bridge-launcher.sh";
        if (!std::filesystem::exists(launcher)) {
            Log("Missing helper launcher: " + launcher.string());
            return;
        }

        char session_text[32]{};
        char native_port_text[16]{};
        char helper_port_text[16]{};
        std::snprintf(session_text, sizeof(session_text), "%llu", static_cast<unsigned long long>(session_));
        std::snprintf(native_port_text, sizeof(native_port_text), "%u", native_port_);
        std::snprintf(helper_port_text, sizeof(helper_port_text), "%u", helper_port_);

        std::string launcher_text = launcher.string();
        std::array<char*, 8> arguments{
            launcher_text.data(),
            const_cast<char*>("--session"),
            session_text,
            const_cast<char*>("--native-port"),
            native_port_text,
            const_cast<char*>("--helper-port"),
            helper_port_text,
            nullptr,
        };

        pid_t child = 0;
        const int result = ::posix_spawn(&child, launcher_text.c_str(), nullptr, nullptr, arguments.data(), environ);
        if (result != 0) {
            Log("Could not start the bridge launcher: error " + std::to_string(result));
            return;
        }
        Log("Started bridge launcher pid=" + std::to_string(child));
    }

    void ReceiveFromHelper() {
        ReceivedPacket packet{};
        for (std::size_t count = 0; count < 512 && transport_.Receive(packet); ++count) {
            last_helper_packet_ = Clock::now();
            bridge_online_ = true;
            HandlePacket(packet);
        }
    }

    void HandlePacket(const ReceivedPacket& packet) {
        switch (packet.type) {
        case bridge::MessageType::Hello: {
            bridge::HelloPayload hello{};
            if (ReadPayload(packet, hello)) {
                Log(std::string("Windows helper connected: ") + hello.build.data());
            }
            break;
        }
        case bridge::MessageType::Heartbeat:
            break;
        case bridge::MessageType::TrackerAdded: {
            bridge::TrackerAddedPayload added{};
            if (ReadPayload(packet, added)) {
                OnTrackerAdded(added);
            }
            break;
        }
        case bridge::MessageType::TrackerProperty: {
            bridge::PropertyPayload property{};
            if (ReadPayload(packet, property)) {
                if (auto* tracker = FindTrackerByRemote(property.device_index)) {
                    tracker->UpdateProperty(property);
                }
            }
            break;
        }
        case bridge::MessageType::TrackerPose: {
            bridge::TrackerPosePayload update{};
            if (ReadPayload(packet, update)) {
                if (auto* tracker = FindTrackerByRemote(update.remote_device_index)) {
                    tracker->UpdatePose(update.pose);
                    tracker->PublishPose();
                }
            }
            break;
        }
        case bridge::MessageType::ProviderStatus: {
            bridge::ProviderStatusPayload status{};
            if (ReadPayload(packet, status)) {
                Log("Windows provider state=" + std::to_string(status.state) +
                    " openvr_error=" + std::to_string(status.openvr_error) +
                    " detail=" + std::string(status.detail.data()));
            }
            break;
        }
        case bridge::MessageType::Log: {
            bridge::LogPayload log{};
            if (ReadPayload(packet, log)) {
                Log(std::string("[windows] ") + log.text.data());
            }
            break;
        }
        case bridge::MessageType::Shutdown:
            DisconnectTrackers();
            bridge_online_ = false;
            break;
        default:
            break;
        }
    }

    void OnTrackerAdded(const bridge::TrackerAddedPayload& added) {
        std::array<char, bridge::kMaxTrackerSerialBytes> serial = added.serial;
        serial.back() = '\0';

        RelayTracker* tracker = FindTrackerBySerial(serial.data());
        if (tracker == nullptr) {
            tracker = AcquireTracker();
            if (tracker == nullptr) {
                Log("Original DLL registered more trackers than the bridge can hold");
                return;
            }
            tracker->Configure(added.remote_device_index, added.device_class, serial.data());
            const auto device_class = static_cast<vr::ETrackedDeviceClass>(added.device_class);
            if (!vr::VRServerDriverHost()->TrackedDeviceAdded(serial.data(), device_class, tracker)) {
                Log("SteamVR rejected relayed tracker " + std::string(serial.data()));
                return;
            }
            Log("Relayed original tracker: " + std::string(serial.data()));
        } else if (tracker->RemoteDeviceIndex() != added.remote_device_index) {
            tracker->Configure(added.remote_device_index, added.device_class, serial.data());
        }
    }

    RelayTracker* AcquireTracker() {
        for (auto& tracker : trackers_) {
            if (!tracker.IsConfigured()) {
                return &tracker;
            }
        }
        return nullptr;
    }

    RelayTracker* FindTrackerByRemote(std::uint32_t remote_index) {
        for (auto& tracker : trackers_) {
            if (tracker.IsConfigured() && tracker.RemoteDeviceIndex() == remote_index) {
                return &tracker;
            }
        }
        return nullptr;
    }

    RelayTracker* FindTrackerBySerial(const char* serial) {
        for (auto& tracker : trackers_) {
            if (tracker.IsConfigured() && tracker.Serial() == serial) {
                return &tracker;
            }
        }
        return nullptr;
    }

    bool IsRelayObject(std::uint32_t index) const {
        for (const auto& tracker : trackers_) {
            if (tracker.IsConfigured() && tracker.ObjectId() == index) {
                return true;
            }
        }
        return false;
    }

    void SendPhysicalState() {
        std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
        vr::VRServerDriverHost()->GetRawTrackedDevicePoses(0.0F, poses.data(), poses.size());

        const auto now = Clock::now();
        const bool refresh_properties = last_property_refresh_ == Clock::time_point{} ||
            now - last_property_refresh_ >= std::chrono::seconds(5);

        for (std::uint32_t index = 0; index < poses.size(); ++index) {
            if (IsRelayObject(index)) {
                continue;
            }

            const auto& pose = poses[index];
            if (!pose.bDeviceIsConnected) {
                if (physical_connected_[index]) {
                    bridge::PhysicalDisconnectedPayload disconnected{index};
                    transport_.Send(bridge::MessageType::PhysicalDisconnected, disconnected);
                    physical_connected_[index] = false;
                }
                continue;
            }

            const auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(index);
            vr::ETrackedPropertyError error = vr::TrackedProp_Success;
            const auto device_class = vr::VRProperties()->GetInt32Property(
                container,
                vr::Prop_DeviceClass_Int32,
                &error);
            const auto controller_role = vr::VRProperties()->GetInt32Property(
                container,
                vr::Prop_ControllerRoleHint_Int32,
                &error);

            bridge::PhysicalPosePayload update{};
            update.device_index = index;
            update.device_class = device_class;
            update.controller_role = controller_role;
            update.pose = ToWire(pose);
            transport_.Send(bridge::MessageType::PhysicalPose, update);

            const bool newly_connected = !physical_connected_[index];
            physical_connected_[index] = true;
            if (newly_connected || refresh_properties) {
                SendProperties(index, container);
            }
        }

        if (refresh_properties) {
            last_property_refresh_ = now;
        }
    }

    void SendProperties(std::uint32_t index, vr::PropertyContainerHandle_t container) {
        for (const auto property : kMirroredProperties) {
            bridge::PropertyPayload outgoing{};
            outgoing.device_index = index;
            outgoing.property = static_cast<std::int32_t>(property);
            outgoing.write_type = static_cast<std::int32_t>(vr::PropertyWrite_Set);

            vr::PropertyRead_t read{};
            read.prop = property;
            read.pvBuffer = outgoing.data.data();
            read.unBufferSize = outgoing.data.size();
            static_cast<void>(vr::VRPropertiesRaw()->ReadPropertyBatch(container, &read, 1));
            if (read.eError != vr::TrackedProp_Success || read.unRequiredBufferSize > outgoing.data.size()) {
                continue;
            }
            outgoing.tag = read.unTag;
            outgoing.data_size = read.unRequiredBufferSize;
            transport_.Send(bridge::MessageType::PhysicalProperty, outgoing);
        }
    }

    void SendEvents() {
        vr::VREvent_t event{};
        for (std::size_t count = 0;
             count < 64 && vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event));
             ++count) {
            bridge::EventPayload outgoing{};
            outgoing.event_type = event.eventType;
            outgoing.tracked_device_index = event.trackedDeviceIndex;
            outgoing.event_age_seconds = event.eventAgeSeconds;
            outgoing.data_size = std::min<std::size_t>(sizeof(event.data), outgoing.data.size());
            std::memcpy(outgoing.data.data(), &event.data, outgoing.data_size);
            transport_.Send(bridge::MessageType::VrEvent, outgoing);
        }
    }

    void SendPeriodicControl() {
        const auto now = Clock::now();
        if (last_heartbeat_ == Clock::time_point{} || now - last_heartbeat_ >= std::chrono::milliseconds(250)) {
            bridge::HeartbeatPayload heartbeat{};
            heartbeat.monotonic_milliseconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
            transport_.Send(bridge::MessageType::Heartbeat, heartbeat);
            last_heartbeat_ = now;
        }
        if (last_hello_ == Clock::time_point{} || now - last_hello_ >= std::chrono::seconds(2)) {
            SendHello();
        }
    }

    void CheckHelperTimeout() {
        if (!bridge_online_ || last_helper_packet_ == Clock::time_point{}) {
            return;
        }
        if (Clock::now() - last_helper_packet_ >= std::chrono::seconds(3)) {
            Log("Windows helper timed out; marking relayed trackers disconnected");
            DisconnectTrackers();
            bridge_online_ = false;
        }
    }

    void DisconnectTrackers() {
        for (auto& tracker : trackers_) {
            if (tracker.IsConfigured()) {
                tracker.MarkDisconnected();
            }
        }
    }

    NativeTransport transport_;
    std::array<RelayTracker, kMaxRelayTrackers> trackers_{};
    std::array<bool, vr::k_unMaxTrackedDeviceCount> physical_connected_{};
    std::filesystem::path root_;
    std::uint64_t session_{};
    std::uint16_t native_port_{};
    std::uint16_t helper_port_{};
    Clock::time_point last_hello_{};
    Clock::time_point last_heartbeat_{};
    Clock::time_point last_property_refresh_{};
    Clock::time_point last_helper_packet_{};
    bool initialized_{false};
    bool bridge_online_{false};
};

DeviceProvider g_device_provider;

} // namespace standable::native

extern "C" __attribute__((visibility("default"))) void* HmdDriverFactory(
    const char* interface_name,
    int* return_code) {
    if (interface_name != nullptr &&
        std::strcmp(interface_name, vr::IServerTrackedDeviceProvider_Version) == 0) {
        if (return_code != nullptr) {
            *return_code = vr::VRInitError_None;
        }
        return &standable::native::g_device_provider;
    }

    if (return_code != nullptr) {
        *return_code = vr::VRInitError_Init_InterfaceNotFound;
    }
    return nullptr;
}
