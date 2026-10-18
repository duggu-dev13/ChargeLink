#pragma once

#include <functional>
#include <optional>
#include <string>

#include "chargelink/ocpp/caller.hpp"
#include "chargelink/sim/station_link.hpp"

namespace chargelink::sim {

    // Adapter: turns the typed IStationLink calls into OCPP CALLs and reads the replies back.
    class OcppLink : public IStationLink {
    public:
        explicit OcppLink(ocpp::ICaller& caller) : caller_(caller) {}

        void send_status(const ocpp::StatusNotificationRequest& request) override;
        void authorize(const std::string& id_tag,
            std::function<void(std::optional<ocpp::AuthorizationStatus>)> on_result) override;
        void start_transaction(
            const ocpp::StartTransactionRequest& request,
            std::function<void(std::optional<ocpp::StartTransactionResponse>)> on_result) override;
        void stop_transaction(const ocpp::StopTransactionRequest& request) override;
        void send_meter_values(const ocpp::MeterValuesRequest& request) override;

    private:
        ocpp::ICaller& caller_;
    };

}  // namespace chargelink::sim