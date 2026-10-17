#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "chargelink/ocpp/call_tracker.hpp"

using namespace chargelink::ocpp;
using Clock = CallTracker::Clock;
using ms = std::chrono::milliseconds;

namespace {

    const Clock::time_point t0{};  // an arbitrary starting point; tests move time by hand

    Frame result_for(const std::string& id) {
        return CallResult{ id, json::object() };
    }

    Frame error_for(const std::string& id) {
        return CallError{ id, "NotImplemented", "no such action", json::object() };
    }

}  // namespace

TEST(CallTracker, IdsAreUniqueAndShortEnough) {
    CallTracker t;
    std::string a = t.add(t0, ms(1000), nullptr);
    std::string b = t.add(t0, ms(1000), nullptr);
    EXPECT_TRUE(a != b);
    EXPECT_TRUE(a.size() <= 36);  // OCPP limit for unique ids
    EXPECT_EQ(t.pending(), 2u);
}

TEST(CallTracker, ResultReachesTheCallbackExactlyOnce) {
    CallTracker t;
    int calls = 0;
    CallOutcome seen;
    std::string id = t.add(t0, ms(1000), [&](const CallOutcome& o) {
        ++calls;
        seen = o;
        });

    EXPECT_TRUE(t.resolve(result_for(id)));
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(seen.status == CallOutcome::Status::Result);
    EXPECT_TRUE(seen.payload.is_object());
    EXPECT_EQ(t.pending(), 0u);

    EXPECT_FALSE(t.resolve(result_for(id)));  // a duplicate reply is ignored
    EXPECT_EQ(calls, 1);
}

TEST(CallTracker, ErrorCarriesCodeAndDescription) {
    CallTracker t;
    CallOutcome seen;
    std::string id = t.add(t0, ms(1000), [&](const CallOutcome& o) { seen = o; });

    EXPECT_TRUE(t.resolve(error_for(id)));
    EXPECT_TRUE(seen.status == CallOutcome::Status::Error);
    EXPECT_EQ(seen.error_code, "NotImplemented");
    EXPECT_EQ(seen.error_description, "no such action");
}

TEST(CallTracker, UnknownIdIsNotResolved) {
    CallTracker t;
    t.add(t0, ms(1000), nullptr);
    EXPECT_FALSE(t.resolve(result_for("999")));
    EXPECT_EQ(t.pending(), 1u);
}

TEST(CallTracker, ACallFrameIsNotAReply) {
    CallTracker t;
    std::string id = t.add(t0, ms(1000), nullptr);
    Frame request = Call{ id, "Reset", json::object() };  // same id, but it is a request
    EXPECT_FALSE(t.resolve(request));
    EXPECT_EQ(t.pending(), 1u);
}

TEST(CallTracker, ExpireOnlyFailsOverdueCalls) {
    CallTracker t;
    std::vector<std::string> log;
    t.add(t0, ms(1000), [&](const CallOutcome& o) { log.push_back(std::string("fast:") + to_string(o.status)); });
    t.add(t0, ms(5000), [&](const CallOutcome& o) { log.push_back(std::string("slow:") + to_string(o.status)); });

    EXPECT_EQ(t.expire(t0 + ms(999)), 0u);   // one millisecond early
    EXPECT_EQ(t.expire(t0 + ms(1000)), 1u);  // exactly at the deadline counts as expired
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0], "fast:Timeout");
    EXPECT_EQ(t.pending(), 1u);

    EXPECT_EQ(t.expire(t0 + ms(6000)), 1u);
    EXPECT_EQ(log.size(), 2u);
    EXPECT_EQ(t.pending(), 0u);
}

TEST(CallTracker, LateReplyAfterTimeoutIsIgnored) {
    CallTracker t;
    int calls = 0;
    std::string id = t.add(t0, ms(1000), [&](const CallOutcome&) { ++calls; });
    t.expire(t0 + ms(2000));
    EXPECT_EQ(calls, 1);  // the timeout

    EXPECT_FALSE(t.resolve(result_for(id)));  // the answer arrives too late
    EXPECT_EQ(calls, 1);                      // the callback does not run twice
}

TEST(CallTracker, FailAllReportsTheGivenStatus) {
    CallTracker t;
    int disconnected = 0;
    for (int i = 0; i < 3; ++i) {
        t.add(t0, ms(1000), [&](const CallOutcome& o) {
            if (o.status == CallOutcome::Status::Disconnected) ++disconnected;
            });
    }
    t.fail_all(CallOutcome::Status::Disconnected);
    EXPECT_EQ(disconnected, 3);
    EXPECT_EQ(t.pending(), 0u);
}

TEST(CallTracker, CallbackMayAddANewCall) {
    CallTracker t;
    int second_calls = 0;
    std::string second_id;
    std::string first = t.add(t0, ms(1000), [&](const CallOutcome&) {
        // for example: a retry sent from inside the failure handler
        second_id = t.add(t0, ms(1000), [&](const CallOutcome&) { ++second_calls; });
        });

    EXPECT_TRUE(t.resolve(result_for(first)));
    EXPECT_EQ(t.pending(), 1u);  // the new call survived
    EXPECT_TRUE(t.resolve(result_for(second_id)));
    EXPECT_EQ(second_calls, 1);
}

TEST(CallTracker, CallbackMayAddANewCallDuringFailAll) {
    CallTracker t;
    t.add(t0, ms(1000), [&](const CallOutcome&) { t.add(t0, ms(1000), nullptr); });
    t.fail_all(CallOutcome::Status::Disconnected);
    EXPECT_EQ(t.pending(), 1u);  // the call added during fail_all was not swallowed
}

TEST(CallTracker, NullCallbackIsAllowed) {
    CallTracker t;
    std::string id = t.add(t0, ms(1000), nullptr);
    EXPECT_TRUE(t.resolve(result_for(id)));  // fire-and-forget: no crash
    t.add(t0, ms(1000), nullptr);
    EXPECT_EQ(t.expire(t0 + ms(2000)), 1u);
}

TEST(CallTracker, StatusNames) {
    EXPECT_EQ(std::string(to_string(CallOutcome::Status::Timeout)), "Timeout");
    EXPECT_EQ(std::string(to_string(CallOutcome::Status::Disconnected)), "Disconnected");
}