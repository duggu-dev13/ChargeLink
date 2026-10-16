#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "chargelink/ocpp/frame.hpp"

using namespace chargelink::ocpp;

TEST(Frame, ParsesCall) {
    Frame f = parse(R"([2,"abc","Heartbeat",{}])");
    ASSERT_TRUE(std::holds_alternative<Call>(f));
    const Call& c = std::get<Call>(f);
    EXPECT_EQ(c.unique_id, "abc");
    EXPECT_EQ(c.action, "Heartbeat");
    EXPECT_TRUE(c.payload.is_object());
}

TEST(Frame, ParsesCallResult) {
    Frame f = parse(R"([3,"abc",{"status":"Accepted"}])");
    ASSERT_TRUE(std::holds_alternative<CallResult>(f));
    const CallResult& r = std::get<CallResult>(f);
    EXPECT_EQ(r.unique_id, "abc");
    EXPECT_EQ(r.payload["status"], "Accepted");
}

TEST(Frame, ParsesCallError) {
    Frame f = parse(R"([4,"abc","NotImplemented","Unknown action",{}])");
    ASSERT_TRUE(std::holds_alternative<CallError>(f));
    const CallError& e = std::get<CallError>(f);
    EXPECT_EQ(e.error_code, "NotImplemented");
    EXPECT_EQ(e.error_description, "Unknown action");
}

TEST(Frame, CallRoundTrip) {
    Call original{ "id1", "BootNotification", json{ { "chargePointVendor", "SCharge" } } };
    Frame f = parse(serialize(original));
    ASSERT_TRUE(std::holds_alternative<Call>(f));
    const Call& back = std::get<Call>(f);
    EXPECT_EQ(back.unique_id, original.unique_id);
    EXPECT_EQ(back.action, original.action);
    EXPECT_EQ(back.payload, original.payload);
}

TEST(Frame, RejectsMalformedInput) {
    const std::vector<std::string> bad = {
        "",                                  // empty
        "not json",                          // not JSON
        R"({"a":1})",                        // object, not array
        R"([2,"abc"])",                      // too short
        R"(["2","abc","Heartbeat",{}])",     // type is a string
        R"([2,123,"Heartbeat",{}])",         // id is a number
        R"([2,"abc","Heartbeat"])",          // CALL missing payload
        R"([2,"abc","Heartbeat",[]])",       // payload not an object
        R"([9,"abc","Heartbeat",{}])",       // unknown message type
        R"([3,"abc",{},{}])",                // CALLRESULT too long
    };
    for (const auto& text : bad) {
        EXPECT_THROW(parse(text), ParseError) << "input: " << text;
    }
}