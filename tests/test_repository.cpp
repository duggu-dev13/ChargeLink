#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS  // getenv
#endif

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "chargelink/db/repository.hpp"

using namespace chargelink::db;
using Kind = StartOutcome::Kind;

namespace {
    // Namespace scope: MSVC won't let a lambda use a local constexpr uncaptured.
    constexpr int kThreads = 8;
    const char* const kNow = "2026-10-21T10:00:00Z";
    const char* const kLater = "2026-10-21T11:00:00Z";
}  // namespace

class RepositoryTest : public ::testing::Test {
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
        tx.exec0("DELETE FROM charge_points WHERE left(id,5)='TEST_'");  // cascades connectors
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
    void boot(const std::string& cp) { repo->upsert_boot(cp, { "Acme", "Zap", "SN", "1.0" }); }

    std::string conninfo_;
    std::unique_ptr<Repository> repo;
};

TEST_F(RepositoryTest, AuthorizeKnownBlockedExpiredAndUnknownTags) {
    sql("INSERT INTO id_tags(id_tag,status) VALUES ('TEST_OK','Accepted'),('TEST_BLOCKED','Blocked')");
    sql("INSERT INTO id_tags(id_tag,status,expires_at) VALUES ('TEST_OLD','Accepted', now() - interval '1 day')");
    EXPECT_EQ(repo->authorize("TEST_OK"), "Accepted");
    EXPECT_EQ(repo->authorize("TEST_BLOCKED"), "Blocked");
    EXPECT_EQ(repo->authorize("TEST_OLD"), "Expired");
    EXPECT_EQ(repo->authorize("TEST_NOPE"), "Invalid");
}

TEST_F(RepositoryTest, BootCreatesThenUpdatesTheChargePoint) {
    repo->upsert_boot("TEST_CP1", { "Acme", "Zap", "SN1", "1.0" });
    EXPECT_EQ(value<std::string>("SELECT vendor FROM charge_points WHERE id='TEST_CP1'"), "Acme");
    EXPECT_TRUE(value<bool>("SELECT online FROM charge_points WHERE id='TEST_CP1'"));
    repo->upsert_boot("TEST_CP1", { "Acme", "Zap", "SN1", "2.0" });  // second boot: update, no error
    EXPECT_EQ(value<std::string>("SELECT firmware_version FROM charge_points WHERE id='TEST_CP1'"), "2.0");
    EXPECT_EQ(value<int>("SELECT count(*) FROM charge_points WHERE id='TEST_CP1'"), 1);
}

TEST_F(RepositoryTest, OnlineFlagAndHeartbeat) {
    boot("TEST_CP2");
    repo->set_online("TEST_CP2", false);
    EXPECT_FALSE(value<bool>("SELECT online FROM charge_points WHERE id='TEST_CP2'"));
    repo->heartbeat("TEST_CP2");
    EXPECT_TRUE(value<bool>("SELECT last_heartbeat_at IS NOT NULL FROM charge_points WHERE id='TEST_CP2'"));
}

TEST_F(RepositoryTest, StatusUpdatesTheConnectorAndKeepsHistory) {
    boot("TEST_CP3");
    repo->record_status("TEST_CP3", 1, "Charging", "NoError", kNow);
    repo->record_status("TEST_CP3", 1, "Available", "NoError", kLater);
    repo->record_status("TEST_CP3", 0, "Available", "NoError", kLater);  // whole charger
    EXPECT_EQ(value<std::string>("SELECT status FROM connectors WHERE charge_point_id='TEST_CP3' AND connector_id=1"), "Available");
    EXPECT_EQ(value<int>("SELECT count(*) FROM connectors WHERE charge_point_id='TEST_CP3'"), 1);
    EXPECT_EQ(value<int>("SELECT count(*) FROM status_events WHERE charge_point_id='TEST_CP3'"), 3);
}

TEST_F(RepositoryTest, StartThenStop) {
    boot("TEST_CP4");
    const auto started = repo->start_transaction("TEST_CP4", 1, "TEST_TAG", 100, kNow);
    EXPECT_TRUE(started.kind == Kind::Created);
    EXPECT_GE(started.id, 1000);
    EXPECT_TRUE(repo->stop_transaction("TEST_CP4", started.id, 3780, kLater, "Local") == StopResult::Stopped);
    const std::string where = " FROM transactions WHERE id=" + std::to_string(started.id);
    EXPECT_EQ(value<long long>("SELECT meter_stop_wh" + where), 3780);
    EXPECT_EQ(value<std::string>("SELECT stop_reason" + where), "Local");
    // The connector is free again.
    EXPECT_TRUE(repo->start_transaction("TEST_CP4", 1, "TEST_TAG", 3780, kLater).kind == Kind::Created);
}

TEST_F(RepositoryTest, ARetriedStartGetsTheSameId) {
    boot("TEST_CP5");
    const auto first = repo->start_transaction("TEST_CP5", 1, "TEST_TAG", 50, kNow);
    const auto again = repo->start_transaction("TEST_CP5", 1, "TEST_TAG", 50, kNow);
    EXPECT_TRUE(again.kind == Kind::Duplicate);
    EXPECT_EQ(again.id, first.id);
    EXPECT_EQ(value<int>("SELECT count(*) FROM transactions WHERE charge_point_id='TEST_CP5'"), 1);
}

TEST_F(RepositoryTest, SecondOpenTransactionOnAConnectorIsRefused) {
    boot("TEST_CP6");
    repo->start_transaction("TEST_CP6", 1, "TEST_A", 0, kNow);
    const auto other = repo->start_transaction("TEST_CP6", 1, "TEST_B", 0, kNow);
    EXPECT_TRUE(other.kind == Kind::ConnectorBusy);
    EXPECT_EQ(other.id, 0);
}

TEST_F(RepositoryTest, BadStopsAreRefused) {
    boot("TEST_CP7");
    const int id = repo->start_transaction("TEST_CP7", 1, "TEST_TAG", 100, kNow).id;
    EXPECT_TRUE(repo->stop_transaction("TEST_CP7", 2000000000, 500, kLater, "") == StopResult::NotFound);
    EXPECT_TRUE(repo->stop_transaction("TEST_OTHER", id, 500, kLater, "") == StopResult::WrongCharger);
    EXPECT_TRUE(repo->stop_transaction("TEST_CP7", id, 99, kLater, "") == StopResult::MeterDecreased);
    EXPECT_TRUE(repo->stop_transaction("TEST_CP7", id, 500, kLater, "") == StopResult::Stopped);
    EXPECT_TRUE(repo->stop_transaction("TEST_CP7", id, 500, kLater, "") == StopResult::AlreadyStopped);
    EXPECT_EQ(value<long long>("SELECT meter_stop_wh FROM transactions WHERE id=" + std::to_string(id)), 500);
}

TEST_F(RepositoryTest, MeterValuesAreStoredWithAndWithoutATransaction) {
    boot("TEST_CP8");
    const int id = repo->start_transaction("TEST_CP8", 1, "TEST_TAG", 0, kNow).id;
    repo->add_meter_value("TEST_CP8", id, kNow, "Energy.Active.Import.Register", 1840.5, "Wh");
    repo->add_meter_value("TEST_CP8", std::nullopt, kNow, "Energy.Active.Import.Register", 7, "Wh");
    EXPECT_EQ(value<int>("SELECT count(*) FROM meter_values WHERE charge_point_id='TEST_CP8'"), 2);
    EXPECT_EQ(value<int>("SELECT count(*) FROM meter_values WHERE transaction_id IS NULL AND charge_point_id='TEST_CP8'"), 1);
}

TEST_F(RepositoryTest, BadTimestampThrowsAndThePoolSurvives) {
    boot("TEST_CP9");
    for (int i = 0; i < 10; ++i) {  // more failures than pool slots: a leaked connection would hang here
        EXPECT_ANY_THROW(repo->record_status("TEST_CP9", 1, "Available", "NoError", "not a time"));
    }
    EXPECT_NO_THROW(repo->heartbeat("TEST_CP9"));
}

TEST_F(RepositoryTest, RacingStartsOnOneConnectorHaveOneWinner) {
    boot("TEST_RACE");
    std::atomic<int> created{ 0 }, busy{ 0 }, failed{ 0 };
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            try {
                const auto r = repo->start_transaction("TEST_RACE", 1, "TEST_T" + std::to_string(i), 0, kNow);
                if (r.kind == Kind::Created) ++created;
                if (r.kind == Kind::ConnectorBusy) ++busy;
            }
            catch (const std::exception&) {
                ++failed;
            }
            });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(failed.load(), 0);
    EXPECT_EQ(created.load(), 1);
    EXPECT_EQ(busy.load(), kThreads - 1);
}

TEST_F(RepositoryTest, ParallelStartsOnDifferentConnectorsGetDistinctIds) {
    boot("TEST_PAR");
    std::vector<int> ids(kThreads, 0);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            try {
                ids[i] = repo->start_transaction("TEST_PAR", i + 1, "TEST_TAG", 0, kNow).id;
            }
            catch (const std::exception&) {
            }
            });
    }
    for (auto& t : threads) t.join();
    const std::set<int> distinct(ids.begin(), ids.end());
    EXPECT_EQ(distinct.size(), static_cast<std::size_t>(kThreads));
    EXPECT_EQ(distinct.count(0), 0u);
}