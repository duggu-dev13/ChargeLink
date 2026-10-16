#include "chargelink/ocpp/messages.hpp"

namespace chargelink::ocpp {

    namespace {

        // Required string field, with a maximum length.
        std::string require_string(const json& j, const char* key, std::size_t max_len) {
            auto it = j.find(key);
            if (it == j.end() || !it->is_string()) {
                throw ParseError(std::string("missing or invalid field: ") + key);
            }
            std::string value = it->get<std::string>();
            if (value.size() > max_len) {
                throw ParseError(std::string("field too long: ") + key);
            }
            return value;
        }

        // Optional string field: absent is fine, present-but-wrong is not.
        std::optional<std::string> optional_string(const json& j, const char* key, std::size_t max_len) {
            auto it = j.find(key);
            if (it == j.end()) {
                return std::nullopt;
            }
            if (!it->is_string()) {
                throw ParseError(std::string("invalid field: ") + key);
            }
            std::string value = it->get<std::string>();
            if (value.size() > max_len) {
                throw ParseError(std::string("field too long: ") + key);
            }
            return value;
        }

        void require_object(const json& j) {
            if (!j.is_object()) {
                throw ParseError("payload must be a JSON object");
            }
        }

    }  // namespace

    // ---------- RegistrationStatus ----------

    std::string to_string(RegistrationStatus status) {
        switch (status) {
        case RegistrationStatus::Accepted: return "Accepted";
        case RegistrationStatus::Pending:  return "Pending";
        case RegistrationStatus::Rejected: return "Rejected";
        }
        throw ParseError("unknown RegistrationStatus");
    }

    RegistrationStatus registration_status_from_string(const std::string& text) {
        if (text == "Accepted") return RegistrationStatus::Accepted;
        if (text == "Pending")  return RegistrationStatus::Pending;
        if (text == "Rejected") return RegistrationStatus::Rejected;
        throw ParseError("unknown RegistrationStatus: " + text);
    }

    // ---------- BootNotification ----------

    void to_json(json& j, const BootNotificationRequest& v) {
        j = json{ { "chargePointVendor", v.charge_point_vendor },
                 { "chargePointModel", v.charge_point_model } };
        if (v.charge_point_serial_number) {
            j["chargePointSerialNumber"] = *v.charge_point_serial_number;
        }
        if (v.firmware_version) {
            j["firmwareVersion"] = *v.firmware_version;
        }
    }

    void from_json(const json& j, BootNotificationRequest& v) {
        require_object(j);
        v.charge_point_vendor = require_string(j, "chargePointVendor", 20);
        v.charge_point_model = require_string(j, "chargePointModel", 20);
        v.charge_point_serial_number = optional_string(j, "chargePointSerialNumber", 25);
        v.firmware_version = optional_string(j, "firmwareVersion", 50);
    }

    void to_json(json& j, const BootNotificationResponse& v) {
        j = json{ { "status", to_string(v.status) },
                 { "currentTime", v.current_time },
                 { "interval", v.interval } };
    }

    void from_json(const json& j, BootNotificationResponse& v) {
        require_object(j);
        v.status = registration_status_from_string(require_string(j, "status", 20));
        v.current_time = require_string(j, "currentTime", 40);

        auto it = j.find("interval");
        if (it == j.end() || !it->is_number_integer() || it->get<int>() < 0) {
            throw ParseError("missing or invalid field: interval");
        }
        v.interval = it->get<int>();
    }

    // ---------- Heartbeat ----------

    void to_json(json& j, const HeartbeatRequest&) {
        j = json::object();
    }

    void from_json(const json& j, HeartbeatRequest&) {
        require_object(j);
    }

    void to_json(json& j, const HeartbeatResponse& v) {
        j = json{ { "currentTime", v.current_time } };
    }

    void from_json(const json& j, HeartbeatResponse& v) {
        require_object(j);
        v.current_time = require_string(j, "currentTime", 40);
    }

}  // namespace chargelink::ocpp