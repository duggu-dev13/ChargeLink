#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS  // getenv
#endif

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <cstdlib>
#include <memory>
#include <string>

#include "chargelink/csms/handlers.hpp"
#include "chargelink/db/repository.hpp"
#include "chargelink/ocpp/dispatcher.hpp"

using namespace chargelink;

namespace {

    const char* const kStamp = "2026-10-21T10:00:00Z";
    const char* const kLater = "2026-10-21T11:00:00Z";

    bool is_result(const std::string& reply) { return reply.rfind("[3,", 0) == 0; }
    bool is_error(const std::string& reply, const std::string& code) {
        return reply.rfind("[4,", 0) == 0 && reply.find("\"" + code + "\"") != std::string::npos;
    }
    bool contains(const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    }

    std::string start_payload(int connector, const std::string& tag, int meter, const std::string& ts) {
        return "{\"connectorId\":" + std::to_string(connector) + ",\"idTag\":\"" + tag +
            "\",\"meterStart\":" + std::to_string(meter) + ",\"timestamp\":\"" + ts + "\"}";
    }

    std::string stop_payload(int tx_id, int meter) {
        return "{\"meterStop\":" + std::to_string(meter) + ",\"timestamp\":\"" + kLater +
            "\",\"transactionId\":" + std::to_string(tx_id) + "}";
    }

    std::string meter_payload(int tx_id, const std::string& value) {
        return "{\"connectorId\":1,\"transactionId\":" + std::to_string(tx_id) +
            ",\"meterValue\":[{\"timestamp\":\"" + kStamp + "\",\"sampledValue\":[{\"value\":\"" +
            value + "\",\"measurand\":\"Energy.Active.Import.Register\",\"unit\":\"Wh\"}]}]}";
    }

    int tx_id_of(const std::string& reply) {
        return nlohmann::json::parse(reply)[2]["transactionId"].get<int>();
    }

}  // namespace

class HandlersTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* env = std::getenv("CHARGELINK_TEST_DB");
        if (env == nullptr) {
            GTEST_SKIP() << "set CHARGELINK_TEST_DB to run the database tests";
        }
        conninfo_ = env;
        clean();
        auto repo = std::make_shared<db::Repository>(conninfo_);
        dispatcher_ = std::make_unique<ocpp::Dispatcher>(csms::make_db_dispatcher(repo));
    }
    void TearDown() override {
        dispatcher_.reset();
        if (!conninfo_.empty()) clean();
    }

    std::string call(const std::string& cp, const std::string& action, const std::string& payload) {
        const std::string frame =
            "[2,\"" + std::to_string(++counter_) + "\",\"" + action + "\"," + payload + "]";
        const auto reply = dispatcher_->handle(cp, frame);
        return reply ? *reply : std::string();
    }
    std::string boot(const std::string& cp) {
        return call(cp, "BootNotification", "{\"chargePointVendor\":\"Acme\",\"chargePointModel\":\"Zap\"}");
    }

    void clean() {
        pqxx::connection c{ conninfo_ };
        pqxx::work tx{ c };
        tx.exec0("DELETE FROM meter_values WHERE left(charge_point_id,5)='TEST_'");
        tx.exec0("DELETE FROM status_events WHERE left(charge_point_id,5)='TEST_'");
        tx.exec0("DELETE FROM transactions WHERE left(charge_point_id,5)='TEST_'");
        tx.exec0("DELETE FROM charge_points WHERE left(id,5)='TEST_'");
        tx.exec0("DELETE FROM id_tags WHERE left(id_tag,5)='TEST_'");
        tx.commit();
    }
    void sql(const std::string& statement) {
        pqxx::connection c{ conninfo_ };
        pqxx::work tx{ c };
        tx.exec0(statement);
        tx.commit();
    }
    template <class T>
    T value(const std::string& query) {
        pqxx::connection c{ conninfo_ };
        pqxx::work tx{ c };
        return tx.query_value<T>(query);
    }

    std::string conninfo_;
    std::unique_ptr<ocpp::Dispatcher> dispatcher_;
    int counter_ = 0;
};

TEST_F(HandlersTest, HeartbeatBeforeBootIsRefused) {
    EXPECT_TRUE(is_error(call("TEST_H1", "Heartbeat", "{}"), "SecurityError"));
}

TEST_F(HandlersTest, BootPersistsAndHeartbeatWorks) {
    EXPECT_TRUE(is_result(boot("TEST_H2")));
    EXPECT_EQ(value<std::string>("SELECT vendor FROM charge_points WHERE id='TEST_H2'"), "Acme");
    EXPECT_TRUE(value<bool>("SELECT online FROM charge_points WHERE id='TEST_H2'"));
    EXPECT_TRUE(is_result(call("TEST_H2", "Heartbeat", "{}")));
    EXPECT_TRUE(value<bool>("SELECT last_heartbeat_at IS NOT NULL FROM charge_points WHERE id='TEST_H2'"));
}

TEST_F(HandlersTest, AFullSessionIsPersisted) {
    sql("INSERT INTO id_tags(id_tag) VALUES ('TEST_OK')");
    ASSERT_TRUE(is_result(boot("TEST_H3")));
    EXPECT_TRUE(is_result(call("TEST_H3", "StatusNotification",
        "{\"connectorId\":1,\"errorCode\":\"NoError\",\"status\":\"Preparing\"}")));
    const std::string started = call("TEST_H3", "StartTransaction", start_payload(1, "TEST_OK", 100, kStamp));
    ASSERT_TRUE(is_result(started));
    EXPECT_TRUE(contains(started, "\"Accepted\""));
    const int id = tx_id_of(started);
    EXPECT_GE(id, 1000);
    EXPECT_TRUE(is_result(call("TEST_H3", "MeterValues", meter_payload(id, "100"))));
    EXPECT_TRUE(is_result(call("TEST_H3", "MeterValues", meter_payload(id, "650"))));
    EXPECT_TRUE(is_result(call("TEST_H3", "StopTransaction", stop_payload(id, 700))));

    const std::string tx = std::to_string(id);
    EXPECT_EQ(value<long long>("SELECT meter_stop_wh FROM transactions WHERE id=" + tx), 700);
    EXPECT_EQ(value<int>("SELECT count(*) FROM meter_values WHERE transaction_id=" + tx), 2);
    EXPECT_EQ(value<int>("SELECT count(*) FROM status_events WHERE charge_point_id='TEST_H3'"), 1);
    EXPECT_EQ(value<std::string>("SELECT status FROM connectors WHERE charge_point_id='TEST_H3' AND connector_id=1"),
        "Preparing");
}

TEST_F(HandlersTest, ARetriedStartReturnsTheSameId) {
    sql("INSERT INTO id_tags(id_tag) VALUES ('TEST_OK')");
    ASSERT_TRUE(is_result(boot("TEST_H4")));
    const std::string first = call("TEST_H4", "StartTransaction", start_payload(1, "TEST_OK", 50, kStamp));
    const std::string again = call("TEST_H4", "StartTransaction", start_payload(1, "TEST_OK", 50, kStamp));
    ASSERT_TRUE(is_result(first));
    ASSERT_TRUE(is_result(again));
    EXPECT_EQ(tx_id_of(first), tx_id_of(again));
    EXPECT_EQ(value<int>("SELECT count(*) FROM transactions WHERE charge_point_id='TEST_H4'"), 1);
}

TEST_F(HandlersTest, ABusyConnectorIsAGenericError) {
    sql("INSERT INTO id_tags(id_tag) VALUES ('TEST_OK'),('TEST_OK2')");
    ASSERT_TRUE(is_result(boot("TEST_H5")));
    EXPECT_TRUE(is_result(call("TEST_H5", "StartTransaction", start_payload(1, "TEST_OK", 0, kStamp))));
    EXPECT_TRUE(is_error(call("TEST_H5", "StartTransaction", start_payload(1, "TEST_OK2", 0, kStamp)),
        "GenericError"));
}

TEST_F(HandlersTest, BadStartValuesAreRefused) {
    ASSERT_TRUE(is_result(boot("TEST_H6")));
    // The message parser rejects these before the handler runs.
    EXPECT_TRUE(is_error(call("TEST_H6", "StartTransaction", start_payload(0, "TEST_OK", 0, kStamp)),
        "FormationViolation"));
    EXPECT_TRUE(is_error(call("TEST_H6", "StartTransaction", start_payload(1, "TEST_OK", -5, kStamp)),
        "FormationViolation"));
    EXPECT_EQ(value<int>("SELECT count(*) FROM transactions WHERE charge_point_id='TEST_H6'"), 0);
}

TEST_F(HandlersTest, StopForAnUnknownTransactionIsRefused) {
    ASSERT_TRUE(is_result(boot("TEST_H7")));
    EXPECT_TRUE(is_error(call("TEST_H7", "StopTransaction", stop_payload(2000000000, 5)),
        "PropertyConstraintViolation"));
}

TEST_F(HandlersTest, ABadTimestampIsAFormationViolation) {
    ASSERT_TRUE(is_result(boot("TEST_H8")));
    EXPECT_TRUE(is_error(call("TEST_H8", "StartTransaction", start_payload(1, "TEST_OK", 0, "garbage")),
        "FormationViolation"));
    EXPECT_EQ(value<int>("SELECT count(*) FROM transactions WHERE charge_point_id='TEST_H8'"), 0);
}

TEST_F(HandlersTest, TagStatusesComeFromTheDatabase) {
    sql("INSERT INTO id_tags(id_tag,status) VALUES ('TEST_OK','Accepted'),('TEST_BLOCKED','Blocked')");
    ASSERT_TRUE(is_result(boot("TEST_H9")));
    EXPECT_TRUE(contains(call("TEST_H9", "Authorize", "{\"idTag\":\"TEST_OK\"}"), "\"Accepted\""));
    EXPECT_TRUE(contains(call("TEST_H9", "Authorize", "{\"idTag\":\"TEST_BLOCKED\"}"), "\"Blocked\""));
    EXPECT_TRUE(contains(call("TEST_H9", "Authorize", "{\"idTag\":\"TEST_UNKNOWN\"}"), "\"Blocked\""));
}

TEST_F(HandlersTest, MeterValuesForAnUnknownTransactionAreNotStored) {
    ASSERT_TRUE(is_result(boot("TEST_H10")));
    EXPECT_TRUE(is_result(call("TEST_H10", "MeterValues", meter_payload(2000000000, "100"))));
    EXPECT_EQ(value<int>("SELECT count(*) FROM meter_values WHERE charge_point_id='TEST_H10'"), 0);
}