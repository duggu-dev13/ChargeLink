#include "chargelink/sim/connector_fsm.hpp"

namespace chargelink::sim {

    bool ConnectorFsm::handle(Event event) {
        switch (status_) {
        case ChargePointStatus::Available:     return on_available(event);
        case ChargePointStatus::Preparing:     return on_preparing(event);
        case ChargePointStatus::Charging:      return on_charging(event);
        case ChargePointStatus::SuspendedEV:   return on_suspended_ev(event);
        case ChargePointStatus::SuspendedEVSE: return on_suspended_evse(event);
        case ChargePointStatus::Finishing:     return on_finishing(event);
        case ChargePointStatus::Faulted:       return on_faulted(event);
        case ChargePointStatus::Unavailable:   return on_unavailable(event);
        case ChargePointStatus::Reserved:      return false;  // not supported
        }
        return false;
    }

    // ---------- helpers ----------

    void ConnectorFsm::move_to(ChargePointStatus next) {
        if (next == status_) {
            return;
        }
        const ChargePointStatus from = status_;
        status_ = next;
        if (next != ChargePointStatus::Faulted) {
            error_ = ChargePointErrorCode::NoError;  // leaving Faulted clears the error
        }
        if (on_change_) {
            on_change_(from, next, error_);
        }
    }

    void ConnectorFsm::end_session() {
        authorized_ = false;
        move_to(plugged_ ? ChargePointStatus::Finishing : ChargePointStatus::Available);
    }

    void ConnectorFsm::abort_by_unplug() {
        plugged_ = false;
        authorized_ = false;
        move_to(ChargePointStatus::Available);
    }

    // ---------- fault entry point (allowed from any state) ----------

    bool ConnectorFsm::raise_fault(ChargePointErrorCode code) {
        if (code == ChargePointErrorCode::NoError) {
            return false;  // validate first, change nothing
        }
        authorized_ = false;  // a fault aborts any session
        error_ = code;        // set BEFORE move_to so the callback carries it
        move_to(ChargePointStatus::Faulted);
        return true;
    }

    // ---------- one function per state ----------

    bool ConnectorFsm::on_available(Event e) {
        switch (e) {
        case Event::CablePlugged:
            plugged_ = true;
            move_to(ChargePointStatus::Preparing);
            return true;
        case Event::Authorized:  // tag tapped before the car is plugged in
            authorized_ = true;
            move_to(ChargePointStatus::Preparing);
            return true;
        case Event::MadeUnavailable:
            move_to(ChargePointStatus::Unavailable);
            return true;
        default:
            return false;
        }
    }

    bool ConnectorFsm::on_preparing(Event e) {
        switch (e) {
        case Event::CablePlugged:
            if (plugged_) return false;  // already plugged in
            plugged_ = true;
            return true;
        case Event::Authorized:
            authorized_ = true;
            return true;
        case Event::CableUnplugged:
            if (!plugged_) return false;  // nothing to unplug
            plugged_ = false;
            authorized_ = false;  // authorization does not survive an unplug
            move_to(ChargePointStatus::Available);
            return true;
        case Event::ChargingStarted:
            if (!plugged_ || !authorized_) return false;  // need BOTH
            move_to(ChargePointStatus::Charging);
            return true;
        default:
            return false;
        }
    }

    bool ConnectorFsm::on_charging(Event e) {
        switch (e) {
        case Event::EvPaused:
            move_to(ChargePointStatus::SuspendedEV);
            return true;
        case Event::EvsePaused:
            move_to(ChargePointStatus::SuspendedEVSE);
            return true;
        case Event::ChargingStopped:
            end_session();
            return true;
        case Event::CableUnplugged:
            abort_by_unplug();  // the caller must stop the transaction (EVDisconnected)
            return true;
        default:
            return false;
        }
    }

    bool ConnectorFsm::on_suspended_ev(Event e) {
        switch (e) {
        case Event::EvResumed:
            move_to(ChargePointStatus::Charging);
            return true;
        case Event::ChargingStopped:
            end_session();
            return true;
        case Event::CableUnplugged:
            abort_by_unplug();
            return true;
        default:
            return false;
        }
    }

    bool ConnectorFsm::on_suspended_evse(Event e) {
        switch (e) {
        case Event::EvseResumed:
            move_to(ChargePointStatus::Charging);
            return true;
        case Event::ChargingStopped:
            end_session();
            return true;
        case Event::CableUnplugged:
            abort_by_unplug();
            return true;
        default:
            return false;
        }
    }

    bool ConnectorFsm::on_finishing(Event e) {
        if (e == Event::CableUnplugged) {
            plugged_ = false;
            move_to(ChargePointStatus::Available);
            return true;
        }
        return false;
    }

    bool ConnectorFsm::on_faulted(Event e) {
        switch (e) {
        case Event::FaultCleared:
            move_to(plugged_ ? ChargePointStatus::Preparing : ChargePointStatus::Available);
            return true;
        case Event::CablePlugged:  // the cable is still tracked while faulted
            plugged_ = true;
            return true;
        case Event::CableUnplugged:
            plugged_ = false;
            return true;
        default:
            return false;
        }
    }

    bool ConnectorFsm::on_unavailable(Event e) {
        switch (e) {
        case Event::MadeAvailable:
            move_to(plugged_ ? ChargePointStatus::Preparing : ChargePointStatus::Available);
            return true;
        case Event::CablePlugged:
            plugged_ = true;
            return true;
        case Event::CableUnplugged:
            plugged_ = false;
            return true;
        default:
            return false;
        }
    }

}  // namespace chargelink::sim