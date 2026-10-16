#pragma once

#include <optional>
#include <string>
#include <vector>
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

    // ---------- IdTagInfo (shared by Authorize / Start / Stop) ----------

    enum class AuthorizationStatus { Accepted, Blocked, Expired, Invalid, ConcurrentTx };

    struct IdTagInfo {
        AuthorizationStatus status = AuthorizationStatus::Invalid;
        std::optional<std::string> expiry_date;
        std::optional<std::string> parent_id_tag;
    };

    void to_json(json& j, const IdTagInfo& v);
    void from_json(const json& j, IdTagInfo& v);

    // ---------- Authorize ----------

    struct AuthorizeRequest {
        static constexpr const char* kAction = "Authorize";
        std::string id_tag;
    };

    struct AuthorizeResponse {
        IdTagInfo id_tag_info;
    };

    void to_json(json& j, const AuthorizeRequest& v);
    void from_json(const json& j, AuthorizeRequest& v);
    void to_json(json& j, const AuthorizeResponse& v);
    void from_json(const json& j, AuthorizeResponse& v);

    // ---------- StatusNotification ----------

    enum class ChargePointStatus {
        Available, Preparing, Charging, SuspendedEVSE, SuspendedEV,
        Finishing, Reserved, Unavailable, Faulted
    };

    enum class ChargePointErrorCode {
        ConnectorLockFailure, EVCommunicationError, GroundFailure, HighTemperature,
        InternalError, LocalListConflict, NoError, OtherError, OverCurrentFailure,
        PowerMeterFailure, PowerSwitchFailure, ReaderFailure, ResetFailure,
        UnderVoltage, OverVoltage, WeakSignal
    };

    struct StatusNotificationRequest {
        static constexpr const char* kAction = "StatusNotification";
        int connector_id = 0;  // 0 = the charge point itself
        ChargePointErrorCode error_code = ChargePointErrorCode::NoError;
        ChargePointStatus status = ChargePointStatus::Available;
        std::optional<std::string> info;
        std::optional<std::string> timestamp;
        std::optional<std::string> vendor_id;
        std::optional<std::string> vendor_error_code;
    };

    struct StatusNotificationResponse {};  // empty payload

    void to_json(json& j, const StatusNotificationRequest& v);
    void from_json(const json& j, StatusNotificationRequest& v);
    void to_json(json& j, const StatusNotificationResponse& v);
    void from_json(const json& j, StatusNotificationResponse& v);

    // ---------- StartTransaction ----------

    struct StartTransactionRequest {
        static constexpr const char* kAction = "StartTransaction";
        int connector_id = 0;
        std::string id_tag;
        int meter_start = 0;  // Wh
        std::string timestamp;
        std::optional<int> reservation_id;
    };

    struct StartTransactionResponse {
        IdTagInfo id_tag_info;
        int transaction_id = 0;
    };

    void to_json(json& j, const StartTransactionRequest& v);
    void from_json(const json& j, StartTransactionRequest& v);
    void to_json(json& j, const StartTransactionResponse& v);
    void from_json(const json& j, StartTransactionResponse& v);

    // ---------- MeterValues ----------

    struct SampledValue {
        std::string value;                    // numbers travel as strings in OCPP
        std::optional<std::string> context;   // e.g. "Sample.Periodic"
        std::optional<std::string> format;
        std::optional<std::string> measurand; // e.g. "Energy.Active.Import.Register"
        std::optional<std::string> phase;
        std::optional<std::string> location;
        std::optional<std::string> unit;      // e.g. "Wh", "A", "V"
    };

    struct MeterValue {
        std::string timestamp;
        std::vector<SampledValue> sampled_value;
    };

    struct MeterValuesRequest {
        static constexpr const char* kAction = "MeterValues";
        int connector_id = 0;
        std::optional<int> transaction_id;
        std::vector<MeterValue> meter_value;
    };

    struct MeterValuesResponse {};  // empty payload

    void to_json(json& j, const SampledValue& v);
    void from_json(const json& j, SampledValue& v);
    void to_json(json& j, const MeterValue& v);
    void from_json(const json& j, MeterValue& v);
    void to_json(json& j, const MeterValuesRequest& v);
    void from_json(const json& j, MeterValuesRequest& v);
    void to_json(json& j, const MeterValuesResponse& v);
    void from_json(const json& j, MeterValuesResponse& v);

    // ---------- StopTransaction (YOUR EXERCISE) ----------

    enum class StopReason {
        EmergencyStop, EVDisconnected, HardReset, Local, Other, PowerLoss,
        Reboot, Remote, SoftReset, UnlockCommand, DeAuthorized
    };

    struct StopTransactionRequest {
        static constexpr const char* kAction = "StopTransaction";
        std::optional<std::string> id_tag;
        int meter_stop = 0;
        std::string timestamp;
        int transaction_id = 0;
        std::optional<StopReason> reason;
    };

    struct StopTransactionResponse {
        std::optional<IdTagInfo> id_tag_info;
    };

    void to_json(json& j, const StopTransactionRequest& v);
    void from_json(const json& j, StopTransactionRequest& v);
    void to_json(json& j, const StopTransactionResponse& v);
    void from_json(const json& j, StopTransactionResponse& v);
}  // namespace chargelink::ocpp