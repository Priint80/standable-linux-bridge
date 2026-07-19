#include "fake_openvr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include <windows.h>

namespace standable::windows {

namespace {

#ifndef STANDABLE_BRIDGE_VERSION
#define STANDABLE_BRIDGE_VERSION "development"
#endif

thread_local bool g_injecting_physical = false;

__declspec(noinline) bool InvokeTrackedDeviceAdded(
    vr::IVRServerDriverHost* host,
    const char* serial,
    vr::ETrackedDeviceClass device_class,
    vr::ITrackedDeviceServerDriver* driver) {
    return host->TrackedDeviceAdded(serial, device_class, driver);
}

__declspec(noinline) void InvokeTrackedDevicePoseUpdated(
    vr::IVRServerDriverHost* host,
    std::uint32_t index,
    const vr::DriverPose_t& pose) {
    host->TrackedDevicePoseUpdated(index, pose, sizeof(pose));
}

template <typename Payload>
bool ReadPayload(const ReceivedPacket& packet, Payload& payload) {
    if (packet.payload_size != sizeof(Payload)) {
        return false;
    }
    std::memcpy(&payload, packet.payload.data(), sizeof(payload));
    return true;
}

vr::HmdQuaternion_t QuaternionFromMatrix(const vr::HmdMatrix34_t& matrix) {
    const double m00 = matrix.m[0][0];
    const double m11 = matrix.m[1][1];
    const double m22 = matrix.m[2][2];
    vr::HmdQuaternion_t quaternion{};
    const double trace = m00 + m11 + m22;
    if (trace > 0.0) {
        const double scale = std::sqrt(trace + 1.0) * 2.0;
        quaternion.w = 0.25 * scale;
        quaternion.x = (matrix.m[2][1] - matrix.m[1][2]) / scale;
        quaternion.y = (matrix.m[0][2] - matrix.m[2][0]) / scale;
        quaternion.z = (matrix.m[1][0] - matrix.m[0][1]) / scale;
    } else if (m00 > m11 && m00 > m22) {
        const double scale = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        quaternion.w = (matrix.m[2][1] - matrix.m[1][2]) / scale;
        quaternion.x = 0.25 * scale;
        quaternion.y = (matrix.m[0][1] + matrix.m[1][0]) / scale;
        quaternion.z = (matrix.m[0][2] + matrix.m[2][0]) / scale;
    } else if (m11 > m22) {
        const double scale = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        quaternion.w = (matrix.m[0][2] - matrix.m[2][0]) / scale;
        quaternion.x = (matrix.m[0][1] + matrix.m[1][0]) / scale;
        quaternion.y = 0.25 * scale;
        quaternion.z = (matrix.m[1][2] + matrix.m[2][1]) / scale;
    } else {
        const double scale = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        quaternion.w = (matrix.m[1][0] - matrix.m[0][1]) / scale;
        quaternion.x = (matrix.m[0][2] + matrix.m[2][0]) / scale;
        quaternion.y = (matrix.m[1][2] + matrix.m[2][1]) / scale;
        quaternion.z = 0.25 * scale;
    }
    return quaternion;
}

} // namespace

void PhysicalDeviceStub::Configure(OpenVrHost* host, std::uint32_t index) {
    host_ = host;
    index_ = index;
}

vr::EVRInitError PhysicalDeviceStub::Activate(std::uint32_t object_id) {
    static_cast<void>(object_id);
    return vr::VRInitError_None;
}

void PhysicalDeviceStub::Deactivate() {}
void PhysicalDeviceStub::EnterStandby() {}

void* PhysicalDeviceStub::GetComponent(const char* component_name_and_version) {
    static_cast<void>(component_name_and_version);
    return nullptr;
}

void PhysicalDeviceStub::DebugRequest(
    const char* request,
    char* response,
    std::uint32_t response_size) {
    static_cast<void>(request);
    if (response != nullptr && response_size != 0) {
        response[0] = '\0';
    }
}

vr::DriverPose_t PhysicalDeviceStub::GetPose() {
    return host_ == nullptr ? vr::DriverPose_t{} : host_->CurrentPhysicalDriverPose(index_);
}

OpenVrHost::OpenVrHost(WindowsTransport& transport, std::filesystem::path driver_root)
    : transport_(transport), driver_root_(std::move(driver_root)) {
    trackers_.reserve(16);
    for (std::uint32_t index = 0; index < physical_stubs_.size(); ++index) {
        physical_stubs_[index].Configure(this, index);
    }

    SettingValue enabled{};
    enabled.type = SettingValue::Type::Boolean;
    enabled.boolean = true;
    settings_[SettingKey("driver_standable", "enable")] = enabled;
    settings_[SettingKey("steamvr", "activateMultipleDrivers")] = enabled;
}

void OpenVrHost::HandlePacket(const ReceivedPacket& packet) {
    switch (packet.type) {
    case bridge::MessageType::Hello: {
        bridge::HelloPayload hello{};
        if (ReadPayload(packet, hello)) {
            SendHello();
            ReplayTrackers();
        }
        break;
    }
    case bridge::MessageType::Heartbeat:
        break;
    case bridge::MessageType::PhysicalPose: {
        bridge::PhysicalPosePayload update{};
        if (ReadPayload(packet, update)) {
            OnPhysicalPose(update);
        }
        break;
    }
    case bridge::MessageType::PhysicalProperty: {
        bridge::PropertyPayload property{};
        if (ReadPayload(packet, property)) {
            OnPhysicalProperty(property);
        }
        break;
    }
    case bridge::MessageType::PhysicalDisconnected: {
        bridge::PhysicalDisconnectedPayload disconnected{};
        if (ReadPayload(packet, disconnected)) {
            OnPhysicalDisconnected(disconnected);
        }
        break;
    }
    case bridge::MessageType::VrEvent: {
        bridge::EventPayload event{};
        if (ReadPayload(packet, event)) {
            OnEvent(event);
        }
        break;
    }
    case bridge::MessageType::Shutdown:
        SignalShutdown();
        break;
    default:
        break;
    }
}

void OpenVrHost::SendHello() {
    bridge::HelloPayload hello{};
    hello.role = static_cast<std::uint32_t>(bridge::EndpointRole::WindowsHelper);
    hello.process_id = ::GetCurrentProcessId();
    bridge::CopyString(hello.build, STANDABLE_BRIDGE_VERSION);
    transport_.Send(bridge::MessageType::Hello, hello);
}

void OpenVrHost::SendHeartbeat() {
    bridge::HeartbeatPayload heartbeat{};
    heartbeat.monotonic_milliseconds = ::GetTickCount64();
    transport_.Send(bridge::MessageType::Heartbeat, heartbeat);
}

void OpenVrHost::SendProviderStatus(
    bridge::ProviderState state,
    std::int32_t error,
    const char* detail) {
    bridge::ProviderStatusPayload status{};
    status.state = static_cast<std::int32_t>(state);
    status.openvr_error = error;
    bridge::CopyString(status.detail, detail);
    transport_.Send(bridge::MessageType::ProviderStatus, status);
}

void OpenVrHost::ReplayTrackers() {
    std::vector<TrackerRegistration> trackers;
    {
        std::scoped_lock lock(state_mutex_);
        trackers = trackers_;
    }

    for (const auto& tracker : trackers) {
        bridge::TrackerAddedPayload added{};
        added.remote_device_index = tracker.index;
        added.device_class = tracker.device_class;
        bridge::CopyString(added.serial, tracker.serial.c_str());
        transport_.Send(bridge::MessageType::TrackerAdded, added);

        const auto container = TrackedDeviceToPropertyContainer(tracker.index);
        std::unordered_map<std::int32_t, StoredProperty> stored;
        {
            std::scoped_lock lock(property_mutex_);
            const auto found = properties_.find(container);
            if (found != properties_.end()) {
                stored = found->second;
            }
        }
        for (const auto& [property, value] : stored) {
            if (value.data.size() > bridge::kMaxPropertyBytes) {
                continue;
            }
            bridge::PropertyPayload outgoing{};
            outgoing.device_index = tracker.index;
            outgoing.property = property;
            outgoing.write_type = static_cast<std::int32_t>(vr::PropertyWrite_Set);
            outgoing.set_error = static_cast<std::int32_t>(value.error);
            outgoing.tag = value.tag;
            outgoing.data_size = static_cast<std::uint32_t>(value.data.size());
            std::copy(value.data.begin(), value.data.end(), outgoing.data.begin());
            transport_.Send(bridge::MessageType::TrackerProperty, outgoing);
        }
        if (tracker.has_pose) {
            bridge::TrackerPosePayload pose{};
            pose.remote_device_index = tracker.index;
            pose.pose = DriverPoseToWire(tracker.last_pose);
            transport_.Send(bridge::MessageType::TrackerPose, pose);
        }
    }
}

void OpenVrHost::EnablePhysicalInjection() {
    physical_injection_enabled_.store(true, std::memory_order_release);
    for (std::uint32_t index = 0; index < physical_stubs_.size(); ++index) {
        bool connected = false;
        {
            std::scoped_lock lock(state_mutex_);
            connected = physical_poses_[index].bDeviceIsConnected;
        }
        if (connected) {
            TryInjectPhysicalDevice(index);
            InjectPhysicalPose(index);
        }
    }
}

void OpenVrHost::SignalShutdown() {
    exiting_.store(true, std::memory_order_release);
}

vr::DriverPose_t OpenVrHost::CurrentPhysicalDriverPose(std::uint32_t index) const {
    vr::DriverPose_t driver_pose{};
    driver_pose.qWorldFromDriverRotation.w = 1.0;
    driver_pose.qDriverFromHeadRotation.w = 1.0;
    driver_pose.qRotation.w = 1.0;
    if (index >= physical_poses_.size()) {
        return driver_pose;
    }

    std::scoped_lock lock(state_mutex_);
    const auto& physical = physical_poses_[index];
    driver_pose.vecPosition[0] = physical.mDeviceToAbsoluteTracking.m[0][3];
    driver_pose.vecPosition[1] = physical.mDeviceToAbsoluteTracking.m[1][3];
    driver_pose.vecPosition[2] = physical.mDeviceToAbsoluteTracking.m[2][3];
    driver_pose.vecVelocity[0] = physical.vVelocity.v[0];
    driver_pose.vecVelocity[1] = physical.vVelocity.v[1];
    driver_pose.vecVelocity[2] = physical.vVelocity.v[2];
    driver_pose.vecAngularVelocity[0] = physical.vAngularVelocity.v[0];
    driver_pose.vecAngularVelocity[1] = physical.vAngularVelocity.v[1];
    driver_pose.vecAngularVelocity[2] = physical.vAngularVelocity.v[2];
    driver_pose.qRotation = QuaternionFromMatrix(physical.mDeviceToAbsoluteTracking);
    driver_pose.result = physical.eTrackingResult;
    driver_pose.poseIsValid = physical.bPoseIsValid;
    driver_pose.deviceIsConnected = physical.bDeviceIsConnected;
    return driver_pose;
}

void* OpenVrHost::GetGenericInterface(const char* interface_version, vr::EVRInitError* error) {
    if (error != nullptr) {
        *error = vr::VRInitError_None;
    }
    if (interface_version == nullptr) {
        if (error != nullptr) {
            *error = vr::VRInitError_Init_InterfaceNotFound;
        }
        return nullptr;
    }
    if (std::strcmp(interface_version, vr::IVRServerDriverHost_Version) == 0 ||
        std::strcmp(interface_version, "IVRServerDriverHost_005") == 0) {
        return static_cast<vr::IVRServerDriverHost*>(this);
    }
    if (std::strcmp(interface_version, vr::IVRProperties_Version) == 0) {
        return static_cast<vr::IVRProperties*>(this);
    }
    if (std::strcmp(interface_version, vr::IVRSettings_Version) == 0) {
        return static_cast<vr::IVRSettings*>(this);
    }
    if (std::strcmp(interface_version, vr::IVRDriverLog_Version) == 0) {
        return static_cast<vr::IVRDriverLog*>(this);
    }
    if (std::strcmp(interface_version, vr::IVRResources_Version) == 0) {
        return static_cast<vr::IVRResources*>(this);
    }
    if (std::strcmp(interface_version, vr::IVRDriverInput_Version) == 0) {
        return static_cast<vr::IVRDriverInput*>(this);
    }
    if (std::strcmp(interface_version, vr::IVRDriverManager_Version) == 0) {
        return static_cast<vr::IVRDriverManager*>(this);
    }

    if (error != nullptr) {
        *error = vr::VRInitError_Init_InterfaceNotFound;
    }
    Log((std::string("Unsupported OpenVR interface requested: ") + interface_version).c_str());
    return nullptr;
}

vr::DriverHandle_t OpenVrHost::GetDriverHandle() {
    return 1;
}

bool OpenVrHost::TrackedDeviceAdded(
    const char* serial,
    vr::ETrackedDeviceClass device_class,
    vr::ITrackedDeviceServerDriver* driver) {
    if (g_injecting_physical) {
        return true;
    }
    if (serial == nullptr || *serial == '\0' || driver == nullptr) {
        return false;
    }

    std::uint32_t index = 0;
    {
        std::scoped_lock lock(state_mutex_);
        const auto existing = tracker_indices_.find(driver);
        if (existing != tracker_indices_.end()) {
            return true;
        }
        if (next_tracker_index_ >= vr::k_unMaxTrackedDeviceCount) {
            return false;
        }
        index = next_tracker_index_++;
        TrackerRegistration registration{};
        registration.index = index;
        registration.device_class = static_cast<std::int32_t>(device_class);
        registration.serial = serial;
        registration.driver = driver;
        trackers_.push_back(registration);
        tracker_indices_[driver] = index;
    }

    bridge::TrackerAddedPayload added{};
    added.remote_device_index = index;
    added.device_class = static_cast<std::int32_t>(device_class);
    bridge::CopyString(added.serial, serial);
    transport_.Send(bridge::MessageType::TrackerAdded, added);

    const auto activation_error = driver->Activate(index);
    if (activation_error != vr::VRInitError_None) {
        Log((std::string("Tracker activation failed for ") + serial).c_str());
        return false;
    }
    Log((std::string("Original DLL registered tracker ") + serial).c_str());
    return true;
}

void OpenVrHost::TrackedDevicePoseUpdated(
    std::uint32_t device_index,
    const vr::DriverPose_t& pose,
    std::uint32_t pose_size) {
    if (g_injecting_physical || pose_size < sizeof(vr::DriverPose_t)) {
        return;
    }

    {
        std::scoped_lock lock(state_mutex_);
        auto* tracker = FindTracker(device_index);
        if (tracker == nullptr) {
            return;
        }
        tracker->last_pose = pose;
        tracker->has_pose = true;
    }

    bridge::TrackerPosePayload update{};
    update.remote_device_index = device_index;
    update.pose = DriverPoseToWire(pose);
    transport_.Send(bridge::MessageType::TrackerPose, update);
}

void OpenVrHost::VsyncEvent(double time_offset) {
    static_cast<void>(time_offset);
}

void OpenVrHost::VendorSpecificEvent(
    std::uint32_t device_index,
    vr::EVREventType event_type,
    const vr::VREvent_Data_t& event_data,
    double time_offset) {
    static_cast<void>(device_index);
    static_cast<void>(event_type);
    static_cast<void>(event_data);
    static_cast<void>(time_offset);
}

bool OpenVrHost::IsExiting() {
    return ShouldExit();
}

bool OpenVrHost::PollNextEvent(vr::VREvent_t* event, std::uint32_t event_size) {
    if (event == nullptr || event_size < sizeof(vr::VREvent_t)) {
        return false;
    }
    std::scoped_lock lock(event_mutex_);
    if (events_.empty()) {
        return false;
    }
    *event = events_.front();
    events_.pop();
    return true;
}

void OpenVrHost::GetRawTrackedDevicePoses(
    float predicted_seconds,
    vr::TrackedDevicePose_t* poses,
    std::uint32_t pose_count) {
    static_cast<void>(predicted_seconds);
    if (poses == nullptr) {
        return;
    }
    std::scoped_lock lock(state_mutex_);
    const auto count = std::min<std::size_t>(pose_count, physical_poses_.size());
    std::copy_n(physical_poses_.begin(), count, poses);
    if (pose_count > count) {
        std::fill(poses + count, poses + pose_count, vr::TrackedDevicePose_t{});
    }
}

void OpenVrHost::RequestRestart(
    const char* localized_reason,
    const char* executable,
    const char* arguments,
    const char* working_directory) {
    static_cast<void>(executable);
    static_cast<void>(arguments);
    static_cast<void>(working_directory);
    Log((std::string("Original DLL requested SteamVR restart: ") +
         (localized_reason == nullptr ? "" : localized_reason)).c_str());
}

std::uint32_t OpenVrHost::GetFrameTimings(
    vr::Compositor_FrameTiming* timing,
    std::uint32_t frames) {
    static_cast<void>(timing);
    static_cast<void>(frames);
    return 0;
}

void OpenVrHost::SetDisplayEyeToHead(
    std::uint32_t device_index,
    const vr::HmdMatrix34_t& left,
    const vr::HmdMatrix34_t& right) {
    static_cast<void>(device_index);
    static_cast<void>(left);
    static_cast<void>(right);
}

void OpenVrHost::SetDisplayProjectionRaw(
    std::uint32_t device_index,
    const vr::HmdRect2_t& left,
    const vr::HmdRect2_t& right) {
    static_cast<void>(device_index);
    static_cast<void>(left);
    static_cast<void>(right);
}

void OpenVrHost::SetRecommendedRenderTargetSize(
    std::uint32_t device_index,
    std::uint32_t width,
    std::uint32_t height) {
    static_cast<void>(device_index);
    static_cast<void>(width);
    static_cast<void>(height);
}

vr::ETrackedPropertyError OpenVrHost::ReadPropertyBatch(
    vr::PropertyContainerHandle_t container,
    vr::PropertyRead_t* batch,
    std::uint32_t count) {
    if (batch == nullptr) {
        return vr::TrackedProp_InvalidOperation;
    }

    vr::ETrackedPropertyError overall = vr::TrackedProp_Success;
    std::scoped_lock lock(property_mutex_);
    const auto container_found = properties_.find(container);

    for (std::uint32_t index = 0; index < count; ++index) {
        auto& read = batch[index];
        read.unRequiredBufferSize = 0;
        if (container_found == properties_.end()) {
            read.eError = vr::TrackedProp_InvalidContainer;
            overall = read.eError;
            continue;
        }
        const auto property_found = container_found->second.find(static_cast<std::int32_t>(read.prop));
        if (property_found == container_found->second.end()) {
            read.eError = vr::TrackedProp_UnknownProperty;
            if (overall == vr::TrackedProp_Success) {
                overall = read.eError;
            }
            continue;
        }

        const auto& property = property_found->second;
        read.unTag = property.tag;
        read.unRequiredBufferSize = static_cast<std::uint32_t>(property.data.size());
        if (property.error != vr::TrackedProp_Success) {
            read.eError = property.error;
        } else if (read.unBufferSize < property.data.size() ||
                   (read.pvBuffer == nullptr && !property.data.empty())) {
            read.eError = vr::TrackedProp_BufferTooSmall;
        } else {
            if (!property.data.empty()) {
                std::memcpy(read.pvBuffer, property.data.data(), property.data.size());
            }
            read.eError = vr::TrackedProp_Success;
        }
        if (read.eError != vr::TrackedProp_Success && overall == vr::TrackedProp_Success) {
            overall = read.eError;
        }
    }
    return overall;
}

vr::ETrackedPropertyError OpenVrHost::WritePropertyBatch(
    vr::PropertyContainerHandle_t container,
    vr::PropertyWrite_t* batch,
    std::uint32_t count) {
    if (batch == nullptr) {
        return vr::TrackedProp_InvalidOperation;
    }

    const auto device_index = container == 0 ? vr::k_unTrackedDeviceIndexInvalid :
        static_cast<std::uint32_t>(container - 1);
    for (std::uint32_t index = 0; index < count; ++index) {
        auto& write = batch[index];
        {
            std::scoped_lock lock(property_mutex_);
            auto& values = properties_[container];
            if (write.writeType == vr::PropertyWrite_Erase) {
                values.erase(static_cast<std::int32_t>(write.prop));
            } else {
                StoredProperty stored{};
                stored.tag = write.unTag;
                stored.error = write.writeType == vr::PropertyWrite_SetError ?
                    write.eSetError : vr::TrackedProp_Success;
                if (write.writeType == vr::PropertyWrite_Set &&
                    write.pvBuffer != nullptr && write.unBufferSize != 0) {
                    const auto* bytes = static_cast<const std::uint8_t*>(write.pvBuffer);
                    stored.data.assign(bytes, bytes + write.unBufferSize);
                }
                values[static_cast<std::int32_t>(write.prop)] = std::move(stored);
            }
        }

        write.eError = vr::TrackedProp_Success;
        bool is_tracker = false;
        if (device_index != vr::k_unTrackedDeviceIndexInvalid) {
            std::scoped_lock lock(state_mutex_);
            is_tracker = FindTracker(device_index) != nullptr;
        }
        if (is_tracker) {
            RelayTrackerProperty(device_index, write);
        }
    }
    return vr::TrackedProp_Success;
}

const char* OpenVrHost::GetPropErrorNameFromEnum(vr::ETrackedPropertyError error) {
    switch (error) {
    case vr::TrackedProp_Success: return "TrackedProp_Success";
    case vr::TrackedProp_WrongDataType: return "TrackedProp_WrongDataType";
    case vr::TrackedProp_BufferTooSmall: return "TrackedProp_BufferTooSmall";
    case vr::TrackedProp_UnknownProperty: return "TrackedProp_UnknownProperty";
    case vr::TrackedProp_InvalidDevice: return "TrackedProp_InvalidDevice";
    case vr::TrackedProp_InvalidContainer: return "TrackedProp_InvalidContainer";
    default: return "TrackedProp_UnknownError";
    }
}

vr::PropertyContainerHandle_t OpenVrHost::TrackedDeviceToPropertyContainer(
    std::uint32_t device_index) {
    if (device_index == vr::k_unTrackedDeviceIndexInvalid) {
        return 0;
    }
    return static_cast<vr::PropertyContainerHandle_t>(device_index) + 1;
}

void OpenVrHost::OnPhysicalPose(const bridge::PhysicalPosePayload& update) {
    if (update.device_index >= physical_stubs_.size()) {
        return;
    }

    const auto pose = PhysicalPoseFromWire(update.pose);
    {
        std::scoped_lock lock(state_mutex_);
        physical_poses_[update.device_index] = pose;
        physical_classes_[update.device_index] = update.device_class;
    }
    StoreScalarProperty(
        update.device_index,
        vr::Prop_DeviceClass_Int32,
        vr::k_unInt32PropertyTag,
        &update.device_class,
        sizeof(update.device_class));
    StoreScalarProperty(
        update.device_index,
        vr::Prop_ControllerRoleHint_Int32,
        vr::k_unInt32PropertyTag,
        &update.controller_role,
        sizeof(update.controller_role));
    TryInjectPhysicalDevice(update.device_index);
    InjectPhysicalPose(update.device_index);
}

void OpenVrHost::OnPhysicalProperty(const bridge::PropertyPayload& property) {
    if (property.device_index >= physical_poses_.size()) {
        return;
    }
    StoreProperty(TrackedDeviceToPropertyContainer(property.device_index), property);
    if (property.property == static_cast<std::int32_t>(vr::Prop_SerialNumber_String) ||
        property.property == static_cast<std::int32_t>(vr::Prop_DeviceClass_Int32)) {
        TryInjectPhysicalDevice(property.device_index);
    }
}

void OpenVrHost::OnPhysicalDisconnected(
    const bridge::PhysicalDisconnectedPayload& disconnected) {
    if (disconnected.device_index >= physical_poses_.size()) {
        return;
    }
    {
        std::scoped_lock lock(state_mutex_);
        auto& pose = physical_poses_[disconnected.device_index];
        pose.bDeviceIsConnected = false;
        pose.bPoseIsValid = false;
        pose.eTrackingResult = vr::TrackingResult_Uninitialized;
        physical_injected_[disconnected.device_index] = false;
    }
    InjectPhysicalPose(disconnected.device_index);
}

void OpenVrHost::OnEvent(const bridge::EventPayload& event) {
    if (event.data_size > event.data.size()) {
        return;
    }
    vr::VREvent_t converted{};
    converted.eventType = event.event_type;
    converted.trackedDeviceIndex = event.tracked_device_index;
    converted.eventAgeSeconds = event.event_age_seconds;
    std::memcpy(&converted.data, event.data.data(),
        std::min<std::size_t>(event.data_size, sizeof(converted.data)));
    std::scoped_lock lock(event_mutex_);
    events_.push(converted);
}

void OpenVrHost::TryInjectPhysicalDevice(std::uint32_t index) {
    if (!physical_injection_enabled_.load(std::memory_order_acquire) ||
        index >= physical_stubs_.size()) {
        return;
    }

    const std::string serial = PhysicalSerial(index);
    std::int32_t device_class = 0;
    {
        std::scoped_lock lock(state_mutex_);
        if (!physical_poses_[index].bDeviceIsConnected || physical_injected_[index] || serial.empty()) {
            return;
        }
        physical_injected_[index] = true;
        device_class = physical_classes_[index];
    }

    g_injecting_physical = true;
    const bool accepted = InvokeTrackedDeviceAdded(
        static_cast<vr::IVRServerDriverHost*>(this),
        serial.c_str(),
        static_cast<vr::ETrackedDeviceClass>(device_class),
        &physical_stubs_[index]);
    g_injecting_physical = false;
    if (!accepted) {
        std::scoped_lock lock(state_mutex_);
        physical_injected_[index] = false;
    }
}

void OpenVrHost::InjectPhysicalPose(std::uint32_t index) {
    if (index >= physical_stubs_.size()) {
        return;
    }
    const auto pose = CurrentPhysicalDriverPose(index);
    g_injecting_physical = true;
    InvokeTrackedDevicePoseUpdated(static_cast<vr::IVRServerDriverHost*>(this), index, pose);
    g_injecting_physical = false;
}

std::string OpenVrHost::PhysicalSerial(std::uint32_t index) const {
    const auto container = static_cast<vr::PropertyContainerHandle_t>(index) + 1;
    std::scoped_lock lock(property_mutex_);
    const auto container_found = properties_.find(container);
    if (container_found == properties_.end()) {
        return {};
    }
    const auto property_found = container_found->second.find(
        static_cast<std::int32_t>(vr::Prop_SerialNumber_String));
    if (property_found == container_found->second.end() || property_found->second.data.empty()) {
        return {};
    }
    const auto& data = property_found->second.data;
    const auto* text = reinterpret_cast<const char*>(data.data());
    const auto length = std::find(data.begin(), data.end(), 0) - data.begin();
    return std::string(text, length);
}

void OpenVrHost::StoreProperty(
    std::uint64_t container,
    const bridge::PropertyPayload& property) {
    if (property.data_size > property.data.size()) {
        return;
    }
    std::scoped_lock lock(property_mutex_);
    auto& values = properties_[container];
    if (property.write_type == static_cast<std::int32_t>(vr::PropertyWrite_Erase)) {
        values.erase(property.property);
        return;
    }
    StoredProperty stored{};
    stored.tag = property.tag;
    stored.error = property.write_type == static_cast<std::int32_t>(vr::PropertyWrite_SetError) ?
        static_cast<vr::ETrackedPropertyError>(property.set_error) : vr::TrackedProp_Success;
    stored.data.assign(property.data.begin(), property.data.begin() + property.data_size);
    values[property.property] = std::move(stored);
}

void OpenVrHost::StoreScalarProperty(
    std::uint32_t index,
    vr::ETrackedDeviceProperty property,
    std::uint32_t tag,
    const void* data,
    std::size_t size) {
    bridge::PropertyPayload wire{};
    wire.device_index = index;
    wire.property = static_cast<std::int32_t>(property);
    wire.write_type = static_cast<std::int32_t>(vr::PropertyWrite_Set);
    wire.tag = tag;
    wire.data_size = static_cast<std::uint32_t>(std::min(size, wire.data.size()));
    if (wire.data_size != 0) {
        std::memcpy(wire.data.data(), data, wire.data_size);
    }
    StoreProperty(TrackedDeviceToPropertyContainer(index), wire);
}

void OpenVrHost::RelayTrackerProperty(
    std::uint32_t index,
    const vr::PropertyWrite_t& write) {
    if (write.unBufferSize > bridge::kMaxPropertyBytes) {
        Log("Skipping an oversized tracker property");
        return;
    }
    bridge::PropertyPayload outgoing{};
    outgoing.device_index = index;
    outgoing.property = static_cast<std::int32_t>(write.prop);
    outgoing.write_type = static_cast<std::int32_t>(write.writeType);
    outgoing.set_error = static_cast<std::int32_t>(write.eSetError);
    outgoing.tag = write.unTag;
    outgoing.data_size = write.unBufferSize;
    if (write.pvBuffer != nullptr && write.unBufferSize != 0) {
        std::memcpy(outgoing.data.data(), write.pvBuffer, write.unBufferSize);
    }
    transport_.Send(bridge::MessageType::TrackerProperty, outgoing);
}

OpenVrHost::TrackerRegistration* OpenVrHost::FindTracker(std::uint32_t index) {
    const auto found = std::find_if(trackers_.begin(), trackers_.end(),
        [index](const TrackerRegistration& tracker) { return tracker.index == index; });
    return found == trackers_.end() ? nullptr : &*found;
}

const OpenVrHost::TrackerRegistration* OpenVrHost::FindTracker(std::uint32_t index) const {
    const auto found = std::find_if(trackers_.begin(), trackers_.end(),
        [index](const TrackerRegistration& tracker) { return tracker.index == index; });
    return found == trackers_.end() ? nullptr : &*found;
}

vr::TrackedDevicePose_t OpenVrHost::PhysicalPoseFromWire(
    const bridge::WireTrackedPose& wire) {
    vr::TrackedDevicePose_t pose{};
    std::size_t offset = 0;
    for (auto& row : pose.mDeviceToAbsoluteTracking.m) {
        for (float& value : row) {
            value = wire.device_to_absolute[offset++];
        }
    }
    std::copy(wire.velocity.begin(), wire.velocity.end(), std::begin(pose.vVelocity.v));
    std::copy(wire.angular_velocity.begin(), wire.angular_velocity.end(), std::begin(pose.vAngularVelocity.v));
    pose.eTrackingResult = static_cast<vr::ETrackingResult>(wire.tracking_result);
    pose.bPoseIsValid = wire.pose_is_valid != 0;
    pose.bDeviceIsConnected = wire.device_is_connected != 0;
    return pose;
}

bridge::WireDriverPose OpenVrHost::DriverPoseToWire(const vr::DriverPose_t& pose) {
    bridge::WireDriverPose wire{};
    wire.pose_time_offset = pose.poseTimeOffset;
    wire.world_from_driver_rotation = {
        pose.qWorldFromDriverRotation.w,
        pose.qWorldFromDriverRotation.x,
        pose.qWorldFromDriverRotation.y,
        pose.qWorldFromDriverRotation.z};
    std::copy(std::begin(pose.vecWorldFromDriverTranslation), std::end(pose.vecWorldFromDriverTranslation), wire.world_from_driver_translation.begin());
    wire.driver_from_head_rotation = {
        pose.qDriverFromHeadRotation.w,
        pose.qDriverFromHeadRotation.x,
        pose.qDriverFromHeadRotation.y,
        pose.qDriverFromHeadRotation.z};
    std::copy(std::begin(pose.vecDriverFromHeadTranslation), std::end(pose.vecDriverFromHeadTranslation), wire.driver_from_head_translation.begin());
    std::copy(std::begin(pose.vecPosition), std::end(pose.vecPosition), wire.position.begin());
    std::copy(std::begin(pose.vecVelocity), std::end(pose.vecVelocity), wire.velocity.begin());
    std::copy(std::begin(pose.vecAcceleration), std::end(pose.vecAcceleration), wire.acceleration.begin());
    wire.rotation = {pose.qRotation.w, pose.qRotation.x, pose.qRotation.y, pose.qRotation.z};
    std::copy(std::begin(pose.vecAngularVelocity), std::end(pose.vecAngularVelocity), wire.angular_velocity.begin());
    std::copy(std::begin(pose.vecAngularAcceleration), std::end(pose.vecAngularAcceleration), wire.angular_acceleration.begin());
    wire.tracking_result = static_cast<std::int32_t>(pose.result);
    wire.pose_is_valid = pose.poseIsValid ? 1 : 0;
    wire.will_drift_in_yaw = pose.willDriftInYaw ? 1 : 0;
    wire.should_apply_head_model = pose.shouldApplyHeadModel ? 1 : 0;
    wire.device_is_connected = pose.deviceIsConnected ? 1 : 0;
    return wire;
}

std::string OpenVrHost::SettingKey(const char* section, const char* key) {
    return std::string(section == nullptr ? "" : section) + '\x1f' +
        (key == nullptr ? "" : key);
}

void OpenVrHost::SetSettingsError(
    vr::EVRSettingsError* error,
    vr::EVRSettingsError value) {
    if (error != nullptr) {
        *error = value;
    }
}

void OpenVrHost::CopyOutString(
    const std::string& source,
    char* destination,
    std::uint32_t capacity) {
    if (destination == nullptr || capacity == 0) {
        return;
    }
    const auto count = std::min<std::size_t>(source.size(), capacity - 1);
    std::memcpy(destination, source.data(), count);
    destination[count] = '\0';
}

const char* OpenVrHost::GetSettingsErrorNameFromEnum(vr::EVRSettingsError error) {
    switch (error) {
    case vr::VRSettingsError_None: return "VRSettingsError_None";
    case vr::VRSettingsError_IPCFailed: return "VRSettingsError_IPCFailed";
    case vr::VRSettingsError_WriteFailed: return "VRSettingsError_WriteFailed";
    case vr::VRSettingsError_ReadFailed: return "VRSettingsError_ReadFailed";
    case vr::VRSettingsError_JsonParseFailed: return "VRSettingsError_JsonParseFailed";
    case vr::VRSettingsError_UnsetSettingHasNoDefault: return "VRSettingsError_UnsetSettingHasNoDefault";
    case vr::VRSettingsError_AccessDenied: return "VRSettingsError_AccessDenied";
    default: return "VRSettingsError_Unknown";
    }
}

void OpenVrHost::SetBool(
    const char* section,
    const char* key,
    bool value,
    vr::EVRSettingsError* error) {
    SettingValue setting{};
    setting.type = SettingValue::Type::Boolean;
    setting.boolean = value;
    {
        std::scoped_lock lock(settings_mutex_);
        settings_[SettingKey(section, key)] = std::move(setting);
    }
    SetSettingsError(error, vr::VRSettingsError_None);
}

void OpenVrHost::SetInt32(
    const char* section,
    const char* key,
    std::int32_t value,
    vr::EVRSettingsError* error) {
    SettingValue setting{};
    setting.type = SettingValue::Type::Integer;
    setting.integer = value;
    {
        std::scoped_lock lock(settings_mutex_);
        settings_[SettingKey(section, key)] = std::move(setting);
    }
    SetSettingsError(error, vr::VRSettingsError_None);
}

void OpenVrHost::SetFloat(
    const char* section,
    const char* key,
    float value,
    vr::EVRSettingsError* error) {
    SettingValue setting{};
    setting.type = SettingValue::Type::Float;
    setting.floating = value;
    {
        std::scoped_lock lock(settings_mutex_);
        settings_[SettingKey(section, key)] = std::move(setting);
    }
    SetSettingsError(error, vr::VRSettingsError_None);
}

void OpenVrHost::SetString(
    const char* section,
    const char* key,
    const char* value,
    vr::EVRSettingsError* error) {
    SettingValue setting{};
    setting.type = SettingValue::Type::String;
    setting.string = value == nullptr ? "" : value;
    {
        std::scoped_lock lock(settings_mutex_);
        settings_[SettingKey(section, key)] = std::move(setting);
    }
    SetSettingsError(error, vr::VRSettingsError_None);
}

bool OpenVrHost::GetBool(
    const char* section,
    const char* key,
    vr::EVRSettingsError* error) {
    std::scoped_lock lock(settings_mutex_);
    const auto found = settings_.find(SettingKey(section, key));
    if (found == settings_.end() || found->second.type != SettingValue::Type::Boolean) {
        SetSettingsError(error, vr::VRSettingsError_UnsetSettingHasNoDefault);
        return false;
    }
    SetSettingsError(error, vr::VRSettingsError_None);
    return found->second.boolean;
}

std::int32_t OpenVrHost::GetInt32(
    const char* section,
    const char* key,
    vr::EVRSettingsError* error) {
    std::scoped_lock lock(settings_mutex_);
    const auto found = settings_.find(SettingKey(section, key));
    if (found == settings_.end() || found->second.type != SettingValue::Type::Integer) {
        SetSettingsError(error, vr::VRSettingsError_UnsetSettingHasNoDefault);
        return 0;
    }
    SetSettingsError(error, vr::VRSettingsError_None);
    return found->second.integer;
}

float OpenVrHost::GetFloat(
    const char* section,
    const char* key,
    vr::EVRSettingsError* error) {
    std::scoped_lock lock(settings_mutex_);
    const auto found = settings_.find(SettingKey(section, key));
    if (found == settings_.end() || found->second.type != SettingValue::Type::Float) {
        SetSettingsError(error, vr::VRSettingsError_UnsetSettingHasNoDefault);
        return 0.0F;
    }
    SetSettingsError(error, vr::VRSettingsError_None);
    return found->second.floating;
}

void OpenVrHost::GetString(
    const char* section,
    const char* key,
    char* value,
    std::uint32_t value_length,
    vr::EVRSettingsError* error) {
    std::scoped_lock lock(settings_mutex_);
    const auto found = settings_.find(SettingKey(section, key));
    if (found == settings_.end() || found->second.type != SettingValue::Type::String) {
        CopyOutString("", value, value_length);
        SetSettingsError(error, vr::VRSettingsError_UnsetSettingHasNoDefault);
        return;
    }
    CopyOutString(found->second.string, value, value_length);
    SetSettingsError(error, vr::VRSettingsError_None);
}

void OpenVrHost::RemoveSection(const char* section, vr::EVRSettingsError* error) {
    const std::string prefix = std::string(section == nullptr ? "" : section) + '\x1f';
    std::scoped_lock lock(settings_mutex_);
    for (auto iterator = settings_.begin(); iterator != settings_.end();) {
        if (iterator->first.rfind(prefix, 0) == 0) {
            iterator = settings_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    SetSettingsError(error, vr::VRSettingsError_None);
}

void OpenVrHost::RemoveKeyInSection(
    const char* section,
    const char* key,
    vr::EVRSettingsError* error) {
    std::scoped_lock lock(settings_mutex_);
    settings_.erase(SettingKey(section, key));
    SetSettingsError(error, vr::VRSettingsError_None);
}

void OpenVrHost::Log(const char* message) {
    const char* safe = message == nullptr ? "" : message;
    std::fprintf(stderr, "[standable-bridge/windows] %s\n", safe);
    std::fflush(stderr);
    bridge::LogPayload outgoing{};
    bridge::CopyString(outgoing.text, safe);
    transport_.Send(bridge::MessageType::Log, outgoing);
}

std::uint32_t OpenVrHost::LoadSharedResource(
    const char* resource_name,
    char* buffer,
    std::uint32_t buffer_length) {
    const auto path = ResolveResource(resource_name, nullptr);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return 0;
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) {
        return 0;
    }
    const auto required = static_cast<std::uint32_t>(end);
    if (buffer != nullptr && buffer_length >= required) {
        input.seekg(0, std::ios::beg);
        input.read(buffer, required);
    }
    return required;
}

std::uint32_t OpenVrHost::GetResourceFullPath(
    const char* resource_name,
    const char* resource_type_directory,
    char* path_buffer,
    std::uint32_t buffer_length) {
    const auto path = ResolveResource(resource_name, resource_type_directory).string();
    const auto required = static_cast<std::uint32_t>(path.size() + 1);
    if (path_buffer != nullptr && buffer_length != 0) {
        CopyOutString(path, path_buffer, buffer_length);
    }
    return required;
}

std::filesystem::path OpenVrHost::ResolveResource(
    const char* resource_name,
    const char* resource_type_directory) const {
    std::string name = resource_name == nullptr ? "" : resource_name;
    std::filesystem::path base = driver_root_ / "resources";
    constexpr char prefix[] = "{standable}/";
    if (name.rfind(prefix, 0) == 0) {
        name.erase(0, sizeof(prefix) - 1);
    } else if (resource_type_directory != nullptr && *resource_type_directory != '\0') {
        base /= resource_type_directory;
    }
    std::replace(name.begin(), name.end(), '/', '\\');
    return base / name;
}

vr::VRInputComponentHandle_t OpenVrHost::AllocateInputHandle(
    vr::VRInputComponentHandle_t* output) {
    const auto handle = next_input_handle_.fetch_add(1, std::memory_order_acq_rel);
    if (output != nullptr) {
        *output = handle;
    }
    return handle;
}

vr::EVRInputError OpenVrHost::CreateBooleanComponent(
    vr::PropertyContainerHandle_t container,
    const char* name,
    vr::VRInputComponentHandle_t* handle) {
    static_cast<void>(container);
    static_cast<void>(name);
    AllocateInputHandle(handle);
    return vr::VRInputError_None;
}

vr::EVRInputError OpenVrHost::UpdateBooleanComponent(
    vr::VRInputComponentHandle_t component,
    bool value,
    double time_offset) {
    static_cast<void>(component);
    static_cast<void>(value);
    static_cast<void>(time_offset);
    return vr::VRInputError_None;
}

vr::EVRInputError OpenVrHost::CreateScalarComponent(
    vr::PropertyContainerHandle_t container,
    const char* name,
    vr::VRInputComponentHandle_t* handle,
    vr::EVRScalarType type,
    vr::EVRScalarUnits units) {
    static_cast<void>(container);
    static_cast<void>(name);
    static_cast<void>(type);
    static_cast<void>(units);
    AllocateInputHandle(handle);
    return vr::VRInputError_None;
}

vr::EVRInputError OpenVrHost::UpdateScalarComponent(
    vr::VRInputComponentHandle_t component,
    float value,
    double time_offset) {
    static_cast<void>(component);
    static_cast<void>(value);
    static_cast<void>(time_offset);
    return vr::VRInputError_None;
}

vr::EVRInputError OpenVrHost::CreateHapticComponent(
    vr::PropertyContainerHandle_t container,
    const char* name,
    vr::VRInputComponentHandle_t* handle) {
    static_cast<void>(container);
    static_cast<void>(name);
    AllocateInputHandle(handle);
    return vr::VRInputError_None;
}

vr::EVRInputError OpenVrHost::CreateSkeletonComponent(
    vr::PropertyContainerHandle_t container,
    const char* name,
    const char* skeleton_path,
    const char* base_pose_path,
    vr::EVRSkeletalTrackingLevel tracking_level,
    const vr::VRBoneTransform_t* grip_limit_transforms,
    std::uint32_t grip_limit_transform_count,
    vr::VRInputComponentHandle_t* handle) {
    static_cast<void>(container);
    static_cast<void>(name);
    static_cast<void>(skeleton_path);
    static_cast<void>(base_pose_path);
    static_cast<void>(tracking_level);
    static_cast<void>(grip_limit_transforms);
    static_cast<void>(grip_limit_transform_count);
    AllocateInputHandle(handle);
    return vr::VRInputError_None;
}

vr::EVRInputError OpenVrHost::UpdateSkeletonComponent(
    vr::VRInputComponentHandle_t component,
    vr::EVRSkeletalMotionRange motion_range,
    const vr::VRBoneTransform_t* transforms,
    std::uint32_t transform_count) {
    static_cast<void>(component);
    static_cast<void>(motion_range);
    static_cast<void>(transforms);
    static_cast<void>(transform_count);
    return vr::VRInputError_None;
}

vr::EVRInputError OpenVrHost::CreatePoseComponent(
    vr::PropertyContainerHandle_t container,
    const char* name,
    vr::VRInputComponentHandle_t* handle) {
    static_cast<void>(container);
    static_cast<void>(name);
    AllocateInputHandle(handle);
    return vr::VRInputError_None;
}

vr::EVRInputError OpenVrHost::UpdatePoseComponent(
    vr::VRInputComponentHandle_t component,
    const vr::HmdMatrix34_t* pose_offset,
    double time_offset) {
    static_cast<void>(component);
    static_cast<void>(pose_offset);
    static_cast<void>(time_offset);
    return vr::VRInputError_None;
}

vr::EVRInputError OpenVrHost::CreateEyeTrackingComponent(
    vr::PropertyContainerHandle_t container,
    const char* name,
    vr::VRInputComponentHandle_t* handle) {
    static_cast<void>(container);
    static_cast<void>(name);
    AllocateInputHandle(handle);
    return vr::VRInputError_None;
}

vr::EVRInputError OpenVrHost::UpdateEyeTrackingComponent(
    vr::VRInputComponentHandle_t component,
    const vr::VREyeTrackingData_t* eye_tracking_data,
    double time_offset) {
    static_cast<void>(component);
    static_cast<void>(eye_tracking_data);
    static_cast<void>(time_offset);
    return vr::VRInputError_None;
}

std::uint32_t OpenVrHost::GetDriverCount() const {
    return 1;
}

std::uint32_t OpenVrHost::GetDriverName(
    vr::DriverId_t driver,
    char* value,
    std::uint32_t buffer_size) {
    if (driver != 0) {
        if (value != nullptr && buffer_size != 0) {
            value[0] = '\0';
        }
        return 0;
    }
    constexpr char name[] = "standable";
    CopyOutString(name, value, buffer_size);
    return sizeof(name);
}

vr::DriverHandle_t OpenVrHost::GetDriverHandle(const char* driver_name) {
    return driver_name != nullptr && std::strcmp(driver_name, "standable") == 0 ? 1 : 0;
}

bool OpenVrHost::IsEnabled(vr::DriverId_t driver) const {
    return driver == 0;
}

} // namespace standable::windows
