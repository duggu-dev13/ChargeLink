#pragma once

#include <optional>
#include <string>

#include "chargelink/ocpp/frame.hpp"

namespace chargelink::ocpp {

    // ---------- helpers ----------

    // Build a CALL frame from any request struct (uses T::kAction).
    template <class T>
    Call make_call(const std::string& unique_id, const T& request) {
        return Call{ unique_id, T::kAction, json(request) };
    }

    // Convert a payload into a typed struct. Any JSON problem becomes ParseError.
    template <class T>
    T payload_as(const json& payload) {
        try {
            return payload.get<T>();
        }
        catch (const json::exception& e) {
            throw ParseError(std::string("invalid payload: ") + e.what());
        }
    }

    // ---------- BootNotification ----------

    enum class RegistrationStatus { Accepted, Pending, Rejected };

    std::string to_string(RegistrationStatus status);
    RegistrationStatus registration_status_from_string(const std::string& text);

    struct BootNotificationRequest {
        static constexpr const char* kAction = "BootNotification";

        std::string charge_point_vendor;
        std::string charge_point_model;
        std::optional<std::string> charge_point_serial_number;
        std::optional<std::string> firmware_version;
    };

    struct BootNotificationResponse {
        RegistrationStatus status = RegistrationStatus::Rejected;
        std::string current_time;
        int interval = 0;
    };

    void to_json(json& j, const BootNotificationRequest& v);
    void from_json(const json& j, BootNotificationRequest& v);
    void to_json(json& j, const BootNotificationResponse& v);
    void from_json(const json& j, BootNotificationResponse& v);

    // ---------- Heartbeat ----------

    struct HeartbeatRequest {
        static constexpr const char* kAction = "Heartbeat";
    };

    struct HeartbeatResponse {
        std::string current_time;
    };

    void to_json(json& j, const HeartbeatRequest& v);
    void from_json(const json& j, HeartbeatRequest& v);
    void to_json(json& j, const HeartbeatResponse& v);
    void from_json(const json& j, HeartbeatResponse& v);

}  // namespace chargelink::ocpp