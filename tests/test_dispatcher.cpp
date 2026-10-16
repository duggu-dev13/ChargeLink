#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "chargelink/ocpp/dispatcher.hpp"

using namespace chargelink::ocpp;

namespace {

    Dispatcher make_heartbeat_dispatcher() {
        Dispatcher d;
        d.on<HeartbeatRequest, HeartbeatResponse>(
            [](const std::string&, const HeartbeatRequest&) {
                HeartbeatResponse r;
                r.current_time = "2026-10-16T12:00:00Z";
                return r;
            });
        return d;
    }

}  // namespace

TEST(Dispatcher, RoutesHeartbeatToHandler) {
    Dispatcher d = make_heartbeat_dispatcher();
    auto reply = d.handle("CP001", R"([2,"id1","Heartbeat",{}])");
    ASSERT_TRUE(reply.has_value());

    Frame f = parse(*reply);
    ASSERT_TRUE(std::holds_alternative<CallResult>(f));
    const CallResult& r = std::get<CallResult>(f);
    EXPECT_EQ(r.unique_id, "id1");  // reply is matched to the request
    EXPECT_EQ(r.payload["currentTime"], "2026-10-16T12:00:00Z");
}

TEST(Dispatcher, UnknownActionGivesNotImplemented) {
    Dispatcher d = make_heartbeat_dispatcher();
    auto reply = d.handle("CP001", R"([2,"id2","MakeCoffee",{}])");
    ASSERT_TRUE(reply.has_value());

    Frame f = parse(*reply);
    ASSERT_TRUE(std::holds_alternative<CallError>(f));
    const CallError& e = std::get<CallError>(f);
    EXPECT_EQ(e.unique_id, "id2");
    EXPECT_EQ(e.error_code, "NotImplemented");
}

TEST(Dispatcher, BadPayloadGivesFormationViolation) {
    Dispatcher d;
    d.on<BootNotificationRequest, BootNotificationResponse>(
        [](const std::string&, const BootNotificationRequest&) {
            return BootNotificationResponse{};
        });
    // chargePointVendor is missing
    auto reply = d.handle("CP001", R"([2,"id3","BootNotification",{"chargePointModel":"A1"}])");
    ASSERT_TRUE(reply.has_value());

    Frame f = parse(*reply);
    ASSERT_TRUE(std::holds_alternative<CallError>(f));
    EXPECT_EQ(std::get<CallError>(f).error_code, "FormationViolation");
}

TEST(Dispatcher, HandlerExceptionGivesInternalErrorWithoutLeaking) {
    Dispatcher d;
    d.on<HeartbeatRequest, HeartbeatResponse>(
        [](const std::string&, const HeartbeatRequest&) -> HeartbeatResponse {
            throw std::runtime_error("database password is hunter2");
        });
    auto reply = d.handle("CP001", R"([2,"id4","Heartbeat",{}])");
    ASSERT_TRUE(reply.has_value());

    Frame f = parse(*reply);
    ASSERT_TRUE(std::holds_alternative<CallError>(f));
    const CallError& e = std::get<CallError>(f);
    EXPECT_EQ(e.error_code, "InternalError");
    EXPECT_EQ(e.error_description.find("hunter2"), std::string::npos);  // no leak
}

TEST(Dispatcher, HandlerReceivesChargePointId) {
    Dispatcher d;
    std::string seen;
    d.on<HeartbeatRequest, HeartbeatResponse>(
        [&seen](const std::string& cp_id, const HeartbeatRequest&) {
            seen = cp_id;
            return HeartbeatResponse{ "2026-10-16T12:00:00Z" };
        });
    d.handle("CP042", R"([2,"id5","Heartbeat",{}])");
    EXPECT_EQ(seen, "CP042");
}

TEST(Dispatcher, GarbageAndResponsesGetNoReply) {
    Dispatcher d = make_heartbeat_dispatcher();
    EXPECT_FALSE(d.handle("CP001", "not json at all").has_value());
    EXPECT_FALSE(d.handle("CP001", R"([3,"id6",{}])").has_value());  // a CALLRESULT
}