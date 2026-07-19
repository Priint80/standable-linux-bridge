#pragma once

#include "bridge_protocol.hpp"
#include "windows_transport.hpp"

#include <openvr_driver.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace standable::windows {

class OpenVrHost;

class PhysicalDeviceStub final : public vr::ITrackedDeviceServerDriver {
public:
    void Configure(OpenVrHost* host, std::uint32_t index);

    vr::EVRInitError Activate(std::uint32_t object_id) override;
    void Deactivate() override;
    void EnterStandby() override;
    void* GetComponent(const char* component_name_and_version) override;
    void DebugRequest(const char* request, char* response, std::uint32_t response_size) override;
    vr::DriverPose_t GetPose() override;

private:
    OpenVrHost* host_{};
    std::uint32_t index_{vr::k_unTrackedDeviceIndexInvalid};
};

class OpenVrHost final :
    public vr::IVRDriverContext,
    public vr::IVRServerDriverHost,
    public vr::IVRProperties,
    public vr::IVRSettings,
    public vr::IVRDriverLog,
    public vr::IVRResources,
    public vr::IVRDriverInput,
    public vr::IVRDriverManager {
public:
    OpenVrHost(WindowsTransport& transport, std::filesystem::path driver_root);

    void HandlePacket(const ReceivedPacket& packet);
    void SendHello();
    void SendHeartbeat();
    void SendProviderStatus(bridge::ProviderState state, std::int32_t error, const char* detail);
    void ReplayTrackers();
    void EnablePhysicalInjection();
    void SignalShutdown();
    [[nodiscard]] bool ShouldExit() const noexcept { return exiting_.load(std::memory_order_acquire); }
    [[nodiscard]] vr::DriverPose_t CurrentPhysicalDriverPose(std::uint32_t index) const;

    // IVRDriverContext
    void* GetGenericInterface(const char* interface_version, vr::EVRInitError* error) override;
    vr::DriverHandle_t GetDriverHandle() override;

    // IVRServerDriverHost
    bool TrackedDeviceAdded(
        const char* serial,
        vr::ETrackedDeviceClass device_class,
        vr::ITrackedDeviceServerDriver* driver) override;
    void TrackedDevicePoseUpdated(
        std::uint32_t device_index,
        const vr::DriverPose_t& pose,
        std::uint32_t pose_size) override;
    void VsyncEvent(double time_offset) override;
    void VendorSpecificEvent(
        std::uint32_t device_index,
        vr::EVREventType event_type,
        const vr::VREvent_Data_t& event_data,
        double time_offset) override;
    bool IsExiting() override;
    bool PollNextEvent(vr::VREvent_t* event, std::uint32_t event_size) override;
    void GetRawTrackedDevicePoses(
        float predicted_seconds,
        vr::TrackedDevicePose_t* poses,
        std::uint32_t pose_count) override;
    void RequestRestart(
        const char* localized_reason,
        const char* executable,
        const char* arguments,
        const char* working_directory) override;
    std::uint32_t GetFrameTimings(vr::Compositor_FrameTiming* timing, std::uint32_t frames) override;
    void SetDisplayEyeToHead(
        std::uint32_t device_index,
        const vr::HmdMatrix34_t& left,
        const vr::HmdMatrix34_t& right) override;
    void SetDisplayProjectionRaw(
        std::uint32_t device_index,
        const vr::HmdRect2_t& left,
        const vr::HmdRect2_t& right) override;
    void SetRecommendedRenderTargetSize(
        std::uint32_t device_index,
        std::uint32_t width,
        std::uint32_t height) override;

    // IVRProperties
    vr::ETrackedPropertyError ReadPropertyBatch(
        vr::PropertyContainerHandle_t container,
        vr::PropertyRead_t* batch,
        std::uint32_t count) override;
    vr::ETrackedPropertyError WritePropertyBatch(
        vr::PropertyContainerHandle_t container,
        vr::PropertyWrite_t* batch,
        std::uint32_t count) override;
    const char* GetPropErrorNameFromEnum(vr::ETrackedPropertyError error) override;
    vr::PropertyContainerHandle_t TrackedDeviceToPropertyContainer(std::uint32_t device_index) override;

    // IVRSettings
    const char* GetSettingsErrorNameFromEnum(vr::EVRSettingsError error) override;
    void SetBool(const char* section, const char* key, bool value, vr::EVRSettingsError* error) override;
    void SetInt32(const char* section, const char* key, std::int32_t value, vr::EVRSettingsError* error) override;
    void SetFloat(const char* section, const char* key, float value, vr::EVRSettingsError* error) override;
    void SetString(const char* section, const char* key, const char* value, vr::EVRSettingsError* error) override;
    bool GetBool(const char* section, const char* key, vr::EVRSettingsError* error) override;
    std::int32_t GetInt32(const char* section, const char* key, vr::EVRSettingsError* error) override;
    float GetFloat(const char* section, const char* key, vr::EVRSettingsError* error) override;
    void GetString(
        const char* section,
        const char* key,
        char* value,
        std::uint32_t value_length,
        vr::EVRSettingsError* error) override;
    void RemoveSection(const char* section, vr::EVRSettingsError* error) override;
    void RemoveKeyInSection(const char* section, const char* key, vr::EVRSettingsError* error) override;

    // IVRDriverLog
    void Log(const char* message) override;

    // IVRResources
    std::uint32_t LoadSharedResource(const char* resource_name, char* buffer, std::uint32_t buffer_length) override;
    std::uint32_t GetResourceFullPath(
        const char* resource_name,
        const char* resource_type_directory,
        char* path_buffer,
        std::uint32_t buffer_length) override;

    // IVRDriverInput
    vr::EVRInputError CreateBooleanComponent(
        vr::PropertyContainerHandle_t container,
        const char* name,
        vr::VRInputComponentHandle_t* handle) override;
    vr::EVRInputError UpdateBooleanComponent(
        vr::VRInputComponentHandle_t component,
        bool value,
        double time_offset) override;
    vr::EVRInputError CreateScalarComponent(
        vr::PropertyContainerHandle_t container,
        const char* name,
        vr::VRInputComponentHandle_t* handle,
        vr::EVRScalarType type,
        vr::EVRScalarUnits units) override;
    vr::EVRInputError UpdateScalarComponent(
        vr::VRInputComponentHandle_t component,
        float value,
        double time_offset) override;
    vr::EVRInputError CreateHapticComponent(
        vr::PropertyContainerHandle_t container,
        const char* name,
        vr::VRInputComponentHandle_t* handle) override;
    vr::EVRInputError CreateSkeletonComponent(
        vr::PropertyContainerHandle_t container,
        const char* name,
        const char* skeleton_path,
        const char* base_pose_path,
        vr::EVRSkeletalTrackingLevel tracking_level,
        const vr::VRBoneTransform_t* grip_limit_transforms,
        std::uint32_t grip_limit_transform_count,
        vr::VRInputComponentHandle_t* handle) override;
    vr::EVRInputError UpdateSkeletonComponent(
        vr::VRInputComponentHandle_t component,
        vr::EVRSkeletalMotionRange motion_range,
        const vr::VRBoneTransform_t* transforms,
        std::uint32_t transform_count) override;
    vr::EVRInputError CreatePoseComponent(
        vr::PropertyContainerHandle_t container,
        const char* name,
        vr::VRInputComponentHandle_t* handle) override;
    vr::EVRInputError UpdatePoseComponent(
        vr::VRInputComponentHandle_t component,
        const vr::HmdMatrix34_t* pose_offset,
        double time_offset) override;
    vr::EVRInputError CreateEyeTrackingComponent(
        vr::PropertyContainerHandle_t container,
        const char* name,
        vr::VRInputComponentHandle_t* handle) override;
    vr::EVRInputError UpdateEyeTrackingComponent(
        vr::VRInputComponentHandle_t component,
        const vr::VREyeTrackingData_t* eye_tracking_data,
        double time_offset) override;

    // IVRDriverManager
    std::uint32_t GetDriverCount() const override;
    std::uint32_t GetDriverName(vr::DriverId_t driver, char* value, std::uint32_t buffer_size) override;
    vr::DriverHandle_t GetDriverHandle(const char* driver_name) override;
    bool IsEnabled(vr::DriverId_t driver) const override;

private:
    struct StoredProperty {
        std::uint32_t tag{};
        vr::ETrackedPropertyError error{vr::TrackedProp_Success};
        std::vector<std::uint8_t> data;
    };

    struct TrackerRegistration {
        std::uint32_t index{};
        std::int32_t device_class{};
        std::string serial;
        vr::ITrackedDeviceServerDriver* driver{};
        vr::DriverPose_t last_pose{};
        bool has_pose{false};
    };

    struct SettingValue {
        enum class Type { Boolean, Integer, Float, String } type{Type::String};
        bool boolean{};
        std::int32_t integer{};
        float floating{};
        std::string string;
    };

    static vr::TrackedDevicePose_t PhysicalPoseFromWire(const bridge::WireTrackedPose& wire);
    static bridge::WireDriverPose DriverPoseToWire(const vr::DriverPose_t& pose);
    static std::string SettingKey(const char* section, const char* key);
    static void SetSettingsError(vr::EVRSettingsError* error, vr::EVRSettingsError value);
    static void CopyOutString(const std::string& source, char* destination, std::uint32_t capacity);

    void OnPhysicalPose(const bridge::PhysicalPosePayload& update);
    void OnPhysicalProperty(const bridge::PropertyPayload& property);
    void OnPhysicalDisconnected(const bridge::PhysicalDisconnectedPayload& disconnected);
    void OnEvent(const bridge::EventPayload& event);
    void TryInjectPhysicalDevice(std::uint32_t index);
    void InjectPhysicalPose(std::uint32_t index);
    std::string PhysicalSerial(std::uint32_t index) const;
    void StoreProperty(std::uint64_t container, const bridge::PropertyPayload& property);
    void StoreScalarProperty(
        std::uint32_t index,
        vr::ETrackedDeviceProperty property,
        std::uint32_t tag,
        const void* data,
        std::size_t size);
    void RelayTrackerProperty(
        std::uint32_t index,
        const vr::PropertyWrite_t& write);
    TrackerRegistration* FindTracker(std::uint32_t index);
    const TrackerRegistration* FindTracker(std::uint32_t index) const;
    vr::VRInputComponentHandle_t AllocateInputHandle(vr::VRInputComponentHandle_t* output);
    std::filesystem::path ResolveResource(
        const char* resource_name,
        const char* resource_type_directory) const;

    WindowsTransport& transport_;
    std::filesystem::path driver_root_;
    mutable std::mutex state_mutex_;
    mutable std::mutex property_mutex_;
    mutable std::mutex event_mutex_;
    mutable std::mutex settings_mutex_;
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> physical_poses_{};
    std::array<std::int32_t, vr::k_unMaxTrackedDeviceCount> physical_classes_{};
    std::array<bool, vr::k_unMaxTrackedDeviceCount> physical_injected_{};
    std::array<PhysicalDeviceStub, 48> physical_stubs_{};
    std::unordered_map<std::uint64_t, std::unordered_map<std::int32_t, StoredProperty>> properties_;
    std::queue<vr::VREvent_t> events_;
    std::vector<TrackerRegistration> trackers_;
    std::unordered_map<vr::ITrackedDeviceServerDriver*, std::uint32_t> tracker_indices_;
    std::unordered_map<std::string, SettingValue> settings_;
    std::atomic<bool> exiting_{false};
    std::atomic<bool> physical_injection_enabled_{false};
    std::atomic<std::uint64_t> next_input_handle_{1};
    std::uint32_t next_tracker_index_{48};
};

} // namespace standable::windows
