#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "chargelink/ocpp/messages.hpp"

namespace chargelink::ocpp {

    class Dispatcher {
    public:
        // Untyped handler: charge point id + payload in, response payload out.
        // Throw ParseError for an invalid payload; any other exception is an internal error.
        using RawHandler =
            std::function<json(const std::string& charge_point_id, const json& payload)>;

        void register_raw(const std::string& action, RawHandler handler);

        // Typed registration. Call with explicit types:
        //   dispatcher.on<HeartbeatRequest, HeartbeatResponse>(my_lambda);
        template <class Req, class Res>
        void on(std::function<Res(const std::string&, const Req&)> handler) {
            register_raw(Req::kAction,
                [handler = std::move(handler)](const std::string& charge_point_id,
                    const json& payload) -> json {
                        Req request = payload_as<Req>(payload);  // validates the payload
                        Res response = handler(charge_point_id, request);
                        return json(response);
                });
        }

        // Returns the reply text to send back, or nullopt if no reply is due.
        // Safe to call from several threads as long as nobody registers handlers meanwhile.
        std::optional<std::string> handle(const std::string& charge_point_id,
            const std::string& text) const;

    private:
        std::unordered_map<std::string, RawHandler> handlers_;
    };

}  // namespace chargelink::ocpp