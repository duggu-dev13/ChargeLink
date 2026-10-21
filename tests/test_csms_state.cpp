#include <gtest/gtest.h>

#include <set>
#include <string>
#include <thread>
#include <vector>

#include "chargelink/csms/state.hpp"

using namespace chargelink::csms;
using Stop = TransactionStore::StopResult;

TEST(ChargerStates, BootThenForget) {
    ChargerStates s;
    EXPECT_FALSE(s.is_booted("CP001"));
    s.mark_booted("CP001");
    s.mark_booted("CP001");
    EXPECT_TRUE(s.is_booted("CP001"));
    EXPECT_FALSE(s.is_booted("CP002"));
    EXPECT_EQ(s.booted_count(), 1u);
    s.forget("CP001");
    EXPECT_FALSE(s.is_booted("CP001"));
}

TEST(Transactions, IdsAreUniqueAndStartAt1000) {
    TransactionStore t;
    auto a = t.start("CP001", 1, "TAG", 0, "2026-01-01T00:00:00Z");
    auto b = t.start("CP001", 1, "TAG", 0, "2026-01-01T01:00:00Z");
    EXPECT_EQ(a.id, 1000);
    EXPECT_EQ(b.id, 1001);
    EXPECT_FALSE(a.duplicate);
    EXPECT_EQ(t.active_count(), 2u);
}

TEST(Transactions, ARetriedStartGetsTheSameId) {
    TransactionStore t;
    auto a = t.start("CP001", 1, "TAG", 50, "T1");
    auto again = t.start("CP001", 1, "TAG", 50, "T1");  // reply was lost, charger resent
    EXPECT_TRUE(again.duplicate);
    EXPECT_EQ(again.id, a.id);
    EXPECT_EQ(t.total(), 1u);  // one billable transaction, not two
}

TEST(Transactions, StartsThatDifferInAnyFieldAreDifferent) {
    TransactionStore t;
    t.start("CP001", 1, "TAG", 50, "T1");
    EXPECT_FALSE(t.start("CP002", 1, "TAG", 50, "T1").duplicate);
    EXPECT_FALSE(t.start("CP001", 2, "TAG", 50, "T1").duplicate);
    EXPECT_FALSE(t.start("CP001", 1, "TAH", 50, "T1").duplicate);
    EXPECT_FALSE(t.start("CP001", 1, "TAG", 51, "T1").duplicate);
    EXPECT_FALSE(t.start("CP001", 1, "TAG", 50, "T2").duplicate);
    EXPECT_EQ(t.total(), 6u);
}

TEST(Transactions, StopClosesAndRecordsTheReading) {
    TransactionStore t;
    int id = t.start("CP001", 1, "TAG", 100, "T1").id;
    EXPECT_TRUE(t.stop("CP001", id, 3780, "T2") == Stop::Stopped);
    auto txn = t.find(id);
    ASSERT_TRUE(txn.has_value());
    EXPECT_FALSE(txn->active);
    EXPECT_EQ(txn->meter_stop, 3780);
    EXPECT_EQ(txn->stop_time, "T2");
    EXPECT_EQ(t.active_count(), 0u);
}

TEST(Transactions, AStopRetryIsADuplicateNotAnError) {
    TransactionStore t;
    int id = t.start("CP001", 1, "TAG", 0, "T1").id;
    t.stop("CP001", id, 500, "T2");
    EXPECT_TRUE(t.stop("CP001", id, 500, "T2") == Stop::Duplicate);
    EXPECT_EQ(t.find(id)->meter_stop, 500);  // the first stop stands
}

TEST(Transactions, BadStopsAreRefused) {
    TransactionStore t;
    int id = t.start("CP001", 1, "TAG", 100, "T1").id;
    EXPECT_TRUE(t.stop("CP001", 9999, 500, "T2") == Stop::Unknown);
    EXPECT_TRUE(t.stop("CP002", id, 500, "T2") == Stop::WrongCharger);
    EXPECT_TRUE(t.stop("CP001", id, 99, "T2") == Stop::MeterDecreased);
    EXPECT_TRUE(t.find(id)->active);  // none of the refusals changed anything
}

TEST(Transactions, MeterValuesOnlyForTheOwnersActiveTransaction) {
    TransactionStore t;
    int id = t.start("CP001", 1, "TAG", 100, "T1").id;
    EXPECT_TRUE(t.record_meter("CP001", id, 700));
    EXPECT_EQ(t.find(id)->last_meter_wh, 700);
    EXPECT_TRUE(t.record_meter("CP001", id, 650));  // late sample: the reading never goes down
    EXPECT_EQ(t.find(id)->last_meter_wh, 700);
    EXPECT_FALSE(t.record_meter("CP002", id, 800));
    EXPECT_FALSE(t.record_meter("CP001", 4242, 800));
    t.stop("CP001", id, 900, "T2");
    EXPECT_FALSE(t.record_meter("CP001", id, 950));  // ended
}

namespace {
    constexpr int kThreads = 8;  // namespace scope: MSVC won't let lambdas use locals uncaptured
    constexpr int kEach = 500;
}  // namespace

TEST(Transactions, ManyThreadsStartingGetDistinctIds) {
    TransactionStore t;
    std::vector<std::vector<int>> ids(kThreads);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&t, &ids, i] {
            for (int n = 0; n < kEach; ++n) {
                ids[i].push_back(t.start("CP" + std::to_string(i), 1, "TAG", n, "T").id);
            }
            });
    }
    for (auto& th : threads) th.join();
    std::set<int> all;
    for (auto& v : ids) all.insert(v.begin(), v.end());
    EXPECT_EQ(all.size(), static_cast<std::size_t>(kThreads * kEach));
    EXPECT_EQ(t.total(), static_cast<std::size_t>(kThreads * kEach));
}