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

TEST(Messages, AuthorizeResponseRoundTrip) {
    AuthorizeResponse res;
    res.id_tag_info.status = AuthorizationStatus::Accepted;
    json j = res;
    EXPECT_EQ(j["idTagInfo"]["status"], "Accepted");
    auto back = j.get<AuthorizeResponse>();
    EXPECT_EQ(back.id_tag_info.status, AuthorizationStatus::Accepted);
}

TEST(Messages, AuthorizeUnknownStatusRejected) {
    json j = json::parse(R"({"idTagInfo":{"status":"Maybe"}})");
    EXPECT_THROW(j.get<AuthorizeResponse>(), ParseError);
}

TEST(Messages, AuthorizeMissingIdTagInfoRejected) {
    json j = json::object();
    EXPECT_THROW(j.get<AuthorizeResponse>(), ParseError);
}

TEST(Messages, StatusNotificationRoundTrip) {
    StatusNotificationRequest req;
    req.connector_id = 1;
    req.status = ChargePointStatus::Charging;
    req.error_code = ChargePointErrorCode::NoError;
    json j = req;
    EXPECT_EQ(j["status"], "Charging");
    EXPECT_EQ(j["errorCode"], "NoError");
    EXPECT_FALSE(j.contains("info"));
    auto back = j.get<StatusNotificationRequest>();
    EXPECT_EQ(back.connector_id, 1);
    EXPECT_EQ(back.status, ChargePointStatus::Charging);
}

TEST(Messages, StatusNotificationBadStatusRejected) {
    json j = json::parse(R"({"connectorId":1,"errorCode":"NoError","status":"Sleeping"})");
    EXPECT_THROW(j.get<StatusNotificationRequest>(), ParseError);
}

TEST(Messages, StartTransactionRoundTrip) {
    StartTransactionRequest req;
    req.connector_id = 1;
    req.id_tag = "TAG123";
    req.meter_start = 1000;
    req.timestamp = "2026-10-16T12:00:00Z";
    json j = req;
    EXPECT_EQ(j["meterStart"], 1000);
    EXPECT_FALSE(j.contains("reservationId"));
    auto back = j.get<StartTransactionRequest>();
    EXPECT_EQ(back.id_tag, "TAG123");
    EXPECT_FALSE(back.reservation_id.has_value());
}

TEST(Messages, StartTransactionConnectorZeroRejected) {
    json j = json::parse(
        R"({"connectorId":0,"idTag":"T","meterStart":0,"timestamp":"2026-10-16T12:00:00Z"})");
    EXPECT_THROW(j.get<StartTransactionRequest>(), ParseError);
}

TEST(Messages, StartTransactionHugeIntegerRejected) {
    json j = json::parse(
        R"({"connectorId":1,"idTag":"T","meterStart":99999999999,"timestamp":"x"})");
    EXPECT_THROW(j.get<StartTransactionRequest>(), ParseError);
}

TEST(Messages, MeterValuesParsesNestedArrays) {
    json j = json::parse(R"({
        "connectorId": 1,
        "transactionId": 42,
        "meterValue": [{
            "timestamp": "2026-10-16T12:05:00Z",
            "sampledValue": [
                {"value":"1250","measurand":"Energy.Active.Import.Register","unit":"Wh"},
                {"value":"16.0","measurand":"Current.Import","unit":"A"}
            ]
        }]
    })");
    auto req = j.get<MeterValuesRequest>();
    ASSERT_EQ(req.meter_value.size(), 1u);
    ASSERT_EQ(req.meter_value[0].sampled_value.size(), 2u);
    EXPECT_EQ(req.meter_value[0].sampled_value[0].value, "1250");
    EXPECT_EQ(req.meter_value[0].sampled_value[1].unit.value(), "A");
    EXPECT_EQ(req.transaction_id.value(), 42);
}

TEST(Messages, MeterValuesEmptyListRejected) {
    json j = json::parse(R"({"connectorId":1,"meterValue":[]})");
    EXPECT_THROW(j.get<MeterValuesRequest>(), ParseError);
}

// ---- StopTransaction: these fail until you finish the exercise ----

TEST(Messages, StopTransactionRoundTrip) {
    StopTransactionRequest req;
    req.meter_stop = 1500;
    req.timestamp = "2026-10-16T13:00:00Z";
    req.transaction_id = 42;
    req.reason = StopReason::EVDisconnected;
    json j = req;
    EXPECT_EQ(j["reason"], "EVDisconnected");
    EXPECT_FALSE(j.contains("idTag"));
    auto back = j.get<StopTransactionRequest>();
    EXPECT_EQ(back.transaction_id, 42);
    EXPECT_EQ(back.meter_stop, 1500);
    EXPECT_EQ(back.reason.value(), StopReason::EVDisconnected);
    EXPECT_FALSE(back.id_tag.has_value());
}

TEST(Messages, StopTransactionBadReasonRejected) {
    json j = json::parse(
        R"({"meterStop":1,"timestamp":"x","transactionId":1,"reason":"Bored"})");
    EXPECT_THROW(j.get<StopTransactionRequest>(), ParseError);
}

TEST(Messages, StopTransactionResponseWithoutIdTagInfoIsValid) {
    json j = json::object();
    auto res = j.get<StopTransactionResponse>();
    EXPECT_FALSE(res.id_tag_info.has_value());
}

TEST(Messages, StopTransactionResponseWithIdTagInfo) {
    json j = json::parse(R"({"idTagInfo":{"status":"Accepted"}})");
    auto res = j.get<StopTransactionResponse>();
    ASSERT_TRUE(res.id_tag_info.has_value());
    EXPECT_EQ(res.id_tag_info->status, AuthorizationStatus::Accepted);
}