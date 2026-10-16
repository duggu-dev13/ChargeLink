#pragma once

#include <functional>
#include <utility>

#include "chargelink/ocpp/messages.hpp"

namespace chargelink::sim {

    using ocpp::ChargePointErrorCode;
    using ocpp::ChargePointStatus;

    // Things that can happen to a connector.
    enum class Event {
        CablePlugged,     // car cable inserted
        CableUnplugged,   // car cable removed
        Authorized,       // an id tag was accepted by the CSMS
        ChargingStarted,  // transaction started, contactor closed
        ChargingStopped,  // transaction ended, contactor open
        EvPaused,         // car stopped drawing current
        EvResumed,
        EvsePaused,       // charger limited the current to zero
        EvseResumed,
        FaultCleared,
        MadeUnavailable,  // operator took the connector out of service
        MadeAvailable,
    };

    class ConnectorFsm {
    public:
        using ChangeCallback = std::function<void(ChargePointStatus from, ChargePointStatus to,
            ChargePointErrorCode error)>;

        ChargePointStatus status() const { return status_; }
        ChargePointErrorCode error_code() const { return error_; }
        bool cable_plugged() const { return plugged_; }
        bool authorized() const { return authorized_; }

        void set_on_change(ChangeCallback callback) { on_change_ = std::move(callback); }

        // Feed an event in. Returns true if the event was legal in the current state
        // (the state may or may not have changed); false if it was rejected.
        bool handle(Event event);

        // A hardware fault. Allowed from any state. Returns false if code is NoError.
        bool raise_fault(ChargePointErrorCode code);

    private:
        // one handler per state
        bool on_available(Event e);
        bool on_preparing(Event e);
        bool on_charging(Event e);
        bool on_suspended_ev(Event e);
        bool on_suspended_evse(Event e);
        bool on_finishing(Event e);
        bool on_faulted(Event e);
        bool on_unavailable(Event e);

        // helpers
        void move_to(ChargePointStatus next);
        void end_session();       // transaction over: Finishing if still plugged, else Available
        void abort_by_unplug();   // cable yanked mid-session: straight to Available

        ChargePointStatus status_ = ChargePointStatus::Available;
        ChargePointErrorCode error_ = ChargePointErrorCode::NoError;
        bool plugged_ = false;
        bool authorized_ = false;
        ChangeCallback on_change_;
    };

}  // namespace chargelink::sim