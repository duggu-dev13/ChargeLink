#include "chargelink/ocpp/frame.hpp"

namespace chargelink::ocpp {

    namespace {

        // Helper that lets std::visit take several lambdas, one per variant type.
        template <class... Ts>
        struct Overloaded : Ts... {
            using Ts::operator()...;
        };
        template <class... Ts>
        Overloaded(Ts...) -> Overloaded<Ts...>;

        void require(bool condition, const char* message) {
            if (!condition) {
                throw ParseError(message);
            }
        }

    }  // namespace

    std::string serialize(const Frame& frame) {
        json j = std::visit(
            Overloaded{
                [](const Call& c) {
                    return json::array({ 2, c.unique_id, c.action, c.payload });
                },
                [](const CallResult& r) {
                    return json::array({ 3, r.unique_id, r.payload });
                },
                [](const CallError& e) {
                    return json::array({ 4, e.unique_id, e.error_code,
                                        e.error_description, e.error_details });
                } },
            frame);
        return j.dump();
    }

    Frame parse(const std::string& text) {
        // 3rd argument false = don't throw; returns a "discarded" value instead.
        json j = json::parse(text, nullptr, false);
        require(!j.is_discarded(), "not valid JSON");
        require(j.is_array(), "frame must be a JSON array");
        require(j.size() >= 3, "frame has too few elements");
        require(j[0].is_number_integer(), "message type must be an integer");
        require(j[1].is_string(), "unique id must be a string");

        const std::string id = j[1].get<std::string>();

        switch (j[0].get<int>()) {
        case 2: {  // CALL
            require(j.size() == 4, "CALL must have 4 elements");
            require(j[2].is_string(), "CALL action must be a string");
            require(j[3].is_object(), "CALL payload must be an object");
            return Call{ id, j[2].get<std::string>(), j[3] };
        }
        case 3: {  // CALLRESULT
            require(j.size() == 3, "CALLRESULT must have 3 elements");
            require(j[2].is_object(), "CALLRESULT payload must be an object");
            return CallResult{ id, j[2] };
        }
        case 4: {  // CALLERROR
            require(j.size() == 5, "CALLERROR must have 5 elements");
            require(j[2].is_string(), "CALLERROR code must be a string");
            require(j[3].is_string(), "CALLERROR description must be a string");
            require(j[4].is_object(), "CALLERROR details must be an object");
            return CallError{ id, j[2].get<std::string>(), j[3].get<std::string>(), j[4] };
        }
        default:
            throw ParseError("unknown message type");
        }
    }

}  // namespace chargelink::ocpp