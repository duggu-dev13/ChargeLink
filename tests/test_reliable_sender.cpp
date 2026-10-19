#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "chargelink/ocpp/reliable_sender.hpp"

using namespace chargelink::ocpp;

namespace {

    // Records every CALL and lets the test deliver the outcome by hand.
    struct FakeCaller : ICaller {
        struct Sent {
            std::string action;
            CallTracker::Callback done;
        };
        std::vector<Sent> sent;

        void call(std::string action, json, CallTracker::Callback done) override {
            sent.push_back({ std::move(action), std::move(done) });
        }

        std::vector<std::string> actions() const {
            std::vector<std::string> out;
            for (const auto& s : sent) out.push_back(s.action);
            return out;
        }
    };

    // Answers immediately, inside call().
    struct SyncCaller : ICaller {
        int calls = 0;
        void call(std::string, json, CallTracker::Callback done) override {
            ++calls;
            CallOutcome outcome;
            outcome.status = CallOutcome::Status::Result;
            done(outcome);
        }
    };

    CallOutcome outcome(CallOutcome::Status status) {
        CallOutcome o;
        o.status = status;
        if (status == CallOutcome::Status::Error) o.error_code = "InternalError";
        return o;
    }

    const auto kResult = CallOutcome::Status::Result;
    const auto kTimeout = CallOutcome::Status::Timeout;
    const auto kError = CallOutcome::Status::Error;
    const auto kDisconnected = CallOutcome::Status::Disconnected;

}  // namespace

TEST(ReliableSender, NothingIsSentWhileOffline) {
    FakeCaller caller;
    ReliableSender sender(caller);
    EXPECT_TRUE(sender.enqueue("A", json::object(), nullptr));
    EXPECT_TRUE(caller.sent.empty());
    EXPECT_EQ(sender.pending(), 1u);
    EXPECT_FALSE(sender.online());
}

TEST(ReliableSender, GoingOnlineSendsTheOldestMessage) {
    FakeCaller caller;
    ReliableSender sender(caller);
    sender.enqueue("A", json::object(), nullptr);
    sender.enqueue("B", json::object(), nullptr);
    sender.set_online(true);

    std::vector<std::string> expected = { "A" };
    EXPECT_TRUE(caller.actions() == expected);  // B waits: one at a time
}

TEST(ReliableSender, MessagesGoOutStrictlyInOrderOneAtATime) {
    FakeCaller caller;
    ReliableSender sender(caller);
    sender.set_online(true);
    sender.enqueue("A", json::object(), nullptr);
    sender.enqueue("B", json::object(), nullptr);
    sender.enqueue("C", json::object(), nullptr);
    ASSERT_EQ(caller.sent.size(), 1u);

    caller.sent[0].done(outcome(kResult));
    ASSERT_EQ(caller.sent.size(), 2u);
    caller.sent[1].done(outcome(kResult));
    ASSERT_EQ(caller.sent.size(), 3u);

    std::vector<std::string> expected = { "A", "B", "C" };
    EXPECT_TRUE(caller.actions() == expected);
    caller.sent[2].done(outcome(kResult));
    EXPECT_EQ(sender.pending(), 0u);
}

TEST(ReliableSender, TheCallbackRunsOnceWithTheResult) {
    FakeCaller caller;
    ReliableSender sender(caller);
    sender.set_online(true);
    int calls = 0;
    CallOutcome seen;
    sender.enqueue("A", json::object(), [&](const CallOutcome& o) {
        ++calls;
        seen = o;
        });
    caller.sent[0].done(outcome(kResult));
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(seen.status == kResult);
    EXPECT_EQ(sender.pending(), 0u);
}

TEST(ReliableSender, DisconnectKeepsTheMessageAndDoesNotSpin) {
    FakeCaller caller;
    ReliableSender sender(caller);
    sender.set_online(true);
    int calls = 0;
    sender.enqueue("A", json::object(), [&](const CallOutcome&) { ++calls; });
    ASSERT_EQ(caller.sent.size(), 1u);

    caller.sent[0].done(outcome(kDisconnected));
    EXPECT_EQ(caller.sent.size(), 1u);  // no immediate retry: that would be a hot loop
    EXPECT_EQ(sender.pending(), 1u);
    EXPECT_FALSE(sender.online());
    EXPECT_EQ(calls, 0);                // the caller is not told yet

    sender.set_online(true);            // reconnected
    ASSERT_EQ(caller.sent.size(), 2u);  // the very same message goes out again
    EXPECT_EQ(caller.sent[1].action, "A");
    caller.sent[1].done(outcome(kResult));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(sender.pending(), 0u);
}

TEST(ReliableSender, OrderSurvivesAnOutage) {
    FakeCaller caller;
    ReliableSender sender(caller);
    sender.set_online(true);
    sender.enqueue("A", json::object(), nullptr);
    sender.enqueue("B", json::object(), nullptr);
    sender.enqueue("C", json::object(), nullptr);

    caller.sent[0].done(outcome(kResult));        // A delivered, B in flight
    caller.sent[1].done(outcome(kDisconnected));  // link dies: B must go again FIRST
    sender.set_online(true);

    std::vector<std::string> expected = { "A", "B", "B" };
    EXPECT_TRUE(caller.actions() == expected);
    caller.sent[2].done(outcome(kResult));
    EXPECT_EQ(caller.sent.back().action, "C");    // then C
}

TEST(ReliableSender, TimeoutsAreRetriedThenTheMessageIsDropped) {
    FakeCaller caller;
    ReliableSenderOptions options;
    options.max_attempts = 3;
    ReliableSender sender(caller, options);
    sender.set_online(true);

    int calls = 0;
    CallOutcome seen;
    sender.enqueue("A", json::object(), [&](const CallOutcome& o) {
        ++calls;
        seen = o;
        });
    sender.enqueue("B", json::object(), nullptr);

    caller.sent[0].done(outcome(kTimeout));
    EXPECT_EQ(calls, 0);
    caller.sent[1].done(outcome(kTimeout));
    EXPECT_EQ(calls, 0);
    caller.sent[2].done(outcome(kTimeout));  // the third failure: give up

    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(seen.status == kTimeout);
    std::vector<std::string> expected = { "A", "A", "A", "B" };  // B is not blocked forever
    EXPECT_TRUE(caller.actions() == expected);
}

TEST(ReliableSender, RejectedMessagesAreRetriedThenDropped) {
    FakeCaller caller;
    ReliableSenderOptions options;
    options.max_attempts = 2;
    ReliableSender sender(caller, options);
    sender.set_online(true);

    CallOutcome seen;
    sender.enqueue("A", json::object(), [&](const CallOutcome& o) { seen = o; });
    caller.sent[0].done(outcome(kError));
    caller.sent[1].done(outcome(kError));

    EXPECT_TRUE(seen.status == kError);
    EXPECT_EQ(seen.error_code, "InternalError");
    EXPECT_EQ(sender.pending(), 0u);
}

TEST(ReliableSender, SuccessAfterARetryCountsAsSuccess) {
    FakeCaller caller;
    ReliableSender sender(caller);
    sender.set_online(true);
    CallOutcome seen;
    sender.enqueue("A", json::object(), [&](const CallOutcome& o) { seen = o; });
    caller.sent[0].done(outcome(kTimeout));
    caller.sent[1].done(outcome(kResult));
    EXPECT_TRUE(seen.status == kResult);
    EXPECT_EQ(sender.pending(), 0u);
}

TEST(ReliableSender, ACallerThatIsFullRefusesNewMessages) {
    FakeCaller caller;
    ReliableSenderOptions options;
    options.capacity = 2;
    ReliableSender sender(caller, options);  // offline: messages just pile up

    int calls = 0;
    EXPECT_TRUE(sender.enqueue("A", json::object(), nullptr));
    EXPECT_TRUE(sender.enqueue("B", json::object(), nullptr));
    EXPECT_FALSE(sender.enqueue("C", json::object(), [&](const CallOutcome&) { ++calls; }));
    EXPECT_EQ(sender.pending(), 2u);
    EXPECT_EQ(calls, 0);  // a refused message's callback is not called
}

TEST(ReliableSender, SynchronousAnswersDoNotRecurseDeeply) {
    SyncCaller caller;
    ReliableSenderOptions options;
    options.capacity = 20000;
    ReliableSender sender(caller, options);
    for (int i = 0; i < 20000; ++i) {
        ASSERT_TRUE(sender.enqueue("A", json::object(), nullptr));
    }
    sender.set_online(true);  // would overflow the stack if every answer recursed
    EXPECT_EQ(caller.calls, 20000);
    EXPECT_EQ(sender.pending(), 0u);
}

TEST(ReliableSender, ACallbackMayEnqueueAnotherMessage) {
    FakeCaller caller;
    ReliableSender sender(caller);
    sender.set_online(true);
    sender.enqueue("A", json::object(), [&](const CallOutcome&) {
        sender.enqueue("D", json::object(), nullptr);
        });
    sender.enqueue("B", json::object(), nullptr);

    caller.sent[0].done(outcome(kResult));  // A done; its callback queues D behind B
    ASSERT_EQ(caller.sent.size(), 2u);
    EXPECT_EQ(caller.sent[1].action, "B");
    caller.sent[1].done(outcome(kResult));
    ASSERT_EQ(caller.sent.size(), 3u);
    EXPECT_EQ(caller.sent[2].action, "D");
}

TEST(ReliableSender, SettingOnlineTwiceDoesNotSendTwice) {
    FakeCaller caller;
    ReliableSender sender(caller);
    sender.enqueue("A", json::object(), nullptr);
    sender.set_online(true);
    sender.set_online(true);
    EXPECT_EQ(caller.sent.size(), 1u);
}

TEST(ReliableSender, ALateAnswerAfterTheSenderIsGoneIsHarmless) {
    FakeCaller caller;
    {
        ReliableSender sender(caller);
        sender.set_online(true);
        sender.enqueue("A", json::object(), nullptr);
    }  // destroyed with a message in flight
    ASSERT_EQ(caller.sent.size(), 1u);
    caller.sent[0].done(outcome(kResult));  // must not touch the dead sender
    EXPECT_EQ(caller.sent.size(), 1u);
}

TEST(ReliableSender, NullCallbacksAreAllowed) {
    FakeCaller caller;
    ReliableSenderOptions options;
    options.max_attempts = 1;
    ReliableSender sender(caller, options);
    sender.set_online(true);
    sender.enqueue("A", json::object(), nullptr);
    sender.enqueue("B", json::object(), nullptr);
    caller.sent[0].done(outcome(kError));   // dropped: no callback, no crash
    caller.sent[1].done(outcome(kResult));
    EXPECT_EQ(sender.pending(), 0u);
}