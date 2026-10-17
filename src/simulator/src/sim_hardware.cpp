#include "chargelink/sim/sim_hardware.hpp"

#include <algorithm>
#include <cmath>

namespace chargelink::sim {

    PilotState SimHardware::read_pilot() const {
        if (pilot_error_) {
            return PilotState::Error;
        }
        if (!car_plugged_) {
            return PilotState::NotConnected;
        }
        // The car only asks for energy once it sees a valid PWM from the charger.
        if (car_wants_charge_ && limit_amps_ > 0.0) {
            return PilotState::Charging;
        }
        return PilotState::Connected;
    }

    bool SimHardware::set_current_limit(double amps) {
        if (!amps_to_duty_percent(amps)) {
            return false;  // out of range: leave the previous limit untouched
        }
        limit_amps_ = amps;
        return true;
    }

    void SimHardware::clear_current_limit() {
        limit_amps_ = 0.0;
    }

    void SimHardware::plug_car(double car_max_amps) {
        car_plugged_ = true;
        car_max_amps_ = car_max_amps;
    }

    void SimHardware::unplug_car() {
        car_plugged_ = false;
    }

    double SimHardware::current_draw_amps() const {
        // Current flows only if the contactor is closed AND the car is actually asking.
        if (contactor_closed_ && read_pilot() == PilotState::Charging) {
            return std::min(limit_amps_, car_max_amps_);
        }
        return 0.0;
    }

    void SimHardware::tick(double seconds) {
        if (!(seconds > 0.0)) {
            return;
        }
        const double amps = current_draw_amps();

        // energy (Wh) = power (W) * time (h)
        energy_wh_ += kVoltage * amps * seconds / 3600.0;

        // first-order thermal model: temperature moves toward its steady-state target
        const double target = ambient_c_ + kHeatPerAmpSq * amps * amps;
        temperature_c_ += (target - temperature_c_) * (1.0 - std::exp(-seconds / kThermalTauSec));
    }

}  // namespace chargelink::sim