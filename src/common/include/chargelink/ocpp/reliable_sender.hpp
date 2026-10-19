#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <string>

#include "chargelink/ocpp/call_tracker.hpp"
#include "chargelink/ocpp/caller.hpp"
#include "chargelink/ocpp/frame.hpp"

namespace chargelink::ocpp {

    struct ReliableSenderOptions {
        std::size_t capacity = 1000;  // how many messages may wait; more are refused
        unsigned max_attempts = 5;    // a message that keeps timing out or being rejected is dropped
    };

    // A first-in-first-out outbox for messages that must not be lost (StartTransaction,
    // StopTransaction).
    //
    //  - Messages are sent one at a time, in order, only while online.
    //  - A message stays queued until the peer answers with a CALLRESULT.
    //  - Delivery is at-least-once: a message may be sent again after a lost reply.
    //
    // Pure logic on top of ICaller: no sockets, no timers. Not thread-safe: use it from the
    // strand that owns the connection. The queue lives in memory only.
    class ReliableSender {
    public:
        using Callback = CallTracker::Callback;

        explicit ReliableSender(ICaller& caller, ReliableSenderOptions options = {});

        ReliableSender(const ReliableSender&) = delete;
        ReliableSender& operator=(const ReliableSender&) = delete;

        // Queues a message. Returns false (and does NOT call on_done) if the queue is full.
        // on_done runs once: with the Result, or with the last failure if we gave up.
        bool enqueue(std::string action, json payload, Callback on_done);

        // Call with true once we are registered with the CSMS, false when the link is lost.
        void set_online(bool online);

        bool online() const { return online_; }
        std::size_t pending() const { return queue_.size(); }

    private:
        struct Entry {
            std::string action;
            json payload;
            Callback callback;
            unsigned attempts = 0;
        };

        void pump();
        void on_outcome(const CallOutcome& outcome);
        void finish_head(const CallOutcome& outcome);

        ICaller& caller_;
        ReliableSenderOptions options_;
        std::deque<Entry> queue_;
        bool online_ = false;
        bool in_flight_ = false;  // the head has been sent and not answered yet
        bool pumping_ = false;    // guards against recursion when a caller answers synchronously

        // Replies capture a weak_ptr to this token: if the sender is gone, a late reply is ignored.
        std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    };

}  // namespace chargelink::ocpp