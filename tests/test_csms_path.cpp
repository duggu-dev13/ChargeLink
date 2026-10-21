#include <gtest/gtest.h>

#include <string>

#include "chargelink/csms/path.hpp"

using namespace chargelink::csms;

TEST(CsmsPath, ParsesTheChargePointId) {
    auto id = parse_charge_point_path("/ocpp/CP001");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "CP001");
    EXPECT_EQ(*parse_charge_point_path("/ocpp/scharge-A1_07.x~y"), "scharge-A1_07.x~y");
}

TEST(CsmsPath, RejectsWrongPrefixes) {
    EXPECT_FALSE(parse_charge_point_path("").has_value());
    EXPECT_FALSE(parse_charge_point_path("/").has_value());
    EXPECT_FALSE(parse_charge_point_path("/ocpp").has_value());
    EXPECT_FALSE(parse_charge_point_path("/ocpp/").has_value());
    EXPECT_FALSE(parse_charge_point_path("ocpp/CP001").has_value());
    EXPECT_FALSE(parse_charge_point_path("/OCPP/CP001").has_value());
    EXPECT_FALSE(parse_charge_point_path("/other/CP001").has_value());
    EXPECT_FALSE(parse_charge_point_path("/ocpp1.6/CP001").has_value());
}

TEST(CsmsPath, RejectsDangerousIds) {
    EXPECT_FALSE(parse_charge_point_path("/ocpp/CP/001").has_value());     // extra path
    EXPECT_FALSE(parse_charge_point_path("/ocpp/CP001/").has_value());     // trailing slash
    EXPECT_FALSE(parse_charge_point_path("/ocpp/CP001?x=1").has_value());  // query string
    EXPECT_FALSE(parse_charge_point_path("/ocpp/CP%20001").has_value());   // percent-encoding
    EXPECT_FALSE(parse_charge_point_path("/ocpp/CP 001").has_value());     // space
    EXPECT_FALSE(parse_charge_point_path("/ocpp/../etc").has_value());
    EXPECT_FALSE(parse_charge_point_path(std::string("/ocpp/CP\n001")).has_value());
    std::string with_nul = "/ocpp/CP";
    with_nul.push_back('\0');
    with_nul += "01";
    EXPECT_FALSE(parse_charge_point_path(with_nul).has_value());  // embedded NUL
    EXPECT_FALSE(parse_charge_point_path("/ocpp/C\xC3\xA9P").has_value());  // non-ASCII
}

TEST(CsmsPath, LengthLimitIs48) {
    EXPECT_TRUE(parse_charge_point_path("/ocpp/" + std::string(48, 'a')).has_value());
    EXPECT_FALSE(parse_charge_point_path("/ocpp/" + std::string(49, 'a')).has_value());
}

TEST(CsmsSubprotocol, FindsTheProtocolInTheList) {
    EXPECT_TRUE(offers_subprotocol("ocpp1.6", "ocpp1.6"));
    EXPECT_TRUE(offers_subprotocol("ocpp2.0.1, ocpp1.6", "ocpp1.6"));
    EXPECT_TRUE(offers_subprotocol("ocpp2.0.1,ocpp1.6", "ocpp1.6"));
    EXPECT_TRUE(offers_subprotocol("ocpp1.6,ocpp2.0.1", "ocpp1.6"));
    EXPECT_TRUE(offers_subprotocol("  ocpp1.6  ", "ocpp1.6"));
    EXPECT_TRUE(offers_subprotocol("a,\tocpp1.6\t,b", "ocpp1.6"));
}

TEST(CsmsSubprotocol, MatchesWholeTokensOnly) {
    EXPECT_FALSE(offers_subprotocol("xocpp1.6", "ocpp1.6"));
    EXPECT_FALSE(offers_subprotocol("ocpp1.6x", "ocpp1.6"));
    EXPECT_FALSE(offers_subprotocol("ocpp2.0.1", "ocpp1.6"));
    EXPECT_FALSE(offers_subprotocol("ocpp1.6.1", "ocpp1.6"));
    EXPECT_FALSE(offers_subprotocol("", "ocpp1.6"));
    EXPECT_FALSE(offers_subprotocol(",,,", "ocpp1.6"));
    EXPECT_FALSE(offers_subprotocol("OCPP1.6", "ocpp1.6"));  // case-sensitive
}

TEST(CsmsLog, ControlCharactersAreNeutralised) {
    EXPECT_EQ(safe_for_log("plain text"), "plain text");
    EXPECT_EQ(safe_for_log("a\nb\rc"), "a?b?c");  // no forged log lines
    EXPECT_EQ(safe_for_log(std::string("a\0b", 3)), "a?b");
    EXPECT_EQ(safe_for_log("caf\xC3\xA9"), "caf??");
}

TEST(CsmsLog, LongTextIsCut) {
    EXPECT_EQ(safe_for_log(std::string(100, 'x'), 10), std::string(10, 'x') + "...");
    EXPECT_EQ(safe_for_log(std::string(10, 'x'), 10), std::string(10, 'x'));
    EXPECT_EQ(safe_for_log(""), "");
}