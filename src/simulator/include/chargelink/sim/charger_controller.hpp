#pragma once

#include <functional>
#include <utility>

#include "chargelink/ocpp/messages.hpp"
#include "chargelink/sim/connector_fsm.hpp"
#include "chargelink/sim/hardware.hpp"

namespace chargelink::sim {

    struct ControllerConfig {
        double current_limit_amps = 16.0;    // what we advertise on the pilot
        double max_temperature_c = 70.0;     // above this: HighTemperature fault
        double temperature_recover_c = 60.0; // a fault may only be cleared below this (hysteresis)
    };

    // The charger's brain: it owns the state machine and is the ONLY code allowed to
    // touch the contactor. It reacts to what the hardware reports and enforces the safety rules.
    // Not thread-safe: one owner calls tick() and the other methods.
    class ChargerController {
    public:
        using SessionStartedCallback = std::function<void(int meter_start_wh)>;
        using SessionEndedCallback = std::function<void(int meter_stop_wh, ocpp::StopReason reason)>;

        // The hardware must outlive the controller (we keep a reference, we don't own it).
        explicit ChargerController(IHardware& hardware, ControllerConfig config = {});

        void set_on_status_change(ConnectorFsm::ChangeCallback callback) {
            fsm_.set_on_change(std::move(callback));
        }
        void set_on_session_started(SessionStartedCallback callback) {
            on_session_started_ = std::move(callback);
        }
        void set_on_session_ended(SessionEndedCallback callback) {
            on_session_ended_ = std::move(callback);
        }

        // Inputs from the outside world
        bool authorize();                          // the CSMS accepted an id tag
        bool stop(ocpp::StopReason reason);        // local or remote stop; false if no session
        bool clear_fault();                        // false if not faulted or the cause persists

        // Poll the hardware and react. Call this periodically (for example every 100 ms).
        void tick();

        // Read-only views
        ChargePointStatus status() const { return fsm_.status(); }
        ChargePointErrorCode error_code() const { return fsm_.error_code(); }
        bool session_active() const { return session_active_; }

    private:
        void check_safety(PilotState pilot);
        void track_cable(PilotState pilot);
        void drive_session(PilotState pilot);
        void enforce_contactor_invariant();

        void start_session();
        void finish_session(ocpp::StopReason reason);  // opens the contactor FIRST
        void power_down();                             // contactor open + pilot PWM off
        void fault(ChargePointErrorCode code);

        IHardware& hw_;
        ControllerConfig cfg_;
        ConnectorFsm fsm_;
        bool session_active_ = false;
        SessionStartedCallback on_session_started_;
        SessionEndedCallback on_session_ended_;
    };

}  // namespace chargelink::sim