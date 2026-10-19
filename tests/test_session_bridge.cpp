#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chargelink/sim/session_bridge.hpp"
#include "chargelink/sim/sim_hardware.hpp"

using namespace chargelink::sim;
using namespace chargelink::ocpp;

namespace {

    std::string name_of(ChargePointStatus s) {
        switch (s) {
        case ChargePointStatus::Available:     return "Available";
        case ChargePointStatus::Preparing:     return "Preparing";
        case ChargePointStatus::Charging:      return "Charging";
        case ChargePointStatus::SuspendedEVSE: return "SuspendedEVSE";
        case ChargePointStatus::SuspendedEV:   return "SuspendedEV";
        case ChargePointStatus::Finishing:     return "Finishing";
        case ChargePointStatus::Reserved:      return "Reserved";
        case ChargePointStatus::Unavailable:   return "Unavailable";
        case ChargePointStatus::Faulted:       return "Faulted";
        }
        return "?";
    }

    // Records everything the bridge sends and lets the test deliver replies by hand.
    struct FakeLink : IStationLink {
        struct AuthCall {
            std::string tag;
            std::function<void(std::optional<AuthorizationStatus>)> done;
        };
        struct StartCall {
            StartTransactionRequest request;
            std::function<void(std::optional<StartTransactionResponse>)> done;
        };

        std::vector<StatusNotificationRequest> statuses;
        std::vector<AuthCall> auths;
        std::vector<StartCall> starts;
        std::vector<StopTransactionRequest> stops;
        std::vector<MeterValuesRequest> meters;
        std::vector<std::string> events;  // everything, in the order it was sent

        void send_status(const StatusNotificationRequest& r) override {
            statuses.push_back(r);
            events.push_back("status:" + name_of(r.status));
        }
        void authorize(const std::string& tag,
            std::function<void(std::optional<AuthorizationStatus>)> done) override {
            auths.push_back({ tag, std::move(done) });
            events.push_back("authorize");
        }
        void start_transaction(const StartTransactionRequest& r,
            std::function<void(std::optional<StartTransactionResponse>)> done) override {
            starts.push_back({ r, std::move(done) });
            events.push_back("start");
        }
        void stop_transaction(const StopTransactionRequest& r) override {
            stops.push_back(r);
            events.push_back("stop");
        }
        void send_meter_values(const MeterValuesRequest& r) override {
            meters.push_back(r);
            events.push_back("meter");
        }
    };

    StartTransactionResponse start_reply(int id, AuthorizationStatus status = AuthorizationStatus::Accepted) {
        StartTransactionResponse r;
        r.transaction_id = id;
        r.id_tag_info.status = status;
        return r;
    }

    struct Rig {
        SimHardware hw;
        ChargerController ctl{ hw };
        FakeLink link;
        SessionBridge bridge{ ctl, link, hw, [] { return std::string("2026-10-17T10:00:00Z"); } };

        // Plug in, tap, have the CSMS accept, and poll until charging starts.
        void start_session(const std::string& tag = "TAG1") {
            hw.plug_car();
            ctl.tick();
            bridge.tap(tag);
            link.auths.back().done(AuthorizationStatus::Accepted);
            ctl.tick();
            ctl.tick();
        }
    };

}  // namespace

// ---------- registration ----------

TEST(Bridge, NothingIsSentBeforeRegistration) {
    Rig r;
    r.hw.plug_car();
    r.ctl.tick();
    EXPECT_EQ(r.ctl.status(), ChargePointStatus::Preparing);
    EXPECT_TRUE(r.link.events.empty());
}

TEST(Bridge, RegistrationSendsASnapshot) {
    Rig r;
    r.hw.plug_car();
    r.ctl.tick();
    r.bridge.set_registered(true);

    ASSERT_EQ(r.link.statuses.size(), 2u);
    EXPECT_EQ(r.link.statuses[0].connector_id, 0);  // the charge point itself
    EXPECT_TRUE(r.link.statuses[0].status == ChargePointStatus::Available);
    EXPECT_EQ(r.link.statuses[1].connector_id, 1);
    EXPECT_TRUE(r.link.statuses[1].status == ChargePointStatus::Preparing);
    EXPECT_TRUE(r.link.statuses[1].timestamp.has_value());
}

TEST(Bridge, StatusChangesAreForwardedOnceRegistered) {
    Rig r;
    r.bridge.set_registered(true);
    r.link.events.clear();
    r.hw.plug_car();
    r.ctl.tick();
    ASSERT_EQ(r.link.events.size(), 1u);
    EXPECT_EQ(r.link.events[0], "status:Preparing");
}

TEST(Bridge, FaultCodeIsForwarded) {
    Rig r;
    r.bridge.set_registered(true);
    r.link.statuses.clear();
    r.hw.inject_pilot_error(true);
    r.ctl.tick();
    ASSERT_EQ(r.link.statuses.size(), 1u);
    EXPECT_TRUE(r.link.statuses[0].status == ChargePointStatus::Faulted);
    EXPECT_TRUE(r.link.statuses[0].error_code == ChargePointErrorCode::OtherError);
}

// ---------- tapping a card ----------

TEST(Bridge, TapBeforeRegistrationIsRefused) {
    Rig r;
    EXPECT_TRUE(r.bridge.tap("TAG1") == SessionBridge::TapResult::NotRegistered);
    EXPECT_TRUE(r.link.auths.empty());
}

TEST(Bridge, AcceptedTagAuthorizesTheController) {
    Rig r;
    r.bridge.set_registered(true);
    r.hw.plug_car();
    r.ctl.tick();

    EXPECT_TRUE(r.bridge.tap("TAG1") == SessionBridge::TapResult::Asked);
    ASSERT_EQ(r.link.auths.size(), 1u);
    EXPECT_EQ(r.link.auths[0].tag, "TAG1");

    r.link.auths[0].done(AuthorizationStatus::Accepted);
    r.ctl.tick();
    r.ctl.tick();
    EXPECT_TRUE(r.ctl.status() == ChargePointStatus::Charging);
}

TEST(Bridge, RejectedOrFailedAuthorizationChangesNothing) {
    Rig r;
    r.bridge.set_registered(true);
    r.hw.plug_car();
    r.ctl.tick();

    r.bridge.tap("BAD");
    r.link.auths[0].done(AuthorizationStatus::Blocked);
    r.bridge.tap("LOST");
    r.link.auths[1].done(std::nullopt);  // timeout, error or disconnect

    for (int i = 0; i < 5; ++i) r.ctl.tick();
    EXPECT_TRUE(r.ctl.status() == ChargePointStatus::Preparing);
    EXPECT_FALSE(r.hw.contactor_closed());
}

TEST(Bridge, TapWhileFaultedIsBusy) {
    Rig r;
    r.bridge.set_registered(true);
    r.hw.inject_pilot_error(true);
    r.ctl.tick();
    EXPECT_TRUE(r.bridge.tap("TAG1") == SessionBridge::TapResult::Busy);
    EXPECT_TRUE(r.link.auths.empty());
}

TEST(Bridge, LateAuthorizeReplyAfterTheBridgeIsGoneIsHarmless) {
    SimHardware hw;
    ChargerController ctl{ hw };
    FakeLink link;
    {
        SessionBridge bridge{ ctl, link, hw, [] { return std::string("T"); } };
        bridge.set_registered(true);
        hw.plug_car();
        ctl.tick();
        bridge.tap("TAG1");
    }  // bridge destroyed with the call still pending

    ASSERT_EQ(link.auths.size(), 1u);
    link.auths[0].done(AuthorizationStatus::Accepted);  // must not crash or touch the controller
    ctl.tick();  // and the controller no longer calls into a dead bridge
    EXPECT_FALSE(ctl.session_active());
}

// ---------- a full session ----------

TEST(Bridge, StartTransactionPrecedesTheChargingStatus) {
    Rig r;
    r.bridge.set_registered(true);
    r.start_session();

    ASSERT_EQ(r.link.starts.size(), 1u);
    const auto& req = r.link.starts[0].request;
    EXPECT_EQ(req.connector_id, 1);
    EXPECT_EQ(req.id_tag, "TAG1");
    EXPECT_EQ(req.meter_start, 0);

    // the order on the wire
    std::vector<std::string> expected = { "status:Available", "status:Available", "status:Preparing",
                                         "authorize", "start", "status:Charging" };
    EXPECT_TRUE(r.link.events == expected);
}

TEST(Bridge, SecondTapOfTheSameTagStopsTheSessionAndReportsIt) {
    Rig r;
    r.bridge.set_registered(true);
    r.start_session("TAG1");
    r.link.starts[0].done(start_reply(42));

    r.hw.tick(3600.0);  // an hour at 16 A
    EXPECT_TRUE(r.bridge.tap("TAG1") == SessionBridge::TapResult::StoppedSession);

    ASSERT_EQ(r.link.stops.size(), 1u);
    const auto& stop = r.link.stops[0];
    EXPECT_EQ(stop.transaction_id, 42);
    EXPECT_NEAR(stop.meter_stop, 3680, 2);
    ASSERT_TRUE(stop.reason.has_value());
    EXPECT_TRUE(*stop.reason == StopReason::Local);
    ASSERT_TRUE(stop.id_tag.has_value());
    EXPECT_EQ(*stop.id_tag, "TAG1");

    // StopTransaction goes out before the Finishing status
    const auto& e = r.link.events;
    EXPECT_EQ(e[e.size() - 2], "stop");
    EXPECT_EQ(e[e.size() - 1], "status:Finishing");
}

TEST(Bridge, TappingAnotherTagDuringASessionIsBusy) {
    Rig r;
    r.bridge.set_registered(true);
    r.start_session("TAG1");
    EXPECT_TRUE(r.bridge.tap("OTHER") == SessionBridge::TapResult::Busy);
    EXPECT_TRUE(r.ctl.session_active());
}

TEST(Bridge, StopBeforeTheStartReplyIsDeferredUntilTheIdArrives) {
    Rig r;
    r.bridge.set_registered(true);
    r.start_session();  // the StartTransaction reply has NOT arrived

    r.hw.unplug_car();
    r.ctl.tick();  // the controller ends the session: EVDisconnected
    EXPECT_FALSE(r.ctl.session_active());
    EXPECT_TRUE(r.link.stops.empty());  // we cannot name the transaction yet

    r.link.starts[0].done(start_reply(7));
    ASSERT_EQ(r.link.stops.size(), 1u);
    EXPECT_EQ(r.link.stops[0].transaction_id, 7);
    EXPECT_TRUE(*r.link.stops[0].reason == StopReason::EVDisconnected);
}

TEST(Bridge, FailedStartMeansNoStopIsEverSent) {
    Rig r;
    r.bridge.set_registered(true);
    r.start_session();
    r.link.starts[0].done(std::nullopt);  // StartTransaction timed out

    r.bridge.tap("TAG1");  // end the session
    EXPECT_TRUE(r.link.stops.empty());
}

TEST(Bridge, FailedStartAfterTheSessionEndedDropsThePendingStop) {
    Rig r;
    r.bridge.set_registered(true);
    r.start_session();
    r.hw.unplug_car();
    r.ctl.tick();  // ended, stop deferred

    r.link.starts[0].done(std::nullopt);
    EXPECT_TRUE(r.link.stops.empty());
}

TEST(Bridge, RejectedStartStopsTheSessionWithDeAuthorized) {
    Rig r;
    r.bridge.set_registered(true);
    r.start_session();
    r.link.starts[0].done(start_reply(9, AuthorizationStatus::Blocked));

    EXPECT_FALSE(r.ctl.session_active());
    EXPECT_FALSE(r.hw.contactor_closed());
    ASSERT_EQ(r.link.stops.size(), 1u);
    EXPECT_EQ(r.link.stops[0].transaction_id, 9);
    EXPECT_TRUE(*r.link.stops[0].reason == StopReason::DeAuthorized);
}
// ---------- Step 17: transactions are handed to the (queueing) link even when offline ----------

TEST(Bridge, ASessionThatStartsJustAfterTheLinkDroppedIsStillReported) {
    Rig r;
    r.bridge.set_registered(true);
    r.hw.plug_car();
    r.ctl.tick();
    r.bridge.tap("TAG1");
    r.link.auths[0].done(AuthorizationStatus::Accepted);
    r.bridge.set_registered(false);  // the link drops before the car answers
    r.ctl.tick();
    r.ctl.tick();

    EXPECT_TRUE(r.ctl.session_active());
    ASSERT_EQ(r.link.starts.size(), 1u);  // handed over: the real link queues it
    EXPECT_EQ(r.link.starts[0].request.id_tag, "TAG1");

    r.link.starts[0].done(start_reply(5));  // delivered after reconnecting
    r.bridge.tap("TAG1");
    ASSERT_EQ(r.link.stops.size(), 1u);
    EXPECT_EQ(r.link.stops[0].transaction_id, 5);
}

TEST(Bridge, AStopDuringAnOutageIsHandedToTheLink) {
    Rig r;
    r.bridge.set_registered(true);
    r.start_session();
    r.link.starts[0].done(start_reply(42));

    r.bridge.set_registered(false);  // outage
    r.hw.unplug_car();
    r.ctl.tick();

    ASSERT_EQ(r.link.stops.size(), 1u);  // the real link queues it until we are back
    EXPECT_EQ(r.link.stops[0].transaction_id, 42);
    EXPECT_TRUE(*r.link.stops[0].reason == StopReason::EVDisconnected);
}

TEST(Bridge, TappingTheSameCardStopsTheSessionEvenOffline) {
    Rig r;
    r.bridge.set_registered(true);
    r.start_session("TAG1");
    r.link.starts[0].done(start_reply(42));

    r.bridge.set_registered(false);
    EXPECT_TRUE(r.bridge.tap("TAG1") == SessionBridge::TapResult::StoppedSession);
    EXPECT_FALSE(r.ctl.session_active());
    EXPECT_FALSE(r.hw.contactor_closed());
    EXPECT_EQ(r.link.stops.size(), 1u);
}

TEST(Bridge, StartingASessionStillNeedsTheCsms) {
    Rig r;  // never registered
    r.hw.plug_car();
    r.ctl.tick();
    EXPECT_TRUE(r.bridge.tap("TAG1") == SessionBridge::TapResult::NotRegistered);
    EXPECT_TRUE(r.link.auths.empty());
}
// ---------- meter values ----------

TEST(Bridge, MeterValuesWaitForTheTransactionId) {
    Rig r;
    r.bridge.set_registered(true);
    r.start_session();
    r.bridge.sample_meter_values();
    EXPECT_TRUE(r.link.meters.empty());  // id not known yet

    r.link.starts[0].done(start_reply(42));
    r.bridge.sample_meter_values();
    EXPECT_EQ(r.link.meters.size(), 1u);
}

TEST(Bridge, MeterValuesCarryEnergyCurrentAndTemperature) {
    Rig r;
    r.bridge.set_registered(true);
    r.start_session();
    r.link.starts[0].done(start_reply(42));
    r.hw.tick(3600.0);
    r.bridge.sample_meter_values();

    ASSERT_EQ(r.link.meters.size(), 1u);
    const auto& m = r.link.meters[0];
    EXPECT_EQ(m.connector_id, 1);
    ASSERT_TRUE(m.transaction_id.has_value());
    EXPECT_EQ(*m.transaction_id, 42);
    ASSERT_EQ(m.meter_value.size(), 1u);

    const auto& values = m.meter_value[0].sampled_value;
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(*values[0].measurand, "Energy.Active.Import.Register");
    EXPECT_EQ(*values[0].unit, "Wh");
    EXPECT_NEAR(std::stod(values[0].value), 3680.0, 2.0);
    EXPECT_EQ(*values[1].measurand, "Current.Import");
    EXPECT_EQ(values[1].value, "16.0");
    EXPECT_EQ(*values[2].measurand, "Temperature");
    EXPECT_EQ(*values[2].unit, "Celsius");
    EXPECT_EQ(*values[0].context, "Sample.Periodic");
}

TEST(Bridge, NoMeterValuesOutsideASession) {
    Rig r;
    r.bridge.set_registered(true);
    r.bridge.sample_meter_values();
    EXPECT_TRUE(r.link.meters.empty());

    r.start_session();
    r.link.starts[0].done(start_reply(42));
    r.bridge.tap("TAG1");  // stop
    r.bridge.sample_meter_values();
    EXPECT_TRUE(r.link.meters.empty());
}