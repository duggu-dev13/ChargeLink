#pragma once

#include <string>

#include "chargelink/ocpp/call_tracker.hpp"
#include "chargelink/ocpp/frame.hpp"

namespace chargelink::ocpp {

    // "Something that can send a CALL and tell us what happened to it."
    // The WebSocket client implements it; tests use a fake.
    class ICaller {
    public:
        virtual ~ICaller() = default;
        virtual void call(std::string action, json payload, CallTracker::Callback on_reply) = 0;
    };

}  // namespace chargelink::ocpp