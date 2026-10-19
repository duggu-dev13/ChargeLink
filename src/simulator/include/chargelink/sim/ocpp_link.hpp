#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include "chargelink/ocpp/caller.hpp"
#include "chargelink/ocpp/reliable_sender.hpp"
#include "chargelink/sim/station_link.hpp"

namespace chargelink::sim {

    // Adapter: turns the typed IStationLink calls into OCPP CALLs and reads the replies back.
    //
    // StartTransaction and StopTransaction are billing records, so they go through a
    // ReliableSender: they wait in a queue while we are offline and are delivered in order
    // after we re-register. Everything else is best-effort.
    class OcppLink : public IStationLink {
    public:
        explicit OcppLink(ocpp::ICaller& caller) : caller_(caller), reliable_(caller) {}

        // The Station calls this: true once the CSMS has accepted us, false when the link is lost.
        void set_online(bool online) { reliable_.set_online(online); }
        std::size_t queued() const { return reliable_.pending(); }

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
        ocpp::ReliableSender reliable_;
    };

}  // namespace chargelink::sim