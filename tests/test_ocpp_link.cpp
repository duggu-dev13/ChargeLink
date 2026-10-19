#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "chargelink/sim/ocpp_link.hpp"

using namespace chargelink::sim;
using namespace chargelink::ocpp;

namespace {

    // Records every CALL and lets the test deliver the outcome by hand.
    struct FakeCaller : ICaller {
        struct Sent {
            std::string action;
            json payload;
            CallTracker::Callback done;
        };
        std::vector<Sent> sent;

        void call(std::string action, json payload, CallTracker::Callback done) override {
            sent.push_back({ std::move(action), std::move(payload), std::move(done) });
        }
    };

    CallOutcome result(const std::string& json_text) {
        CallOutcome o;
        o.status = CallOutcome::Status::Result;
        o.payload = json::parse(json_text);
        return o;
    }

    CallOutcome failure(CallOutcome::Status status) {
        CallOutcome o;
        o.status = status;
        if (status == CallOutcome::Status::Error) o.error_code = "InternalError";
        return o;
    }

}  // namespace

// ---------- best-effort messages ----------

TEST(OcppLink, StatusIsSentAsStatusNotification) {
    FakeCaller caller;
    OcppLink link(caller);
    StatusNotificationRequest request;
    request.connector_id = 1;
    request.status = ChargePointStatus::Charging;
    link.send_status(request);

    ASSERT_EQ(caller.sent.size(), 1u);  // not queued: sent at once, even while offline
    EXPECT_EQ(caller.sent[0].action, "StatusNotification");
    EXPECT_EQ(caller.sent[0].payload["status"], "Charging");
    EXPECT_EQ(caller.sent[0].payload["connectorId"], 1);
}

TEST(OcppLink, AuthorizeSendsTheTagAndMapsAccepted) {
    FakeCaller caller;
    OcppLink link(caller);
    std::optional<AuthorizationStatus> got;
    bool called = false;
    link.authorize("TAG1", [&](std::optional<AuthorizationStatus> r) {
        got = r;
        called = true;
        });

    ASSERT_EQ(caller.sent.size(), 1u);
    EXPECT_EQ(caller.sent[0].action, "Authorize");
    EXPECT_EQ(caller.sent[0].payload["idTag"], "TAG1");

    caller.sent[0].done(result(R"({"idTagInfo":{"status":"Accepted"}})"));
    ASSERT_TRUE(called);
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(*got == AuthorizationStatus::Accepted);
}

TEST(OcppLink, AuthorizeMapsBlocked) {
    FakeCaller caller;
    OcppLink link(caller);
    std::optional<AuthorizationStatus> got;
    link.authorize("BAD1", [&](std::optional<AuthorizationStatus> r) { got = r; });
    caller.sent[0].done(result(R"({"idTagInfo":{"status":"Blocked"}})"));
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(*got == AuthorizationStatus::Blocked);
}

TEST(OcppLink, FailedAuthorizeGivesNullopt) {
    for (auto status : { CallOutcome::Status::Error, CallOutcome::Status::Timeout,
                        CallOutcome::Status::Disconnected }) {
        FakeCaller caller;
        OcppLink link(caller);
        bool called = false;
        std::optional<AuthorizationStatus> got = AuthorizationStatus::Accepted;
        link.authorize("TAG1", [&](std::optional<AuthorizationStatus> r) {
            got = r;
            called = true;
            });
        caller.sent[0].done(failure(status));
        EXPECT_TRUE(called);
        EXPECT_FALSE(got.has_value());
    }
}

TEST(OcppLink, UnreadableAuthorizeReplyGivesNullopt) {
    FakeCaller caller;
    OcppLink link(caller);
    bool called = false;
    std::optional<AuthorizationStatus> got = AuthorizationStatus::Accepted;
    link.authorize("TAG1", [&](std::optional<AuthorizationStatus> r) {
        got = r;
        called = true;
        });
    caller.sent[0].done(result(R"({"unexpected":true})"));  // no idTagInfo
    EXPECT_TRUE(called);
    EXPECT_FALSE(got.has_value());
}

TEST(OcppLink, MeterValuesAreSentAsMeterValues) {
    FakeCaller caller;
    OcppLink link(caller);
    MeterValuesRequest request;
    request.connector_id = 1;
    request.transaction_id = 42;
    MeterValue value;
    value.timestamp = "2026-10-17T10:30:00Z";
    SampledValue sample;
    sample.value = "1840";
    sample.unit = "Wh";
    value.sampled_value.push_back(sample);
    request.meter_value.push_back(value);
    link.send_meter_values(request);

    ASSERT_EQ(caller.sent.size(), 1u);
    EXPECT_EQ(caller.sent[0].action, "MeterValues");
    EXPECT_EQ(caller.sent[0].payload["transactionId"], 42);
    EXPECT_EQ(caller.sent[0].payload["meterValue"][0]["sampledValue"][0]["value"], "1840");
}

// ---------- transaction messages go through the queue ----------

TEST(OcppLink, StartTransactionWaitsUntilOnline) {
    FakeCaller caller;
    OcppLink link(caller);
    bool called = false;
    link.start_transaction(StartTransactionRequest{},
        [&](std::optional<StartTransactionResponse>) { called = true; });
    EXPECT_TRUE(caller.sent.empty());  // offline: queued
    EXPECT_EQ(link.queued(), 1u);
    EXPECT_FALSE(called);

    link.set_online(true);
    ASSERT_EQ(caller.sent.size(), 1u);
    EXPECT_EQ(caller.sent[0].action, "StartTransaction");
}

TEST(OcppLink, StartTransactionSendsTheFieldsAndReturnsTheId) {
    FakeCaller caller;
    OcppLink link(caller);
    link.set_online(true);

    StartTransactionRequest request;
    request.connector_id = 1;
    request.id_tag = "TAG1";
    request.meter_start = 120;
    request.timestamp = "2026-10-17T10:00:00Z";

    std::optional<StartTransactionResponse> got;
    link.start_transaction(request, [&](std::optional<StartTransactionResponse> r) { got = r; });

    ASSERT_EQ(caller.sent.size(), 1u);
    EXPECT_EQ(caller.sent[0].action, "StartTransaction");
    EXPECT_EQ(caller.sent[0].payload["idTag"], "TAG1");
    EXPECT_EQ(caller.sent[0].payload["meterStart"], 120);
    EXPECT_EQ(caller.sent[0].payload["connectorId"], 1);

    caller.sent[0].done(result(R"({"idTagInfo":{"status":"Accepted"},"transactionId":42})"));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->transaction_id, 42);
    EXPECT_TRUE(got->id_tag_info.status == AuthorizationStatus::Accepted);
    EXPECT_EQ(link.queued(), 0u);
}

TEST(OcppLink, StartTransactionSurvivesADisconnect) {
    FakeCaller caller;
    OcppLink link(caller);
    link.set_online(true);

    std::optional<StartTransactionResponse> got;
    bool called = false;
    link.start_transaction(StartTransactionRequest{}, [&](std::optional<StartTransactionResponse> r) {
        got = r;
        called = true;
        });
    caller.sent[0].done(failure(CallOutcome::Status::Disconnected));
    EXPECT_FALSE(called);  // not a failure: it is still queued
    EXPECT_EQ(link.queued(), 1u);

    link.set_online(true);              // reconnected
    ASSERT_EQ(caller.sent.size(), 2u);  // the same message goes out again
    caller.sent[1].done(result(R"({"idTagInfo":{"status":"Accepted"},"transactionId":7})"));
    ASSERT_TRUE(called);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->transaction_id, 7);
}

TEST(OcppLink, StartTransactionGivesUpAfterRepeatedErrors) {
    FakeCaller caller;
    OcppLink link(caller);
    link.set_online(true);

    bool called = false;
    std::optional<StartTransactionResponse> got = StartTransactionResponse{};
    link.start_transaction(StartTransactionRequest{}, [&](std::optional<StartTransactionResponse> r) {
        got = r;
        called = true;
        });
    for (size_t i = 0; i < 5; ++i) {  // the default is 5 attempts
        ASSERT_TRUE(i < caller.sent.size());
        caller.sent[i].done(failure(CallOutcome::Status::Error));
    }
    EXPECT_TRUE(called);
    EXPECT_FALSE(got.has_value());
    EXPECT_EQ(link.queued(), 0u);
}

TEST(OcppLink, AFullQueueRefusesStartTransactionImmediately) {
    FakeCaller caller;
    OcppLink link(caller);  // offline: everything piles up (capacity 1000)
    for (int i = 0; i < 1000; ++i) {
        link.start_transaction(StartTransactionRequest{}, [](std::optional<StartTransactionResponse>) {});
    }
    EXPECT_EQ(link.queued(), 1000u);

    bool called = false;
    std::optional<StartTransactionResponse> got = StartTransactionResponse{};
    link.start_transaction(StartTransactionRequest{}, [&](std::optional<StartTransactionResponse> r) {
        got = r;
        called = true;
        });
    EXPECT_TRUE(called);  // told at once, not left hanging
    EXPECT_FALSE(got.has_value());
    EXPECT_EQ(link.queued(), 1000u);
}

TEST(OcppLink, StopTransactionIsQueuedWhileOfflineAndSentWhenBack) {
    FakeCaller caller;
    OcppLink link(caller);

    StopTransactionRequest request;
    request.transaction_id = 42;
    request.meter_stop = 3700;
    request.timestamp = "2026-10-17T11:00:00Z";
    request.reason = StopReason::Local;
    link.stop_transaction(request);

    EXPECT_TRUE(caller.sent.empty());  // the whole point of this step
    EXPECT_EQ(link.queued(), 1u);

    link.set_online(true);
    ASSERT_EQ(caller.sent.size(), 1u);
    EXPECT_EQ(caller.sent[0].action, "StopTransaction");
    EXPECT_EQ(caller.sent[0].payload["transactionId"], 42);
    EXPECT_EQ(caller.sent[0].payload["meterStop"], 3700);
    EXPECT_EQ(caller.sent[0].payload["reason"], "Local");

    caller.sent[0].done(result("{}"));
    EXPECT_EQ(link.queued(), 0u);
}

TEST(OcppLink, StopTransactionKeepsItsPlaceBehindAnEarlierStart) {
    FakeCaller caller;
    OcppLink link(caller);
    link.start_transaction(StartTransactionRequest{}, [](std::optional<StartTransactionResponse>) {});
    link.stop_transaction(StopTransactionRequest{});
    link.set_online(true);

    ASSERT_EQ(caller.sent.size(), 1u);  // only the start: the stop waits behind it
    EXPECT_EQ(caller.sent[0].action, "StartTransaction");
    caller.sent[0].done(result(R"({"idTagInfo":{"status":"Accepted"},"transactionId":1})"));
    ASSERT_EQ(caller.sent.size(), 2u);
    EXPECT_EQ(caller.sent[1].action, "StopTransaction");
}

TEST(OcppLink, FireAndForgetFailuresDoNotCrash) {
    FakeCaller caller;
    OcppLink link(caller);
    link.set_online(true);
    link.send_status(StatusNotificationRequest{});
    link.stop_transaction(StopTransactionRequest{});
    link.send_meter_values(MeterValuesRequest{});
    ASSERT_EQ(caller.sent.size(), 3u);
    for (auto& sent : caller.sent) {
        sent.done(failure(CallOutcome::Status::Disconnected));  // just logs a warning
    }
    EXPECT_EQ(caller.sent.size(), 3u);
}