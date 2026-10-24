#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS  // getenv
#endif

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <cstdlib>
#include <memory>
#include <string>

#include "chargelink/csms/handlers.hpp"
#include "chargelink/db/repository.hpp"

using namespace chargelink;

namespace {
    bool is_result(const std::string& reply) { return reply.rfind("[3,", 0) == 0; }
    bool is_security_error(const std::string& reply) {
        return reply.rfind("[4,", 0) == 0 && reply.find("SecurityError") != std::string::npos;
    }
}  // namespace

class LifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* env = std::getenv("CHARGELINK_TEST_DB");
        if (env == nullptr) {
            GTEST_SKIP() << "set CHARGELINK_TEST_DB to run the database tests";
        }
        conninfo_ = env;
        clean();
        auto repo = std::make_shared<db::Repository>(conninfo_);
        handlers_ = std::make_unique<csms::CsmsHandlers>(csms::make_db_handlers(repo));
    }
    void TearDown() override {
        handlers_.reset();
        if (!conninfo_.empty()) clean();
    }

    std::string call(const std::string& cp, const std::string& action, const std::string& payload) {
        const std::string frame =
            "[2,\"" + std::to_string(++counter_) + "\",\"" + action + "\"," + payload + "]";
        const auto reply = handlers_->dispatcher.handle(cp, frame);
        return reply ? *reply : std::string();
    }
    std::string boot(const std::string& cp) {
        return call(cp, "BootNotification", "{\"chargePointVendor\":\"Acme\",\"chargePointModel\":\"Zap\"}");
    }
    std::string heartbeat(const std::string& cp) { return call(cp, "Heartbeat", "{}"); }
    void connect(const std::string& cp) { handlers_->hooks.on_connect(cp); }
    void disconnect(const std::string& cp) { handlers_->hooks.on_disconnect(cp); }

    bool online(const std::string& cp) {
        pqxx::connection c{ conninfo_ };
        pqxx::work tx{ c };
        return tx.query_value<bool>("SELECT online FROM charge_points WHERE id='" + cp + "'");
    }
    void clean() {
        pqxx::connection c{ conninfo_ };
        pqxx::work tx{ c };
        tx.exec0("DELETE FROM transactions WHERE left(charge_point_id,5)='TEST_'");
        tx.exec0("DELETE FROM charge_points WHERE left(id,5)='TEST_'");
        tx.commit();
    }

    std::string conninfo_;
    std::unique_ptr<csms::CsmsHandlers> handlers_;
    int counter_ = 0;
};

TEST_F(LifecycleTest, AReconnectingChargerMustBootAgain) {
    connect("TEST_L1");
    ASSERT_TRUE(is_result(boot("TEST_L1")));
    EXPECT_TRUE(is_result(heartbeat("TEST_L1")));
    disconnect("TEST_L1");
    connect("TEST_L1");  // the new connection
    EXPECT_TRUE(is_security_error(heartbeat("TEST_L1")));
    ASSERT_TRUE(is_result(boot("TEST_L1")));
    EXPECT_TRUE(is_result(heartbeat("TEST_L1")));
}

TEST_F(LifecycleTest, AReplacementConnectionAlsoMustBootAgain) {
    connect("TEST_L2");
    ASSERT_TRUE(is_result(boot("TEST_L2")));
    connect("TEST_L2");  // a new connection replaces the old one: no disconnect was reported
    EXPECT_TRUE(is_security_error(heartbeat("TEST_L2")));
}

TEST_F(LifecycleTest, ADisconnectMarksTheChargerOffline) {
    connect("TEST_L3");
    ASSERT_TRUE(is_result(boot("TEST_L3")));
    EXPECT_TRUE(online("TEST_L3"));
    disconnect("TEST_L3");
    EXPECT_FALSE(online("TEST_L3"));
}

TEST_F(LifecycleTest, BootingAfterADisconnectMakesItOnlineAgain) {
    connect("TEST_L4");
    ASSERT_TRUE(is_result(boot("TEST_L4")));
    disconnect("TEST_L4");
    connect("TEST_L4");
    ASSERT_TRUE(is_result(boot("TEST_L4")));
    EXPECT_TRUE(online("TEST_L4"));
}

TEST_F(LifecycleTest, ShutdownMarksEveryChargerOffline) {
    connect("TEST_L5");
    connect("TEST_L6");
    ASSERT_TRUE(is_result(boot("TEST_L5")));
    ASSERT_TRUE(is_result(boot("TEST_L6")));
    handlers_->hooks.on_shutdown();
    EXPECT_FALSE(online("TEST_L5"));
    EXPECT_FALSE(online("TEST_L6"));
}

TEST_F(LifecycleTest, HooksForAnUnknownChargerAreHarmless) {
    EXPECT_NO_THROW(connect("TEST_L7"));
    EXPECT_NO_THROW(disconnect("TEST_L7"));
}