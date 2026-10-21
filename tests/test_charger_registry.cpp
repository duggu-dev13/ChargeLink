#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "chargelink/csms/charger_registry.hpp"

using namespace chargelink::csms;

namespace {

    struct FakeSession : ISession {
        explicit FakeSession(std::string id) : id_(std::move(id)) {}
        const std::string& id() const override { return id_; }
        void close() override { ++closes; }

        std::string id_;
        std::atomic<int> closes{ 0 };
    };

    constexpr int kRounds = 2000;  // namespace scope: MSVC won't let a lambda use a local one uncaptured

    std::shared_ptr<FakeSession> make(const std::string& id) {
        return std::make_shared<FakeSession>(id);
    }

}  // namespace

TEST(Registry, AddAndFind) {
    ChargerRegistry registry;
    auto s = make("CP001");
    auto result = registry.add(s);
    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.replaced == nullptr);
    EXPECT_TRUE(registry.find("CP001") == s);
    EXPECT_TRUE(registry.find("CP999") == nullptr);
    EXPECT_EQ(registry.size(), 1u);
}

TEST(Registry, ANewConnectionReplacesTheOldOneAndHandsItBack) {
    ChargerRegistry registry;
    auto old_session = make("CP001");
    auto new_session = make("CP001");
    registry.add(old_session);

    auto result = registry.add(new_session);
    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.replaced == old_session);  // for the caller to close
    EXPECT_EQ(old_session->closes.load(), 0);     // the registry itself never closes it
    EXPECT_TRUE(registry.find("CP001") == new_session);
    EXPECT_EQ(registry.size(), 1u);
}

TEST(Registry, AnOldConnectionClosingLateDoesNotEvictItsReplacement) {
    ChargerRegistry registry;
    auto old_session = make("CP001");
    auto new_session = make("CP001");
    registry.add(old_session);
    registry.add(new_session);

    EXPECT_FALSE(registry.remove(*old_session));
    EXPECT_TRUE(registry.find("CP001") == new_session);
    EXPECT_TRUE(registry.remove(*new_session));
    EXPECT_TRUE(registry.find("CP001") == nullptr);
    EXPECT_EQ(registry.size(), 0u);
}

TEST(Registry, RemovingAnUnknownSessionReturnsFalse) {
    ChargerRegistry registry;
    auto s = make("CP001");
    EXPECT_FALSE(registry.remove(*s));
}

TEST(Registry, AddingTheSameSessionTwiceReplacesNothing) {
    ChargerRegistry registry;
    auto s = make("CP001");
    registry.add(s);
    auto again = registry.add(s);
    EXPECT_TRUE(again.accepted);
    EXPECT_TRUE(again.replaced == nullptr);
    EXPECT_EQ(registry.size(), 1u);
}

TEST(Registry, ADeadSessionIsNotCounted) {
    ChargerRegistry registry;
    {
        auto s = make("CP001");
        registry.add(s);
        EXPECT_EQ(registry.size(), 1u);
    }  // destroyed: the registry only held a weak pointer
    EXPECT_EQ(registry.size(), 0u);
    EXPECT_TRUE(registry.find("CP001") == nullptr);
    EXPECT_TRUE(registry.ids().empty());

    auto next = make("CP001");
    auto result = registry.add(next);
    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.replaced == nullptr);
}

TEST(Registry, IdsAreSorted) {
    ChargerRegistry registry;
    auto c = make("CP003");
    auto a = make("CP001");
    auto b = make("CP002");
    registry.add(c);
    registry.add(a);
    registry.add(b);
    std::vector<std::string> expected = { "CP001", "CP002", "CP003" };
    EXPECT_TRUE(registry.ids() == expected);
}

TEST(Registry, CloseAllReturnsLiveSessionsAndRefusesNewOnes) {
    ChargerRegistry registry;
    auto a = make("CP001");
    auto b = make("CP002");
    registry.add(a);
    registry.add(b);

    auto live = registry.close_all();
    EXPECT_EQ(live.size(), 2u);
    for (auto& session : live) session->close();
    EXPECT_EQ(a->closes.load(), 1);
    EXPECT_EQ(b->closes.load(), 1);

    auto late = make("CP003");  // finished its handshake after shutdown began
    EXPECT_FALSE(registry.add(late).accepted);
    EXPECT_TRUE(registry.find("CP003") == nullptr);
}

TEST(Registry, NullSessionIsRefused) {
    ChargerRegistry registry;
    EXPECT_FALSE(registry.add(nullptr).accepted);
}

TEST(Registry, ManyThreadsAddingAndRemovingStayConsistent) {
    ChargerRegistry registry;
    constexpr int kThreads = 8;
    //constexpr int kRounds = 2000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&registry, t] {
            for (int i = 0; i < kRounds; ++i) {
                auto s = make("CP" + std::to_string((t + i) % 5));  // ids collide on purpose
                auto result = registry.add(s);
                if (result.replaced) {
                    result.replaced->close();  // outside the lock, as the contract says
                }
                registry.find(s->id());
                registry.size();
                registry.ids();
                registry.remove(*s);
            }
            });
    }
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(registry.size(), 0u);
    EXPECT_TRUE(registry.ids().empty());
}