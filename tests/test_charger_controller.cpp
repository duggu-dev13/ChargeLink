#include <gtest/gtest.h>

#include <vector>
#include <string>

#include "chargelink/sim/charger_controller.hpp"
#include "chargelink/sim/sim_hardware.hpp"

using namespace chargelink::sim;
using chargelink::ocpp::StopReason;

namespace {

    // A charger with a simulated car, plus recorders for everything the controller reports.
    struct Rig {
        SimHardware hw;
        ChargerController ctl{ hw };

        std::vector<ChargePointStatus> statuses;
        int started_count = 0;
        int meter_start = -1;
        int ended_count = 0;
        int meter_stop = -1;
        StopReason end_reason = StopReason::Local;
        ChargePointErrorCode last_error = ChargePointErrorCode::NoError;

        Rig() {
            ctl.set_on_status_change(
                [this](ChargePointStatus, ChargePointStatus to, ChargePointErrorCode err) {
                    statuses.push_back(to);
                    last_error = err;
                });
            ctl.set_on_session_started([this](int wh) {
                ++started_count;
                meter_start = wh;
                });
            ctl.set_on_session_ended([this](int wh, StopReason r) {
                ++ended_count;
                meter_stop = wh;
                end_reason = r;
                });
        }

        Rig(const Rig&) = delete;
        Rig& operator=(const Rig&) = delete;

        // Plug in, authorize and tick until charging has started.
        void start_charging() {
            hw.plug_car();
            ctl.tick();       // sees the cable
            ctl.authorize();
            ctl.tick();       // offers energy (PWM on)
            ctl.tick();       // the car answers with C: contactor closes
        }

        // Let the world run: the charger polls, then time passes, then it polls again.
        void run(double seconds) {
            ctl.tick();
            hw.tick(seconds);
            ctl.tick();
        }
    };

}  // namespace

// ---------- starting a session ----------

TEST(Controller, PlugInGoesPreparingWithNoPower) {
    Rig r;
    r.hw.plug_car();
    r.ctl.tick();
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Preparing);
    EXPECT_FALSE(r.hw.contactor_closed());
    EXPECT_DOUBLE_EQ(r.hw.current_limit(), 0.0);  // nothing offered yet
}

TEST(Controller, NoPowerWithoutAuthorization) {
    Rig r;
    r.hw.plug_car();
    for (int i = 0; i < 5; ++i) r.ctl.tick();
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Preparing);
    EXPECT_FALSE(r.hw.contactor_closed());
    EXPECT_DOUBLE_EQ(r.hw.current_limit(), 0.0);
}

TEST(Controller, AuthorizedSessionStartsInTwoTicks) {
    Rig r;
    r.hw.plug_car();
    r.ctl.tick();
    EXPECT_TRUE(r.ctl.authorize());

    r.ctl.tick();  // offers energy, but the car has not answered yet
    EXPECT_DOUBLE_EQ(r.hw.current_limit(), 16.0);
    EXPECT_FALSE(r.hw.contactor_closed());
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Preparing);

    r.ctl.tick();  // car now in state C
    EXPECT_TRUE(r.hw.contactor_closed());
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Charging);
    EXPECT_TRUE(r.ctl.session_active());
    EXPECT_EQ(r.started_count, 1);
    EXPECT_EQ(r.meter_start, 0);
}

TEST(Controller, ContactorWaitsForTheCarToAskForEnergy) {
    Rig r;
    r.hw.plug_car();
    r.hw.car_wants_charge(false);  // the car is plugged in but not ready
    r.ctl.tick();
    r.ctl.authorize();
    for (int i = 0; i < 5; ++i) r.ctl.tick();
    EXPECT_FALSE(r.hw.contactor_closed());
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Preparing);

    r.hw.car_wants_charge(true);
    r.ctl.tick();
    EXPECT_TRUE(r.hw.contactor_closed());
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Charging);
}

TEST(Controller, AuthorizeBeforePlugging) {
    Rig r;
    EXPECT_TRUE(r.ctl.authorize());
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Preparing);
    r.ctl.tick();
    EXPECT_DOUBLE_EQ(r.hw.current_limit(), 0.0);  // no cable, nothing to offer

    r.hw.plug_car();
    r.ctl.tick();  // sees the cable and offers energy
    r.ctl.tick();  // car answers
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Charging);
}

// ---------- charging and stopping ----------

TEST(Controller, EnergyIsMeteredAndStopReportsIt) {
    Rig r;
    r.start_charging();
    r.run(3600.0);  // one hour at 16 A and 230 V = 3680 Wh
    EXPECT_NEAR(r.hw.energy_wh(), 3680, 2);

    EXPECT_TRUE(r.ctl.stop(StopReason::Local));
    EXPECT_EQ(r.ended_count, 1);
    EXPECT_NEAR(r.meter_stop, 3680, 2);
    EXPECT_TRUE(r.end_reason == StopReason::Local);
    EXPECT_FALSE(r.hw.contactor_closed());
    EXPECT_DOUBLE_EQ(r.hw.current_limit(), 0.0);  // we stopped offering energy
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Finishing);  // cable still in
    EXPECT_FALSE(r.ctl.session_active());
}

TEST(Controller, StopWithoutASessionIsRejected) {
    Rig r;
    EXPECT_FALSE(r.ctl.stop(StopReason::Remote));
    r.hw.plug_car();
    r.ctl.tick();
    EXPECT_FALSE(r.ctl.stop(StopReason::Remote));  // plugged in but not charging
    EXPECT_EQ(r.ended_count, 0);
}

TEST(Controller, SecondStopIsRejected) {
    Rig r;
    r.start_charging();
    EXPECT_TRUE(r.ctl.stop(StopReason::Remote));
    EXPECT_FALSE(r.ctl.stop(StopReason::Remote));
    EXPECT_EQ(r.ended_count, 1);  // reported once only
}

TEST(Controller, FullSessionStatusSequence) {
    Rig r;
    r.start_charging();
    r.run(60.0);
    r.ctl.stop(StopReason::Local);
    r.hw.unplug_car();
    r.ctl.tick();

    std::vector<ChargePointStatus> expected = {
        ChargePointStatus::Preparing, ChargePointStatus::Charging,
        ChargePointStatus::Finishing, ChargePointStatus::Available };
    EXPECT_EQ(r.statuses.size(), expected.size());
    EXPECT_TRUE(r.statuses == expected);
}

TEST(Controller, UnplugDuringChargingOpensTheContactorAndReports) {
    Rig r;
    r.start_charging();
    r.run(600.0);
    r.hw.unplug_car();  // the car is yanked out while the contactor is closed
    r.ctl.tick();

    EXPECT_FALSE(r.hw.contactor_closed());
    EXPECT_EQ(r.ended_count, 1);
    EXPECT_TRUE(r.end_reason == StopReason::EVDisconnected);
    EXPECT_GT(r.meter_stop, 0);
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Available);
}

TEST(Controller, UnplugBeforeChargingStopsOffering) {
    Rig r;
    r.hw.plug_car();
    r.ctl.tick();
    r.ctl.authorize();
    r.ctl.tick();  // PWM on, car not yet answered in this tick
    EXPECT_DOUBLE_EQ(r.hw.current_limit(), 16.0);
    r.hw.unplug_car();
    r.ctl.tick();
    EXPECT_DOUBLE_EQ(r.hw.current_limit(), 0.0);
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Available);
    EXPECT_EQ(r.ended_count, 0);  // there was never a transaction
}

// ---------- car pauses ----------

TEST(Controller, CarPausesAndResumes) {
    Rig r;
    r.start_charging();
    r.run(60.0);
    const int before = r.hw.energy_wh();

    r.hw.car_wants_charge(false);
    r.ctl.tick();
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::SuspendedEV);
    EXPECT_FALSE(r.hw.contactor_closed());  // contactor opens while paused
    EXPECT_TRUE(r.ctl.session_active());    // the transaction is still open

    r.hw.tick(600.0);
    EXPECT_EQ(r.hw.energy_wh(), before);    // nothing flowed

    r.hw.car_wants_charge(true);
    r.ctl.tick();
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Charging);
    EXPECT_TRUE(r.hw.contactor_closed());
    EXPECT_EQ(r.started_count, 1);          // still the same session
}

TEST(Controller, StopWhilePaused) {
    Rig r;
    r.start_charging();
    r.hw.car_wants_charge(false);
    r.ctl.tick();
    EXPECT_TRUE(r.ctl.stop(StopReason::Local));
    EXPECT_EQ(r.ended_count, 1);
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Finishing);
}

// ---------- faults ----------

TEST(Controller, OverTemperatureAbortsTheSession) {
    Rig r;
    r.start_charging();
    r.hw.set_ambient_c(80.0);  // a heat wave
    r.run(600.0);

    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Faulted);
    EXPECT_TRUE(r.ctl.error_code() == ChargePointErrorCode::HighTemperature);
    EXPECT_FALSE(r.hw.contactor_closed());
    EXPECT_EQ(r.ended_count, 1);
    EXPECT_TRUE(r.end_reason == StopReason::Other);
    EXPECT_FALSE(r.ctl.session_active());
}

TEST(Controller, PilotErrorFaultsEvenWhenIdle) {
    Rig r;
    r.hw.inject_pilot_error(true);
    r.ctl.tick();
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Faulted);
    EXPECT_TRUE(r.ctl.error_code() == ChargePointErrorCode::OtherError);
}

TEST(Controller, FaultCallbackCarriesTheErrorCode) {
    Rig r;
    r.hw.inject_pilot_error(true);
    r.ctl.tick();
    EXPECT_TRUE(r.last_error == ChargePointErrorCode::OtherError);
}

TEST(Controller, ClearFaultIsRefusedWhileTheCausePersists) {
    Rig r;
    r.hw.inject_pilot_error(true);
    r.ctl.tick();
    EXPECT_FALSE(r.ctl.clear_fault());
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Faulted);

    r.hw.inject_pilot_error(false);
    EXPECT_TRUE(r.ctl.clear_fault());
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Available);
}

TEST(Controller, ClearFaultIsRefusedWhileStillHot) {
    Rig r;
    r.hw.set_ambient_c(80.0);
    r.run(600.0);
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Faulted);
    EXPECT_FALSE(r.ctl.clear_fault());  // still around 80 degrees

    r.hw.set_ambient_c(25.0);
    r.hw.tick(600.0);
    EXPECT_TRUE(r.ctl.clear_fault());
}

TEST(Controller, AuthorizationDoesNotSurviveAFault) {
    Rig r;
    r.start_charging();
    r.hw.inject_pilot_error(true);
    r.ctl.tick();
    r.hw.inject_pilot_error(false);
    EXPECT_TRUE(r.ctl.clear_fault());
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Preparing);  // cable still plugged

    for (int i = 0; i < 5; ++i) r.ctl.tick();
    EXPECT_FALSE(r.hw.contactor_closed());  // must be authorized again

    EXPECT_TRUE(r.ctl.authorize());
    r.ctl.tick();
    r.ctl.tick();
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Charging);
}

// ---------- the safety invariant ----------

TEST(Controller, ContactorInvariantRepairsAForcedClose) {
    Rig r;
    r.hw.set_contactor(true);  // something closed it while we are idle
    r.ctl.tick();
    EXPECT_FALSE(r.hw.contactor_closed());
}

TEST(Controller, ContactorIsClosedOnlyWhileCharging) {
    Rig r;
    r.hw.plug_car();
    r.ctl.tick();
    r.ctl.authorize();

    for (int step = 0; step < 6; ++step) {
        r.ctl.tick();
        if (r.hw.contactor_closed()) {
            EXPECT_EQ(r.ctl.status(), ChargePointStatus::Charging);
        }
    }
    r.ctl.stop(StopReason::Local);
    EXPECT_FALSE(r.hw.contactor_closed());
}

TEST(Controller, SessionStartIsReportedBeforeTheChargingStatus) {
    SimHardware hw;
    ChargerController ctl{ hw };
    std::vector<std::string> events;

    ctl.set_on_status_change(
        [&events](ChargePointStatus, ChargePointStatus to, ChargePointErrorCode) {
            if (to == ChargePointStatus::Charging) events.push_back("status:Charging");
            if (to == ChargePointStatus::Finishing) events.push_back("status:Finishing");
        });
    ctl.set_on_session_started([&events](int) { events.push_back("started"); });
    ctl.set_on_session_ended([&events](int, StopReason) { events.push_back("ended"); });

    hw.plug_car();
    ctl.tick();
    ctl.authorize();
    ctl.tick();
    ctl.tick();
    ctl.stop(StopReason::Local);

    std::vector<std::string> expected = { "started", "status:Charging", "ended",
                                         "status:Finishing" };
    EXPECT_TRUE(events == expected);
}