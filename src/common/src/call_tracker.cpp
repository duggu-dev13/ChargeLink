#include "chargelink/ocpp/call_tracker.hpp"

#include <utility>
#include <variant>
#include <vector>

namespace chargelink::ocpp {

    const char* to_string(CallOutcome::Status status) {
        switch (status) {
        case CallOutcome::Status::Result:       return "Result";
        case CallOutcome::Status::Error:        return "Error";
        case CallOutcome::Status::Timeout:      return "Timeout";
        case CallOutcome::Status::Disconnected: return "Disconnected";
        }
        return "?";
    }

    std::string CallTracker::add(Clock::time_point now, std::chrono::milliseconds timeout,
        Callback on_outcome) {
        std::string id = std::to_string(next_id_++);
        pending_.emplace(id, Pending{ now + timeout, std::move(on_outcome) });
        return id;
    }

    bool CallTracker::resolve(const Frame& reply) {
        const std::string* id = nullptr;
        CallOutcome outcome;

        if (const auto* result = std::get_if<CallResult>(&reply)) {
            id = &result->unique_id;
            outcome.status = CallOutcome::Status::Result;
            outcome.payload = result->payload;
        }
        else if (const auto* error = std::get_if<CallError>(&reply)) {
            id = &error->unique_id;
            outcome.status = CallOutcome::Status::Error;
            outcome.error_code = error->error_code;
            outcome.error_description = error->error_description;
        }
        else {
            return false;  // a CALL is a request, not an answer
        }

        auto it = pending_.find(*id);
        if (it == pending_.end()) {
            return false;  // unknown, or it already timed out
        }

        // Update our own state BEFORE running user code, so a callback may call add() safely.
        Callback callback = std::move(it->second.callback);
        pending_.erase(it);
        if (callback) {
            callback(outcome);
        }
        return true;
    }

    std::size_t CallTracker::expire(Clock::time_point now) {
        std::vector<Callback> expired;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.deadline <= now) {
                expired.push_back(std::move(it->second.callback));
                it = pending_.erase(it);
            }
            else {
                ++it;
            }
        }

        CallOutcome outcome;
        outcome.status = CallOutcome::Status::Timeout;
        for (auto& callback : expired) {
            if (callback) {
                callback(outcome);
            }
        }
        return expired.size();
    }

    void CallTracker::fail_all(CallOutcome::Status status) {
        std::unordered_map<std::string, Pending> taken;
        taken.swap(pending_);  // our map is empty before any callback runs

        CallOutcome outcome;
        outcome.status = status;
        for (auto& entry : taken) {
            if (entry.second.callback) {
                entry.second.callback(outcome);
            }
        }
    }

}  // namespace chargelink::ocpp