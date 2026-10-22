#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS  // getenv
#endif

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chargelink/db/repository.hpp"

using namespace chargelink::db;

namespace {
    const char* const kT1 = "2026-10-21T10:00:00Z";
    const char* const kT2 = "2026-10-21T11:00:00Z";
    const char* const kT3 = "2026-10-21T12:00:00Z";
}  // namespace

class QueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* env = std::getenv("CHARGELINK_TEST_DB");
        if (env == nullptr) {
            GTEST_SKIP() << "set CHARGELINK_TEST_DB to run the database tests";
        }
        conninfo_ = env;
        clean();
        repo = std::make_unique<Repository>(conninfo_);
    }
    void TearDown() override {
        repo.reset();
        if (!conninfo_.empty()) clean();
    }
    void clean() {
        pqxx::connection c{ conninfo_ };
        pqxx::work tx{ c };
        tx.exec0("DELETE FROM meter_values WHERE left(charge_point_id,5)='TEST_'");
        tx.exec0("DELETE FROM status_events WHERE left(charge_point_id,5)='TEST_'");
        tx.exec0("DELETE FROM transactions WHERE left(charge_point_id,5)='TEST_'");
        tx.exec0("DELETE FROM charge_points WHERE left(id,5)='TEST_'");
        tx.commit();
    }

    // Two finished sessions (500 Wh and 1500 Wh) and one open session on the same connector.
    std::vector<int> make_history(const std::string& cp) {
        repo->upsert_boot(cp, { "Acme", "Zap", "SN", "1.0" });
        const int a = repo->start_transaction(cp, 1, "TEST_TAG", 100, kT1).id;
        repo->stop_transaction(cp, a, 600, kT1, "Local");
        const int b = repo->start_transaction(cp, 1, "TEST_TAG", 600, kT2).id;
        repo->stop_transaction(cp, b, 2100, kT2, "EVDisconnected");
        const int c = repo->start_transaction(cp, 1, "TEST_TAG", 2100, kT3).id;
        return { a, b, c };
    }

    std::string conninfo_;
    std::unique_ptr<Repository> repo;
};

TEST_F(QueryTest, ListChargersShowsABootedCharger) {
    repo->upsert_boot("TEST_Q1", { "Acme", "Zap", "SN", "1.0" });
    repo->heartbeat("TEST_Q1");
    const auto all = repo->list_chargers();
    const auto it = std::find_if(all.begin(), all.end(), [](const ChargerRow& c) { return c.id == "TEST_Q1"; });
    ASSERT_TRUE(it != all.end());
    EXPECT_EQ(it->vendor, "Acme");
    EXPECT_EQ(it->firmware, "1.0");
    EXPECT_TRUE(it->online);
    EXPECT_FALSE(it->last_heartbeat.empty());
}

TEST_F(QueryTest, TransactionsComeNewestFirst) {
    const auto ids = make_history("TEST_Q2");
    const auto rows = repo->recent_transactions(10, std::string("TEST_Q2"));
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].id, ids[2]);  // the open one is newest
    EXPECT_FALSE(rows[0].meter_stop_wh.has_value());
    EXPECT_TRUE(rows[0].stopped_at.empty());
    EXPECT_EQ(rows[2].id, ids[0]);
    ASSERT_TRUE(rows[2].meter_stop_wh.has_value());
    EXPECT_EQ(*rows[2].meter_stop_wh, 600);
    EXPECT_EQ(rows[2].stop_reason, "Local");
    EXPECT_FALSE(rows[2].started_at.empty());
}

TEST_F(QueryTest, TheLimitIsRespected) {
    make_history("TEST_Q3");
    EXPECT_EQ(repo->recent_transactions(2, std::string("TEST_Q3")).size(), 2u);
    EXPECT_EQ(repo->recent_transactions(0, std::string("TEST_Q3")).size(), 1u);  // clamped up to 1
}

TEST_F(QueryTest, AnUnknownChargerHasNoTransactions) {
    EXPECT_TRUE(repo->recent_transactions(10, std::string("TEST_NOPE")).empty());
}

TEST_F(QueryTest, WithoutAFilterAllChargersAreIncluded) {
    make_history("TEST_Q5");
    make_history("TEST_Q6");
    const auto rows = repo->recent_transactions(1000, std::nullopt);
    const auto ours = std::count_if(rows.begin(), rows.end(), [](const TransactionRow& t) {
        return t.charge_point_id == "TEST_Q5" || t.charge_point_id == "TEST_Q6";
        });
    EXPECT_EQ(ours, 6);
}

TEST_F(QueryTest, EnergyCountsFinishedSessionsOnly) {
    make_history("TEST_Q7");
    const auto all = repo->energy_per_charger();
    const auto it = std::find_if(all.begin(), all.end(), [](const EnergyRow& e) { return e.charge_point_id == "TEST_Q7"; });
    ASSERT_TRUE(it != all.end());
    EXPECT_EQ(it->sessions, 2);   // the open session is not counted
    EXPECT_EQ(it->wh, 2000);      // (600-100) + (2100-600)
}