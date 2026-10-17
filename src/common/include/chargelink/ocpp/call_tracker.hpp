#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>

#include "chargelink/ocpp/frame.hpp"

namespace chargelink::ocpp {

    // What happened to a CALL we sent.
    struct CallOutcome {
        enum class Status {
            Result,        // the peer answered with a CALLRESULT
            Error,         // the peer answered with a CALLERROR
            Timeout,       // no answer in time
            Disconnected,  // the connection went away first
        };

        Status status = Status::Result;
        json payload;                   // valid for Result
        std::string error_code;         // valid for Error
        std::string error_description;  // valid for Error
    };

    const char* to_string(CallOutcome::Status status);

    // Remembers the CALLs we have sent and are still waiting for an answer to.
    //
    // Pure logic: no sockets, no timers, no clock of its own. The caller passes "now" in,
    // so tests can move time by hand. Not thread-safe: use it from one thread or strand.
    class CallTracker {
    public:
        using Clock = std::chrono::steady_clock;
        using Callback = std::function<void(const CallOutcome&)>;

        // Registers a pending call and returns the unique id to put in the CALL frame.
        std::string add(Clock::time_point now, std::chrono::milliseconds timeout, Callback on_outcome);

        // Feed in a received frame. Returns true if it answered one of our pending calls.
        // CALL frames and unknown or already-expired ids return false.
        bool resolve(const Frame& reply);

        // Fails every call whose deadline has passed (deadline <= now). Returns how many.
        std::size_t expire(Clock::time_point now);

        // Fails every pending call at once (used when the connection drops).
        void fail_all(CallOutcome::Status status);

        std::size_t pending() const { return pending_.size(); }

    private:
        struct Pending {
            Clock::time_point deadline;
            Callback callback;
        };

        std::unordered_map<std::string, Pending> pending_;
        unsigned long long next_id_ = 1;
    };

}  // namespace chargelink::ocpp