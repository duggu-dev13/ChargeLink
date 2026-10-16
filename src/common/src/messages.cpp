#include "chargelink/ocpp/messages.hpp"
#include <limits> 
#include <utility>
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
        // Required field of any type: returns it, or throws if absent.
        const json& require_field(const json& j, const char* key) {
            auto it = j.find(key);
            if (it == j.end()) {
                throw ParseError(std::string("missing field: ") + key);
            }
            return *it;
        }

        // JSON integer -> int, with a range check (a huge number must not wrap around).
        int to_int(const json& value, const char* key) {
            if (!value.is_number_integer()) {
                throw ParseError(std::string("field must be an integer: ") + key);
            }
            const auto n = value.get<long long>();
            if (n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max()) {
                throw ParseError(std::string("integer out of range: ") + key);
            }
            return static_cast<int>(n);
        }

        int require_int(const json& j, const char* key) {
            return to_int(require_field(j, key), key);
        }

        std::optional<int> optional_int(const json& j, const char* key) {
            auto it = j.find(key);
            if (it == j.end()) {
                return std::nullopt;
            }
            return to_int(*it, key);
        }

        // ----- enum <-> string tables: one list per enum, used in both directions -----

        template <class E>
        struct EnumTable;  // specialised below, one per enum

        template <>
        struct EnumTable<AuthorizationStatus> {
            static constexpr std::pair<AuthorizationStatus, const char*> entries[] = {
                { AuthorizationStatus::Accepted, "Accepted" },
                { AuthorizationStatus::Blocked, "Blocked" },
                { AuthorizationStatus::Expired, "Expired" },
                { AuthorizationStatus::Invalid, "Invalid" },
                { AuthorizationStatus::ConcurrentTx, "ConcurrentTx" },
            };
        };

        template <>
        struct EnumTable<ChargePointStatus> {
            static constexpr std::pair<ChargePointStatus, const char*> entries[] = {
                { ChargePointStatus::Available, "Available" },
                { ChargePointStatus::Preparing, "Preparing" },
                { ChargePointStatus::Charging, "Charging" },
                { ChargePointStatus::SuspendedEVSE, "SuspendedEVSE" },
                { ChargePointStatus::SuspendedEV, "SuspendedEV" },
                { ChargePointStatus::Finishing, "Finishing" },
                { ChargePointStatus::Reserved, "Reserved" },
                { ChargePointStatus::Unavailable, "Unavailable" },
                { ChargePointStatus::Faulted, "Faulted" },
            };
        };

        template <>
        struct EnumTable<ChargePointErrorCode> {
            static constexpr std::pair<ChargePointErrorCode, const char*> entries[] = {
                { ChargePointErrorCode::ConnectorLockFailure, "ConnectorLockFailure" },
                { ChargePointErrorCode::EVCommunicationError, "EVCommunicationError" },
                { ChargePointErrorCode::GroundFailure, "GroundFailure" },
                { ChargePointErrorCode::HighTemperature, "HighTemperature" },
                { ChargePointErrorCode::InternalError, "InternalError" },
                { ChargePointErrorCode::LocalListConflict, "LocalListConflict" },
                { ChargePointErrorCode::NoError, "NoError" },
                { ChargePointErrorCode::OtherError, "OtherError" },
                { ChargePointErrorCode::OverCurrentFailure, "OverCurrentFailure" },
                { ChargePointErrorCode::PowerMeterFailure, "PowerMeterFailure" },
                { ChargePointErrorCode::PowerSwitchFailure, "PowerSwitchFailure" },
                { ChargePointErrorCode::ReaderFailure, "ReaderFailure" },
                { ChargePointErrorCode::ResetFailure, "ResetFailure" },
                { ChargePointErrorCode::UnderVoltage, "UnderVoltage" },
                { ChargePointErrorCode::OverVoltage, "OverVoltage" },
                { ChargePointErrorCode::WeakSignal, "WeakSignal" },
            };
        };
        template <>
        struct EnumTable<StopReason> {
            static constexpr std::pair<StopReason, const char*> entries[] = {
                { StopReason::EmergencyStop, "EmergencyStop" },
                { StopReason::EVDisconnected, "EVDisconnected" },
                { StopReason::HardReset, "HardReset" },
                { StopReason::Local, "Local" },
                { StopReason::Other, "Other" },
                { StopReason::PowerLoss, "PowerLoss" },
                { StopReason::Reboot, "Reboot" },
                { StopReason::Remote, "Remote" },
                { StopReason::SoftReset, "SoftReset" },
                { StopReason::UnlockCommand, "UnlockCommand" },
                { StopReason::DeAuthorized, "DeAuthorized" },
            };
        };
        // TODO (exercise): add EnumTable<StopReason> here, following the pattern above.

        template <class E>
        const char* enum_name(E value) {
            for (const auto& entry : EnumTable<E>::entries) {
                if (entry.first == value) {
                    return entry.second;
                }
            }
            throw ParseError("unknown enum value");
        }

        template <class E>
        E enum_parse(const std::string& text, const char* field) {
            for (const auto& entry : EnumTable<E>::entries) {
                if (text == entry.second) {
                    return entry.first;
                }
            }
            throw ParseError(std::string("unknown value for ") + field + ": " + text);
        }

        template <class E>
        E require_enum(const json& j, const char* key) {
            return enum_parse<E>(require_string(j, key, 40), key);
        }

        template <class E>
        std::optional<E> optional_enum(const json& j, const char* key) {
            auto it = j.find(key);
            if (it == j.end()) {
                return std::nullopt;
            }
            if (!it->is_string()) {
                throw ParseError(std::string("invalid field: ") + key);
            }
            return enum_parse<E>(it->get<std::string>(), key);
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
    // ---------- IdTagInfo ----------

    void to_json(json& j, const IdTagInfo& v) {
        j = json{ { "status", enum_name(v.status) } };
        if (v.expiry_date) {
            j["expiryDate"] = *v.expiry_date;
        }
        if (v.parent_id_tag) {
            j["parentIdTag"] = *v.parent_id_tag;
        }
    }

    void from_json(const json& j, IdTagInfo& v) {
        require_object(j);
        v.status = require_enum<AuthorizationStatus>(j, "status");
        v.expiry_date = optional_string(j, "expiryDate", 40);
        v.parent_id_tag = optional_string(j, "parentIdTag", 20);
    }

    // ---------- Authorize ----------

    void to_json(json& j, const AuthorizeRequest& v) {
        j = json{ { "idTag", v.id_tag } };
    }

    void from_json(const json& j, AuthorizeRequest& v) {
        require_object(j);
        v.id_tag = require_string(j, "idTag", 20);
    }

    void to_json(json& j, const AuthorizeResponse& v) {
        j = json{ { "idTagInfo", v.id_tag_info } };
    }

    void from_json(const json& j, AuthorizeResponse& v) {
        require_object(j);
        v.id_tag_info = require_field(j, "idTagInfo").get<IdTagInfo>();
    }

    // ---------- StatusNotification ----------

    void to_json(json& j, const StatusNotificationRequest& v) {
        j = json{ { "connectorId", v.connector_id },
                 { "errorCode", enum_name(v.error_code) },
                 { "status", enum_name(v.status) } };
        if (v.info) j["info"] = *v.info;
        if (v.timestamp) j["timestamp"] = *v.timestamp;
        if (v.vendor_id) j["vendorId"] = *v.vendor_id;
        if (v.vendor_error_code) j["vendorErrorCode"] = *v.vendor_error_code;
    }

    void from_json(const json& j, StatusNotificationRequest& v) {
        require_object(j);
        v.connector_id = require_int(j, "connectorId");
        if (v.connector_id < 0) {
            throw ParseError("connectorId must be >= 0");
        }
        v.error_code = require_enum<ChargePointErrorCode>(j, "errorCode");
        v.status = require_enum<ChargePointStatus>(j, "status");
        v.info = optional_string(j, "info", 50);
        v.timestamp = optional_string(j, "timestamp", 40);
        v.vendor_id = optional_string(j, "vendorId", 255);
        v.vendor_error_code = optional_string(j, "vendorErrorCode", 50);
    }

    void to_json(json& j, const StatusNotificationResponse&) {
        j = json::object();
    }

    void from_json(const json& j, StatusNotificationResponse&) {
        require_object(j);
    }

    // ---------- StartTransaction ----------

    void to_json(json& j, const StartTransactionRequest& v) {
        j = json{ { "connectorId", v.connector_id },
                 { "idTag", v.id_tag },
                 { "meterStart", v.meter_start },
                 { "timestamp", v.timestamp } };
        if (v.reservation_id) {
            j["reservationId"] = *v.reservation_id;
        }
    }

    void from_json(const json& j, StartTransactionRequest& v) {
        require_object(j);
        v.connector_id = require_int(j, "connectorId");
        if (v.connector_id < 1) {
            throw ParseError("connectorId must be >= 1");
        }
        v.id_tag = require_string(j, "idTag", 20);
        v.meter_start = require_int(j, "meterStart");
        if (v.meter_start < 0) {
            throw ParseError("meterStart must be >= 0");
        }
        v.timestamp = require_string(j, "timestamp", 40);
        v.reservation_id = optional_int(j, "reservationId");
    }

    void to_json(json& j, const StartTransactionResponse& v) {
        j = json{ { "idTagInfo", v.id_tag_info },{ "transactionId", v.transaction_id } };
    }

    void from_json(const json& j, StartTransactionResponse& v) {
        require_object(j);
        v.id_tag_info = require_field(j, "idTagInfo").get<IdTagInfo>();
        v.transaction_id = require_int(j, "transactionId");
    }

    // ---------- MeterValues ----------

    void to_json(json& j, const SampledValue& v) {
        j = json{ { "value", v.value } };
        if (v.context) j["context"] = *v.context;
        if (v.format) j["format"] = *v.format;
        if (v.measurand) j["measurand"] = *v.measurand;
        if (v.phase) j["phase"] = *v.phase;
        if (v.location) j["location"] = *v.location;
        if (v.unit) j["unit"] = *v.unit;
    }

    void from_json(const json& j, SampledValue& v) {
        require_object(j);
        v.value = require_string(j, "value", 100);
        v.context = optional_string(j, "context", 40);
        v.format = optional_string(j, "format", 40);
        v.measurand = optional_string(j, "measurand", 60);
        v.phase = optional_string(j, "phase", 10);
        v.location = optional_string(j, "location", 20);
        v.unit = optional_string(j, "unit", 20);
    }

    void to_json(json& j, const MeterValue& v) {
        j = json{ { "timestamp", v.timestamp },{ "sampledValue", v.sampled_value } };
    }

    void from_json(const json& j, MeterValue& v) {
        require_object(j);
        v.timestamp = require_string(j, "timestamp", 40);
        const json& list = require_field(j, "sampledValue");
        if (!list.is_array() || list.empty()) {
            throw ParseError("sampledValue must be a non-empty array");
        }
        v.sampled_value = list.get<std::vector<SampledValue>>();
    }

    void to_json(json& j, const MeterValuesRequest& v) {
        j = json{ { "connectorId", v.connector_id },{ "meterValue", v.meter_value } };
        if (v.transaction_id) {
            j["transactionId"] = *v.transaction_id;
        }
    }

    void from_json(const json& j, MeterValuesRequest& v) {
        require_object(j);
        v.connector_id = require_int(j, "connectorId");
        if (v.connector_id < 0) {
            throw ParseError("connectorId must be >= 0");
        }
        v.transaction_id = optional_int(j, "transactionId");
        const json& list = require_field(j, "meterValue");
        if (!list.is_array() || list.empty()) {
            throw ParseError("meterValue must be a non-empty array");
        }
        v.meter_value = list.get<std::vector<MeterValue>>();
    }

    void to_json(json& j, const MeterValuesResponse&) {
        j = json::object();
    }

    void from_json(const json& j, MeterValuesResponse&) {
        require_object(j);
    }

    // ---------- StopTransaction: YOUR EXERCISE ----------
    // Write these four functions yourself (see section 4).
    // ---------- StopTransaction ----------

    void to_json(json& j, const StopTransactionRequest& v) {
        j = json{ { "meterStop", v.meter_stop },
                 { "timestamp", v.timestamp },
                 { "transactionId", v.transaction_id } };
        if (v.id_tag) {
            j["idTag"] = *v.id_tag;
        }
        if (v.reason) {
            j["reason"] = enum_name(*v.reason);
        }
    }

    void from_json(const json& j, StopTransactionRequest& v) {
        require_object(j);
        v.id_tag = optional_string(j, "idTag", 20);
        v.meter_stop = require_int(j, "meterStop");
        if (v.meter_stop < 0) {
            throw ParseError("meterStop must be >= 0");
        }
        v.timestamp = require_string(j, "timestamp", 40);
        v.transaction_id = require_int(j, "transactionId");
        if (v.transaction_id < 0) {
            throw ParseError("transactionId must be >= 0");
        }
        v.reason = optional_enum<StopReason>(j, "reason");
    }

    void to_json(json& j, const StopTransactionResponse& v) {
        j = json::object();
        if (v.id_tag_info) {
            j["idTagInfo"] = *v.id_tag_info;
        }
    }

    void from_json(const json& j, StopTransactionResponse& v) {
        require_object(j);
        auto it = j.find("idTagInfo");
        if (it != j.end()) {
            v.id_tag_info = it->get<IdTagInfo>();
        }
        else {
            v.id_tag_info.reset();
        }
    }
}  // namespace chargelink::ocpp