#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <variant>

namespace chargelink::ocpp {

    using json = nlohmann::json;

    // Thrown when text is not a valid OCPP frame.
    class ParseError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // [2, "<id>", "<Action>", {payload}]
    struct Call {
        std::string unique_id;
        std::string action;
        json payload;
    };

    // [3, "<id>", {payload}]
    struct CallResult {
        std::string unique_id;
        json payload;
    };

    // [4, "<id>", "<errorCode>", "<errorDescription>", {errorDetails}]
    struct CallError {
        std::string unique_id;
        std::string error_code;
        std::string error_description;
        json error_details;
    };

    // A frame is exactly one of the three.
    using Frame = std::variant<Call, CallResult, CallError>;

    // Frame -> JSON text.
    std::string serialize(const Frame& frame);

    // JSON text -> Frame. Throws ParseError on any malformed input.
    Frame parse(const std::string& text);

}  // namespace chargelink::ocpp