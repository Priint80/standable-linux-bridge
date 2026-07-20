#include "relay_tracker.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace standable::native {

namespace {

vr::DriverPose_t DisconnectedPose() {
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation.w = 1.0;
    pose.qRotation.w = 1.0;
    pose.result = vr::TrackingResult_Uninitialized;
    pose.poseIsValid = false;
    pose.deviceIsConnected = false;
    return pose;
}

} // namespace

void RelayTracker::Configure(
    std::uint32_t remote_device_index,
    std::int32_t device_class,
    const char* serial) {
    remote_device_index_ = remote_device_index;
    device_class_ = device_class;
    serial_ = serial == nullptr ? "" : serial;
    configured_ = !serial_.empty();
    pose_ = DisconnectedPose();
}

void RelayTracker::UpdateProperty(const bridge::PropertyPayload& property) {
    if (property.data_size > bridge::kMaxPropertyBytes) {
        return;
    }

    CachedProperty cached{};
    cached.write_type = property.write_type;
    cached.set_error = property.set_error;
    cached.tag = property.tag;
    cached.data_size = property.data_size;
    if (cached.data_size != 0) {
        std::copy_n(property.data.begin(), cached.data_size, cached.data.begin());
    }
    properties_[property.property] = cached;

    if (ObjectId() != vr::k_unTrackedDeviceIndexInvalid) {
        ApplyProperty(property.property, cached);
    }
}

void RelayTracker::UpdatePose(const bridge::WireDriverPose& pose) {
    pose_ = FromWire(pose);
}

void RelayTracker::PublishPose() const {
    const auto object_id = ObjectId();
    if (object_id == vr::k_unTrackedDeviceIndexInvalid) {
        return;
    }
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id, pose_, sizeof(pose_));
}

void RelayTracker::MarkDisconnected() {
    pose_ = DisconnectedPose();
    PublishPose();
}

vr::EVRInitError RelayTracker::Activate(std::uint32_t object_id) {
    object_id_.store(object_id, std::memory_order_release);
    ApplyDefaults();
    for (const auto& [property, cached] : properties_) {
        ApplyProperty(property, cached);
    }
    return vr::VRInitError_None;
}

void RelayTracker::Deactivate() {
    object_id_.store(vr::k_unTrackedDeviceIndexInvalid, std::memory_order_release);
}

void RelayTracker::EnterStandby() {}

void* RelayTracker::GetComponent(const char* component_name_and_version) {
    static_cast<void>(component_name_and_version);
    return nullptr;
}

void RelayTracker::DebugRequest(
    const char* request,
    char* response,
    std::uint32_t response_size) {
    static_cast<void>(request);
    if (response == nullptr || response_size == 0) {
        return;
    }
    std::snprintf(response, response_size, "%s", "Standable tracker ready");
    response[response_size - 1] = '\0';
}

vr::DriverPose_t RelayTracker::GetPose() {
    return pose_;
}

vr::DriverPose_t RelayTracker::FromWire(const bridge::WireDriverPose& wire) {
    vr::DriverPose_t pose{};
    pose.poseTimeOffset = wire.pose_time_offset;
    pose.qWorldFromDriverRotation = {
        wire.world_from_driver_rotation[0],
        wire.world_from_driver_rotation[1],
        wire.world_from_driver_rotation[2],
        wire.world_from_driver_rotation[3]};
    std::copy(wire.world_from_driver_translation.begin(), wire.world_from_driver_translation.end(), pose.vecWorldFromDriverTranslation);
    pose.qDriverFromHeadRotation = {
        wire.driver_from_head_rotation[0],
        wire.driver_from_head_rotation[1],
        wire.driver_from_head_rotation[2],
        wire.driver_from_head_rotation[3]};
    std::copy(wire.driver_from_head_translation.begin(), wire.driver_from_head_translation.end(), pose.vecDriverFromHeadTranslation);
    std::copy(wire.position.begin(), wire.position.end(), pose.vecPosition);
    std::copy(wire.velocity.begin(), wire.velocity.end(), pose.vecVelocity);
    std::copy(wire.acceleration.begin(), wire.acceleration.end(), pose.vecAcceleration);
    pose.qRotation = {
        wire.rotation[0],
        wire.rotation[1],
        wire.rotation[2],
        wire.rotation[3]};
    std::copy(wire.angular_velocity.begin(), wire.angular_velocity.end(), pose.vecAngularVelocity);
    std::copy(wire.angular_acceleration.begin(), wire.angular_acceleration.end(), pose.vecAngularAcceleration);
    pose.result = static_cast<vr::ETrackingResult>(wire.tracking_result);
    pose.poseIsValid = wire.pose_is_valid != 0;
    pose.willDriftInYaw = wire.will_drift_in_yaw != 0;
    pose.shouldApplyHeadModel = wire.should_apply_head_model != 0;
    pose.deviceIsConnected = wire.device_is_connected != 0;
    return pose;
}

void RelayTracker::ApplyDefaults() const {
    const auto object_id = ObjectId();
    if (object_id == vr::k_unTrackedDeviceIndexInvalid) {
        return;
    }

    const auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id);
    const std::string registered_type = "standable/" + serial_;
    vr::VRProperties()->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "standable");
    vr::VRProperties()->SetStringProperty(container, vr::Prop_ModelNumber_String, serial_.c_str());
    vr::VRProperties()->SetStringProperty(container, vr::Prop_SerialNumber_String, serial_.c_str());
    vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, "Standable");
    vr::VRProperties()->SetStringProperty(container, vr::Prop_RegisteredDeviceType_String, registered_type.c_str());
    vr::VRProperties()->SetStringProperty(container, vr::Prop_InputProfilePath_String, "{htc}/input/vive_tracker_profile.json");
    vr::VRProperties()->SetStringProperty(container, vr::Prop_ControllerType_String, "vive_tracker");
    vr::VRProperties()->SetInt32Property(container, vr::Prop_DeviceClass_Int32, device_class_);
    vr::VRProperties()->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_OptOut);
    vr::VRProperties()->SetBoolProperty(container, vr::Prop_DeviceCanPowerOff_Bool, false);
    vr::VRProperties()->SetBoolProperty(container, vr::Prop_NeverTracked_Bool, false);
}

void RelayTracker::ApplyProperty(std::int32_t property, const CachedProperty& cached) const {
    const auto object_id = ObjectId();
    if (object_id == vr::k_unTrackedDeviceIndexInvalid || cached.data_size > cached.data.size()) {
        return;
    }

    vr::PropertyWrite_t write{};
    write.prop = static_cast<vr::ETrackedDeviceProperty>(property);
    write.writeType = static_cast<vr::EPropertyWriteType>(cached.write_type);
    write.eSetError = static_cast<vr::ETrackedPropertyError>(cached.set_error);
    write.pvBuffer = cached.data_size == 0 ? nullptr : const_cast<std::uint8_t*>(cached.data.data());
    write.unBufferSize = cached.data_size;
    write.unTag = cached.tag;

    const auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id);
    static_cast<void>(vr::VRPropertiesRaw()->WritePropertyBatch(container, &write, 1));
}

} // namespace standable::native
