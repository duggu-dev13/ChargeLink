#pragma once

#include <functional>
#include <optional>
#include <string>

#include "chargelink/ocpp/messages.hpp"

namespace chargelink::sim {

    // Everything the charger needs from the central system, in typed form.
    //
    // In production an adapter turns these calls into OCPP messages over the WebSocket.
    // In tests a fake records them and lets the test deliver replies by hand.
    //
    // For calls that expect an answer, the callback receives nullopt when the call failed
    // (timeout, CALLERROR, lost connection, or an unreadable reply).
    class IStationLink {
    public:
        virtual ~IStationLink() = default;

        virtual void send_status(const ocpp::StatusNotificationRequest& request) = 0;

        virtual void authorize(
            const std::string& id_tag,
            std::function<void(std::optional<ocpp::AuthorizationStatus>)> on_result) = 0;

        virtual void start_transaction(
            const ocpp::StartTransactionRequest& request,
            std::function<void(std::optional<ocpp::StartTransactionResponse>)> on_result) = 0;

        virtual void stop_transaction(const ocpp::StopTransactionRequest& request) = 0;
        virtual void send_meter_values(const ocpp::MeterValuesRequest& request) = 0;
    };

}  // namespace chargelink::sim