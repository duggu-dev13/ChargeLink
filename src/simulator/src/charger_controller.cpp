#include "chargelink/sim/charger_controller.hpp"

namespace chargelink::sim {

    using ocpp::StopReason;

    ChargerController::ChargerController(IHardware& hardware, ControllerConfig config)
        : hw_(hardware), cfg_(config) {
    }

    // ---------- inputs ----------

    bool ChargerController::authorize() {
        return fsm_.handle(Event::Authorized);
    }

    bool ChargerController::stop(StopReason reason) {
        if (!session_active_) {
            return false;
        }
        finish_session(reason);                  // contactor opens before anything else
        fsm_.handle(Event::ChargingStopped);     // Finishing (cable still in) or Available
        return true;
    }

    bool ChargerController::clear_fault() {
        if (fsm_.status() != ChargePointStatus::Faulted) {
            return false;
        }
        const PilotState pilot = hw_.read_pilot();
        if (pilot == PilotState::Error || pilot == PilotState::ChargingVentilated) {
            return false;  // the cause is still there
        }
        if (hw_.temperature_c() >= cfg_.temperature_recover_c) {
            return false;  // still too hot
        }
        return fsm_.handle(Event::FaultCleared);
    }

    // ---------- the polling loop ----------

    void ChargerController::tick() {
        const PilotState pilot = hw_.read_pilot();
        check_safety(pilot);            // 1. safety first
        track_cable(pilot);             // 2. plug / unplug
        drive_session(pilot);           // 3. start, pause, resume
        enforce_contactor_invariant();  // 4. last line of defence
    }

    void ChargerController::check_safety(PilotState pilot) {
        if (fsm_.status() == ChargePointStatus::Faulted) {
            return;  // already handled
        }
        if (pilot == PilotState::Error || pilot == PilotState::ChargingVentilated) {
            // E is a pilot fault; D (needs ventilation) is not supported by this charger.
            fault(ChargePointErrorCode::OtherError);
        }
        else if (hw_.temperature_c() > cfg_.max_temperature_c) {
            fault(ChargePointErrorCode::HighTemperature);
        }
    }

    void ChargerController::track_cable(PilotState pilot) {
        if (pilot == PilotState::Error || pilot == PilotState::ChargingVentilated) {
            return;  // the pilot level tells us nothing reliable about the cable
        }
        const bool plugged_now = (pilot != PilotState::NotConnected);

        if (plugged_now && !fsm_.cable_plugged()) {
            fsm_.handle(Event::CablePlugged);
        }
        else if (!plugged_now && fsm_.cable_plugged()) {
            if (session_active_) {
                finish_session(StopReason::EVDisconnected);  // contactor opens first
            }
            else {
                hw_.clear_current_limit();                   // stop offering energy
            }
            fsm_.handle(Event::CableUnplugged);
        }
    }

    void ChargerController::drive_session(PilotState pilot) {
        switch (fsm_.status()) {
        case ChargePointStatus::Preparing:
            if (fsm_.cable_plugged() && fsm_.authorized()) {
                if (hw_.current_limit() == 0.0) {
                    hw_.set_current_limit(cfg_.current_limit_amps);  // offer energy
                }
                // The contactor closes only when the car answers with state C.
                if (pilot == PilotState::Charging) {
                    start_session();
                }
            }
            break;

        case ChargePointStatus::Charging:
            if (pilot == PilotState::Connected) {  // car stopped asking: pause
                hw_.set_contactor(false);
                fsm_.handle(Event::EvPaused);
            }
            break;

        case ChargePointStatus::SuspendedEV:
            if (pilot == PilotState::Charging) {   // car asks again: resume
                hw_.set_contactor(true);
                fsm_.handle(Event::EvResumed);
            }
            break;

        default:
            break;
        }
    }

    // The rule that makes the whole thing safe: power may flow only while we are Charging.
    void ChargerController::enforce_contactor_invariant() {
        if (hw_.contactor_closed() && fsm_.status() != ChargePointStatus::Charging) {
            hw_.set_contactor(false);
        }
    }

    // ---------- session helpers ----------

    void ChargerController::start_session() {
        hw_.set_contactor(true);
        session_active_ = true;

        // Report the session BEFORE the state change: OCPP expects StartTransaction to come
        // before the StatusNotification(Charging) that the state change triggers.
        if (on_session_started_) {
            on_session_started_(hw_.energy_wh());
        }

        if (!fsm_.handle(Event::ChargingStarted)) {
            // Cannot happen (drive_session checked the conditions), but never leave power on.
            power_down();
            session_active_ = false;
        }
    }

    void ChargerController::finish_session(StopReason reason) {
        power_down();  // before any bookkeeping or callbacks
        session_active_ = false;
        if (on_session_ended_) {
            on_session_ended_(hw_.energy_wh(), reason);
        }
    }

    void ChargerController::power_down() {
        hw_.set_contactor(false);
        hw_.clear_current_limit();
    }

    void ChargerController::fault(ChargePointErrorCode code) {
        if (session_active_) {
            finish_session(StopReason::Other);  // report the aborted transaction
        }
        else {
            power_down();
        }
        fsm_.raise_fault(code);
    }

}  // namespace chargelink::sim