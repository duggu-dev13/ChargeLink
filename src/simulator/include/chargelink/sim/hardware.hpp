#pragma once

#include <optional>

namespace chargelink::sim {

    // Control pilot states (IEC 61851-1).
    enum class PilotState {
        NotConnected,        // A: no car
        Connected,           // B: car plugged in, not asking for energy
        Charging,            // C: car asks for energy
        ChargingVentilated,  // D: car asks for energy and ventilation
        Error,               // E: pilot fault
    };

    const char* to_string(PilotState state);

    // Maximum current -> PWM duty cycle in percent (IEC 61851-1).
    // Returns nullopt when the current is outside the allowed 6..80 A range.
    std::optional<double> amps_to_duty_percent(double amps);

    // Everything the charger logic is allowed to do to the real world.
    class IHardware {
    public:
        virtual ~IHardware() = default;

        // --- control pilot ---
        virtual PilotState read_pilot() const = 0;
        // Advertise a current limit through the PWM. Returns false if not allowed (6..80 A).
        virtual bool set_current_limit(double amps) = 0;
        // Stop the PWM: the car must not charge.
        virtual void clear_current_limit() = 0;
        virtual double current_limit() const = 0;  // 0 when no PWM is active

        // --- contactor (the high-power relay) ---
        virtual void set_contactor(bool closed) = 0;
        virtual bool contactor_closed() const = 0;

        // --- measurements ---
        virtual int energy_wh() const = 0;        // total register, only ever increases
        virtual double temperature_c() const = 0;
    };

}  // namespace chargelink::sim