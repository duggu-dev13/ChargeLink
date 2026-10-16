#include <gtest/gtest.h>

#include "chargelink/ocpp/messages.hpp"

using namespace chargelink::ocpp;

TEST(Messages, BootNotificationRequestRoundTrip) {
    BootNotificationRequest req;
    req.charge_point_vendor = "SCharge";
    req.charge_point_model = "A1-7.4kW";
    req.firmware_version = "0.1.0";

    json j = req;
    EXPECT_EQ(j["chargePointVendor"], "SCharge");
    EXPECT_FALSE(j.contains("chargePointSerialNumber"));  // optional, left out

    auto back = j.get<BootNotificationRequest>();
    EXPECT_EQ(back.charge_point_vendor, "SCharge");
    EXPECT_EQ(back.charge_point_model, "A1-7.4kW");
    EXPECT_FALSE(back.charge_point_serial_number.has_value());
    EXPECT_EQ(back.firmware_version.value(), "0.1.0");
}

TEST(Messages, BootNotificationMissingVendorRejected) {
    json j = json::parse(R"({"chargePointModel":"A1"})");
    EXPECT_THROW(j.get<BootNotificationRequest>(), ParseError);
}

TEST(Messages, BootNotificationWrongTypeRejected) {
    json j = json::parse(R"({"chargePointVendor":123,"chargePointModel":"A1"})");
    EXPECT_THROW(j.get<BootNotificationRequest>(), ParseError);
}

TEST(Messages, BootNotificationResponseRoundTrip) {
    BootNotificationResponse res;
    res.status = RegistrationStatus::Accepted;
    res.current_time = "2026-10-16T12:40:06Z";
    res.interval = 300;

    json j = res;
    EXPECT_EQ(j["status"], "Accepted");
    EXPECT_EQ(j["interval"], 300);

    auto back = j.get<BootNotificationResponse>();
    EXPECT_EQ(back.status, RegistrationStatus::Accepted);
    EXPECT_EQ(back.interval, 300);
}

TEST(Messages, UnknownRegistrationStatusRejected) {
    json j = json::parse(R"({"status":"Maybe","currentTime":"x","interval":5})");
    EXPECT_THROW(j.get<BootNotificationResponse>(), ParseError);
}

TEST(Messages, MakeCallBuildsHeartbeatFrame) {
    Call c = make_call("id7", HeartbeatRequest{});
    EXPECT_EQ(c.action, "Heartbeat");
    EXPECT_EQ(serialize(c), R"([2,"id7","Heartbeat",{}])");
}

TEST(Messages, PayloadAsWrapsErrors) {
    json bad = json::parse(R"({"status":"Accepted","currentTime":5,"interval":300})");
    EXPECT_THROW(payload_as<BootNotificationResponse>(bad), ParseError);
}