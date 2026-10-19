#include "chargelink/ocpp/reliable_sender.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace chargelink::ocpp {

    ReliableSender::ReliableSender(ICaller& caller, ReliableSenderOptions options)
        : caller_(caller), options_(options) {
    }

    bool ReliableSender::enqueue(std::string action, json payload, Callback on_done) {
        if (queue_.size() >= options_.capacity) {
            spdlog::error("outbox full ({} messages): refusing {}", queue_.size(), action);
            return false;
        }
        queue_.push_back(Entry{ std::move(action), std::move(payload), std::move(on_done), 0 });
        pump();
        return true;
    }

    void ReliableSender::set_online(bool online) {
        online_ = online;
        if (online_) {
            pump();
        }
    }

    // Sends the head if we can. A loop instead of recursion: if the caller answers
    // synchronously, on_outcome() runs inside caller_.call() and the loop simply continues.
    void ReliableSender::pump() {
        if (pumping_) {
            return;
        }
        pumping_ = true;
        while (online_ && !in_flight_ && !queue_.empty()) {
            in_flight_ = true;
            std::string action = queue_.front().action;  // copies: the entry stays queued
            json payload = queue_.front().payload;

            std::weak_ptr<bool> alive = alive_;
            caller_.call(std::move(action), std::move(payload),
                [this, alive](const CallOutcome& outcome) {
                    if (alive.expired()) {
                        return;  // the sender is gone
                    }
                    on_outcome(outcome);
                });
        }
        pumping_ = false;
    }

    void ReliableSender::on_outcome(const CallOutcome& outcome) {
        in_flight_ = false;
        if (queue_.empty()) {
            return;  // cannot happen, but never touch an empty queue
        }

        switch (outcome.status) {
        case CallOutcome::Status::Result:
            finish_head(outcome);
            break;

        case CallOutcome::Status::Disconnected:
            // Keep the message and wait for set_online(true). Retrying now would just
            // fail again, in a hot loop.
            online_ = false;
            break;

        case CallOutcome::Status::Timeout:
        case CallOutcome::Status::Error: {
            Entry& head = queue_.front();
            ++head.attempts;
            if (head.attempts >= options_.max_attempts) {
                spdlog::warn("giving up on {} after {} attempts ({})", head.action, head.attempts,
                    to_string(outcome.status));
                finish_head(outcome);
            }
            break;  // otherwise pump() below sends it again
        }
        }
        pump();
    }

    // Updates the queue BEFORE running user code, so a callback may enqueue safely.
    void ReliableSender::finish_head(const CallOutcome& outcome) {
        Callback callback = std::move(queue_.front().callback);
        queue_.pop_front();
        if (callback) {
            callback(outcome);
        }
    }

}  // namespace chargelink::ocpp