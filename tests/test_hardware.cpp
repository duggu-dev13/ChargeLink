#include <gtest/gtest.h>

#include <limits>

#include "chargelink/sim/sim_hardware.hpp"

using namespace chargelink::sim;

// ---------- duty cycle conversion ----------

TEST(Pilot, DutyCycleLowRange) {
    EXPECT_NEAR(*amps_to_duty_percent(6.0), 10.0, 0.01);
    EXPECT_NEAR(*amps_to_duty_percent(16.0), 26.67, 0.01);
    EXPECT_NEAR(*amps_to_duty_percent(32.0), 53.33, 0.01);
    EXPECT_NEAR(*amps_to_duty_percent(51.0), 85.0, 0.01);
}

TEST(Pilot, DutyCycleHighRange) {
    EXPECT_NEAR(*amps_to_duty_percent(63.0), 89.2, 0.01);
    EXPECT_NEAR(*amps_to_duty_percent(80.0), 96.0, 0.01);
}

TEST(Pilot, DutyCycleRejectsOutOfRange) {
    EXPECT_FALSE(amps_to_duty_percent(5.9).has_value());
    EXPECT_FALSE(amps_to_duty_percent(80.1).has_value());
    EXPECT_FALSE(amps_to_duty_percent(-1.0).has_value());
    EXPECT_FALSE(amps_to_duty_percent(std::numeric_limits<double>::quiet_NaN()).has_value());
}

// ---------- pilot state ----------

TEST(SimHardware, PilotFollowsTheCar) {
    SimHardware hw;
    EXPECT_EQ(hw.read_pilot(), PilotState::NotConnected);

    hw.plug_car();
    EXPECT_EQ(hw.read_pilot(), PilotState::Connected);  // no PWM yet, car waits

    ASSERT_TRUE(hw.set_current_limit(16.0));
    EXPECT_EQ(hw.read_pilot(), PilotState::Charging);   // car sees PWM, asks for energy

    hw.car_wants_charge(false);
    EXPECT_EQ(hw.read_pilot(), PilotState::Connected);  // car paused itself

    hw.car_wants_charge(true);
    hw.clear_current_limit();
    EXPECT_EQ(hw.read_pilot(), PilotState::Connected);  // charger stopped the PWM

    hw.unplug_car();
    EXPECT_EQ(hw.read_pilot(), PilotState::NotConnected);
}

TEST(SimHardware, PilotErrorOverridesEverything) {
    SimHardware hw;
    hw.plug_car();
    hw.set_current_limit(16.0);
    hw.inject_pilot_error(true);
    EXPECT_EQ(hw.read_pilot(), PilotState::Error);
    hw.inject_pilot_error(false);
    EXPECT_EQ(hw.read_pilot(), PilotState::Charging);
}

TEST(SimHardware, InvalidLimitIsRejectedAndKeepsOldValue) {
    SimHardware hw;
    ASSERT_TRUE(hw.set_current_limit(16.0));
    EXPECT_FALSE(hw.set_current_limit(3.0));
    EXPECT_DOUBLE_EQ(hw.current_limit(), 16.0);
}

// ---------- energy ----------

TEST(SimHardware, EnergyAccumulatesOnlyWhenCharging) {
    SimHardware hw;
    hw.plug_car(32.0);
    hw.set_current_limit(16.0);

    hw.tick(3600.0);  // contactor still open: nothing flows
    EXPECT_EQ(hw.energy_wh(), 0);

    hw.set_contactor(true);
    hw.tick(3600.0);  // one hour at 16 A, 230 V
    EXPECT_NEAR(hw.energy_wh(), 230 * 16, 2);
}

TEST(SimHardware, CarCurrentIsLimitedByTheSmallerOfCarAndCharger) {
    SimHardware hw;
    hw.plug_car(10.0);  // weak car
    hw.set_current_limit(32.0);
    hw.set_contactor(true);
    EXPECT_DOUBLE_EQ(hw.current_draw_amps(), 10.0);
}

TEST(SimHardware, NoEnergyWhenCarPauses) {
    SimHardware hw;
    hw.plug_car();
    hw.set_current_limit(16.0);
    hw.set_contactor(true);
    hw.tick(60.0);
    const int before = hw.energy_wh();

    hw.car_wants_charge(false);
    hw.tick(600.0);
    EXPECT_EQ(hw.energy_wh(), before);
}

TEST(SimHardware, NoEnergyWithoutACar) {
    SimHardware hw;
    hw.set_current_limit(16.0);
    hw.set_contactor(true);  // closed contactor, but nobody plugged in
    hw.tick(3600.0);
    EXPECT_EQ(hw.energy_wh(), 0);
}

TEST(SimHardware, UnpluggingDoesNotOpenTheContactor) {
    SimHardware hw;
    hw.plug_car();
    hw.set_current_limit(16.0);
    hw.set_contactor(true);
    hw.unplug_car();
    EXPECT_TRUE(hw.contactor_closed());  // the controller must handle this
    EXPECT_DOUBLE_EQ(hw.current_draw_amps(), 0.0);
}

// ---------- temperature ----------

TEST(SimHardware, TemperatureRisesUnderLoadAndCoolsDown) {
    SimHardware hw;
    hw.plug_car(32.0);
    hw.set_current_limit(32.0);
    hw.set_contactor(true);

    EXPECT_NEAR(hw.temperature_c(), 25.0, 0.01);
    hw.tick(600.0);  // 10 time constants: essentially steady state
    EXPECT_GT(hw.temperature_c(), 44.0);
    EXPECT_LT(hw.temperature_c(), 46.0);  // 25 + 0.02 * 32^2 = 45.5

    hw.set_contactor(false);
    hw.tick(600.0);
    EXPECT_NEAR(hw.temperature_c(), 25.0, 1.0);
}

TEST(SimHardware, AmbientTemperatureMatters) {
    SimHardware hw;
    hw.set_ambient_c(40.0);
    hw.tick(600.0);
    EXPECT_NEAR(hw.temperature_c(), 40.0, 0.5);
}

TEST(SimHardware, NonPositiveTickIsIgnored) {
    SimHardware hw;
    hw.plug_car();
    hw.set_current_limit(16.0);
    hw.set_contactor(true);
    hw.tick(0.0);
    hw.tick(-5.0);
    EXPECT_EQ(hw.energy_wh(), 0);
}

// ---------- usable through the interface ----------

TEST(SimHardware, WorksThroughTheInterface) {
    SimHardware sim;
    IHardware& hw = sim;  // the controller will only ever see this
    EXPECT_EQ(hw.read_pilot(), PilotState::NotConnected);
    EXPECT_TRUE(hw.set_current_limit(20.0));
    hw.set_contactor(true);
    EXPECT_TRUE(hw.contactor_closed());
}