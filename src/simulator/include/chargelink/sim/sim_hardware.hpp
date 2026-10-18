#pragma once

#include "chargelink/sim/hardware.hpp"

namespace chargelink::sim {

    // Software model of a charger's hardware plus a car. Not thread-safe: one owner drives it.
    class SimHardware : public IHardware {
    public:
        // ----- IHardware -----
        PilotState read_pilot() const override;
        bool set_current_limit(double amps) override;
        void clear_current_limit() override;
        double current_limit() const override { return limit_amps_; }
        void set_contactor(bool closed) override { contactor_closed_ = closed; }
        bool contactor_closed() const override { return contactor_closed_; }
        int energy_wh() const override { return static_cast<int>(energy_wh_); }
        double current_amps() const override { return current_draw_amps(); }
        double temperature_c() const override { return temperature_c_; }

        // ----- simulation controls (not part of IHardware) -----
        void plug_car(double car_max_amps = 32.0);
        void unplug_car();                       // does NOT open the contactor: that is the controller's job
        void car_wants_charge(bool wants) { car_wants_charge_ = wants; }
        void inject_pilot_error(bool error) { pilot_error_ = error; }
        void set_ambient_c(double celsius) { ambient_c_ = celsius; }

        // Advance simulated time: accumulates energy and updates temperature.
        void tick(double seconds);

        bool car_plugged() const { return car_plugged_; }
        double current_draw_amps() const;        // what the car pulls right now

    private:
        static constexpr double kVoltage = 230.0;       // single phase
        static constexpr double kHeatPerAmpSq = 0.02;   // deg C rise per A^2 at steady state
        static constexpr double kThermalTauSec = 60.0;  // thermal time constant

        bool car_plugged_ = false;
        bool car_wants_charge_ = true;
        bool pilot_error_ = false;
        double car_max_amps_ = 32.0;

        double limit_amps_ = 0.0;
        bool contactor_closed_ = false;

        double energy_wh_ = 0.0;
        double temperature_c_ = 25.0;
        double ambient_c_ = 25.0;
    };

}  // namespace chargelink::sim