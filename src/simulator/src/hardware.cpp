#include "chargelink/sim/hardware.hpp"

namespace chargelink::sim {

    const char* to_string(PilotState state) {
        switch (state) {
        case PilotState::NotConnected:       return "A";
        case PilotState::Connected:          return "B";
        case PilotState::Charging:           return "C";
        case PilotState::ChargingVentilated: return "D";
        case PilotState::Error:              return "E";
        }
        return "?";
    }

    std::optional<double> amps_to_duty_percent(double amps) {
        // Written so that NaN is rejected too (every comparison with NaN is false).
        if (!(amps >= 6.0 && amps <= 80.0)) {
            return std::nullopt;
        }
        if (amps <= 51.0) {
            return amps / 0.6;
        }
        return amps / 2.5 + 64.0;
    }

}  // namespace chargelink::sim