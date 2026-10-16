#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "chargelink/sim/connector_fsm.hpp"

using namespace chargelink::sim;

namespace {

    // Drive a fresh connector into Charging.
    ConnectorFsm charging_fsm() {
        ConnectorFsm fsm;
        fsm.handle(Event::CablePlugged);
        fsm.handle(Event::Authorized);
        fsm.handle(Event::ChargingStarted);
        return fsm;
    }

}  // namespace

// ---------- main flow (should pass as soon as you build) ----------

TEST(ConnectorFsm, StartsAvailable) {
    ConnectorFsm fsm;
    EXPECT_EQ(fsm.status(), ChargePointStatus::Available);
    EXPECT_EQ(fsm.error_code(), ChargePointErrorCode::NoError);
    EXPECT_FALSE(fsm.cable_plugged());
    EXPECT_FALSE(fsm.authorized());
}

TEST(ConnectorFsm, FullSessionFlow) {
    ConnectorFsm fsm;
    EXPECT_TRUE(fsm.handle(Event::CablePlugged));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Preparing);
    EXPECT_TRUE(fsm.handle(Event::Authorized));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Preparing);
    EXPECT_TRUE(fsm.handle(Event::ChargingStarted));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Charging);
    EXPECT_TRUE(fsm.handle(Event::ChargingStopped));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Finishing);
    EXPECT_TRUE(fsm.handle(Event::CableUnplugged));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Available);
}

TEST(ConnectorFsm, AuthorizeBeforePlugging) {
    ConnectorFsm fsm;
    EXPECT_TRUE(fsm.handle(Event::Authorized));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Preparing);
    EXPECT_TRUE(fsm.handle(Event::CablePlugged));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Preparing);
    EXPECT_TRUE(fsm.handle(Event::ChargingStarted));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Charging);
}

TEST(ConnectorFsm, CannotStartWithoutAuthorization) {
    ConnectorFsm fsm;
    fsm.handle(Event::CablePlugged);
    EXPECT_FALSE(fsm.handle(Event::ChargingStarted));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Preparing);
}

TEST(ConnectorFsm, CannotStartWithoutCable) {
    ConnectorFsm fsm;
    fsm.handle(Event::Authorized);
    EXPECT_FALSE(fsm.handle(Event::ChargingStarted));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Preparing);
}

TEST(ConnectorFsm, EvPauseAndResume) {
    ConnectorFsm fsm = charging_fsm();
    EXPECT_TRUE(fsm.handle(Event::EvPaused));
    EXPECT_EQ(fsm.status(), ChargePointStatus::SuspendedEV);
    EXPECT_TRUE(fsm.handle(Event::EvResumed));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Charging);
}

TEST(ConnectorFsm, EvsePauseAndResume) {
    ConnectorFsm fsm = charging_fsm();
    EXPECT_TRUE(fsm.handle(Event::EvsePaused));
    EXPECT_EQ(fsm.status(), ChargePointStatus::SuspendedEVSE);
    EXPECT_FALSE(fsm.handle(Event::EvResumed));  // wrong resume for this state
    EXPECT_TRUE(fsm.handle(Event::EvseResumed));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Charging);
}

TEST(ConnectorFsm, StopFromSuspendedStates) {
    ConnectorFsm fsm = charging_fsm();
    fsm.handle(Event::EvPaused);
    EXPECT_TRUE(fsm.handle(Event::ChargingStopped));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Finishing);
}

TEST(ConnectorFsm, IllegalEventIsRejectedAndStateKept) {
    ConnectorFsm fsm;
    EXPECT_FALSE(fsm.handle(Event::ChargingStarted));
    EXPECT_FALSE(fsm.handle(Event::EvPaused));
    EXPECT_FALSE(fsm.handle(Event::CableUnplugged));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Available);
}

TEST(ConnectorFsm, UnplugBeforeChargingClearsAuthorization) {
    ConnectorFsm fsm;
    fsm.handle(Event::CablePlugged);
    fsm.handle(Event::Authorized);
    EXPECT_TRUE(fsm.handle(Event::CableUnplugged));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Available);
    EXPECT_FALSE(fsm.authorized());

    fsm.handle(Event::CablePlugged);
    EXPECT_FALSE(fsm.handle(Event::ChargingStarted));  // old authorization is gone
}

TEST(ConnectorFsm, UnplugDuringChargingGoesAvailable) {
    ConnectorFsm fsm = charging_fsm();
    EXPECT_TRUE(fsm.handle(Event::CableUnplugged));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Available);
    EXPECT_FALSE(fsm.cable_plugged());
    EXPECT_FALSE(fsm.authorized());
}

TEST(ConnectorFsm, CallbackFiresOnEveryChangeOnly) {
    ConnectorFsm fsm;
    std::vector<std::pair<ChargePointStatus, ChargePointStatus>> seen;
    fsm.set_on_change([&seen](ChargePointStatus from, ChargePointStatus to, ChargePointErrorCode) {
        seen.emplace_back(from, to);
        });

    fsm.handle(Event::CablePlugged);     // Available -> Preparing
    fsm.handle(Event::Authorized);       // stays Preparing: no callback
    fsm.handle(Event::ChargingStarted);  // Preparing -> Charging
    fsm.handle(Event::EvResumed);        // rejected: no callback
    fsm.handle(Event::ChargingStopped);  // Charging -> Finishing

    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], std::make_pair(ChargePointStatus::Available, ChargePointStatus::Preparing));
    EXPECT_EQ(seen[1], std::make_pair(ChargePointStatus::Preparing, ChargePointStatus::Charging));
    EXPECT_EQ(seen[2], std::make_pair(ChargePointStatus::Charging, ChargePointStatus::Finishing));
}

// ---------- YOUR EXERCISE: these fail until you implement it ----------

TEST(ConnectorFsm, FaultAbortsASession) {
    ConnectorFsm fsm = charging_fsm();
    EXPECT_TRUE(fsm.raise_fault(ChargePointErrorCode::GroundFailure));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Faulted);
    EXPECT_EQ(fsm.error_code(), ChargePointErrorCode::GroundFailure);
    EXPECT_FALSE(fsm.authorized());
}

TEST(ConnectorFsm, FaultCallbackCarriesErrorCode) {
    ConnectorFsm fsm;
    ChargePointErrorCode seen = ChargePointErrorCode::NoError;
    fsm.set_on_change([&seen](ChargePointStatus, ChargePointStatus, ChargePointErrorCode e) {
        seen = e;
        });
    fsm.raise_fault(ChargePointErrorCode::HighTemperature);
    EXPECT_EQ(seen, ChargePointErrorCode::HighTemperature);
}

TEST(ConnectorFsm, NoErrorIsNotAFault) {
    ConnectorFsm fsm;
    EXPECT_FALSE(fsm.raise_fault(ChargePointErrorCode::NoError));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Available);
}

TEST(ConnectorFsm, FaultClearedWhenUnpluggedGoesAvailable) {
    ConnectorFsm fsm;
    fsm.raise_fault(ChargePointErrorCode::OverVoltage);
    EXPECT_TRUE(fsm.handle(Event::FaultCleared));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Available);
    EXPECT_EQ(fsm.error_code(), ChargePointErrorCode::NoError);
}

TEST(ConnectorFsm, FaultClearedWhenPluggedGoesPreparing) {
    ConnectorFsm fsm;
    fsm.handle(Event::CablePlugged);
    fsm.raise_fault(ChargePointErrorCode::OverVoltage);
    EXPECT_TRUE(fsm.handle(Event::FaultCleared));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Preparing);
}

TEST(ConnectorFsm, FaultedRejectsEverythingButCableAndClear) {
    ConnectorFsm fsm;
    fsm.raise_fault(ChargePointErrorCode::InternalError);
    EXPECT_FALSE(fsm.handle(Event::Authorized));
    EXPECT_FALSE(fsm.handle(Event::ChargingStarted));
    EXPECT_FALSE(fsm.handle(Event::MadeUnavailable));
    EXPECT_TRUE(fsm.handle(Event::CablePlugged));  // still tracked
    EXPECT_TRUE(fsm.cable_plugged());
    EXPECT_EQ(fsm.status(), ChargePointStatus::Faulted);
}

TEST(ConnectorFsm, UnavailableOnlyFromAvailable) {
    ConnectorFsm fsm;
    EXPECT_TRUE(fsm.handle(Event::MadeUnavailable));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Unavailable);
    EXPECT_TRUE(fsm.handle(Event::MadeAvailable));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Available);

    fsm.handle(Event::CablePlugged);  // now Preparing
    EXPECT_FALSE(fsm.handle(Event::MadeUnavailable));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Preparing);
}

TEST(ConnectorFsm, UnavailableRemembersTheCable) {
    ConnectorFsm fsm;
    fsm.handle(Event::MadeUnavailable);
    EXPECT_TRUE(fsm.handle(Event::CablePlugged));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Unavailable);
    EXPECT_FALSE(fsm.handle(Event::Authorized));
    EXPECT_TRUE(fsm.handle(Event::MadeAvailable));
    EXPECT_EQ(fsm.status(), ChargePointStatus::Preparing);  // cable was inserted meanwhile
}