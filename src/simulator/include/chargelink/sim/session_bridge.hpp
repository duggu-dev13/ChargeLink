#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "chargelink/ocpp/messages.hpp"
#include "chargelink/sim/charger_controller.hpp"
#include "chargelink/sim/hardware.hpp"
#include "chargelink/sim/station_link.hpp"

namespace chargelink::sim {

    // Turns controller events into OCPP messages and OCPP answers into controller actions.
    // Not thread-safe: use it from the same thread that drives the controller.
    //
    // The controller, link and hardware must outlive the bridge.
    class SessionBridge {
    public:
        enum class TapResult {
            NotRegistered,   // the CSMS has not accepted us yet
            Busy,            // a tap makes no sense right now
            StoppedSession,  // the same tag was tapped during a session: we stop it
            Asked,           // we asked the CSMS to authorize the tag
        };

        using Clock = std::function<std::string()>;  // returns an ISO 8601 UTC timestamp

        SessionBridge(ChargerController& controller, IStationLink& link, IHardware& hardware,
            Clock clock, int connector_id = 1);
        ~SessionBridge();

        SessionBridge(const SessionBridge&) = delete;
        SessionBridge& operator=(const SessionBridge&) = delete;

        // Call with true once BootNotification is accepted, and with false when the connection
        // is lost. Registering sends a snapshot of the current status.
        void set_registered(bool registered);
        bool registered() const { return registered_; }

        // An RFID card was tapped.
        TapResult tap(const std::string& id_tag);

        // Call periodically; sends a MeterValues message while a known transaction is running.
        void sample_meter_values();

    private:
        struct PendingStop {
            int meter_stop = 0;
            ocpp::StopReason reason = ocpp::StopReason::Local;
            std::string timestamp;
        };

        struct Transaction {
            std::string id_tag;
            std::optional<int> transaction_id;  // known once the CSMS has answered StartTransaction
            bool start_failed = false;          // the CSMS never learned about this transaction
            std::optional<PendingStop> pending_stop;  // the session ended before the id arrived
        };

        void on_status_change(ChargePointStatus to, ChargePointErrorCode error);
        void on_session_started(int meter_start_wh);
        void on_session_ended(int meter_stop_wh, ocpp::StopReason reason);
        void on_start_reply(const std::optional<ocpp::StartTransactionResponse>& reply);
        void send_stop(const PendingStop& stop);
        void send_status(int connector_id, ChargePointStatus status, ChargePointErrorCode error);

        ChargerController& ctl_;
        IStationLink& link_;
        IHardware& hw_;
        Clock clock_;
        int connector_id_;

        bool registered_ = false;
        std::string current_tag_;
        std::optional<Transaction> txn_;

        // Callbacks given to the link capture a weak_ptr to this token. If the bridge is gone,
        // a late reply finds the token expired and does nothing.
        std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    };

}  // namespace chargelink::sim