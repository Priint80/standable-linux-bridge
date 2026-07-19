#pragma once

#include "bridge_protocol.hpp"

#include <openvr_driver.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace standable::native {

class RelayTracker final : public vr::ITrackedDeviceServerDriver {
public:
    void Configure(
        std::uint32_t remote_device_index,
        std::int32_t device_class,
        const char* serial);
    void UpdateProperty(const bridge::PropertyPayload& property);
    void UpdatePose(const bridge::WireDriverPose& pose);
    void PublishPose() const;
    void MarkDisconnected();

    [[nodiscard]] bool IsConfigured() const noexcept { return configured_; }
    [[nodiscard]] std::uint32_t RemoteDeviceIndex() const noexcept { return remote_device_index_; }
    [[nodiscard]] std::uint32_t ObjectId() const noexcept {
        return object_id_.load(std::memory_order_acquire);
    }
    [[nodiscard]] const std::string& Serial() const noexcept { return serial_; }

    vr::EVRInitError Activate(std::uint32_t object_id) override;
    void Deactivate() override;
    void EnterStandby() override;
    void* GetComponent(const char* component_name_and_version) override;
    void DebugRequest(const char* request, char* response, std::uint32_t response_size) override;
    vr::DriverPose_t GetPose() override;

private:
    struct CachedProperty {
        std::int32_t write_type{};
        std::int32_t set_error{};
        std::uint32_t tag{};
        std::uint32_t data_size{};
        std::array<std::uint8_t, bridge::kMaxPropertyBytes> data{};
    };

    static vr::DriverPose_t FromWire(const bridge::WireDriverPose& wire);
    void ApplyDefaults() const;
    void ApplyProperty(std::int32_t property, const CachedProperty& cached) const;

    bool configured_{false};
    std::uint32_t remote_device_index_{vr::k_unTrackedDeviceIndexInvalid};
    std::int32_t device_class_{static_cast<std::int32_t>(vr::TrackedDeviceClass_GenericTracker)};
    std::string serial_;
    std::atomic<std::uint32_t> object_id_{vr::k_unTrackedDeviceIndexInvalid};
    vr::DriverPose_t pose_{};
    std::unordered_map<std::int32_t, CachedProperty> properties_;
};

} // namespace standable::native
