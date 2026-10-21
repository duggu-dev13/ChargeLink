#include "chargelink/ocpp/dispatcher.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <utility>

namespace chargelink::ocpp {

    namespace {

        std::string error_reply(const std::string& unique_id, const char* code,
            const std::string& description) {
            return serialize(CallError{ unique_id, code, description, json::object() });
        }

    }  // namespace

    void Dispatcher::register_raw(const std::string& action, RawHandler handler) {
        handlers_[action] = std::move(handler);
    }

    std::optional<std::string> Dispatcher::handle(const std::string& charge_point_id,
        const std::string& text) const {
        // 1. Parse. Garbage has no usable unique id, so there is nobody to answer.
        Frame frame;
        try {
            frame = parse(text);
        }
        catch (const ParseError& e) {
            spdlog::warn("[{}] dropping unparseable frame: {}", charge_point_id, e.what());
            return std::nullopt;
        }

        // 2. Only CALLs get dispatched. (Responses to our own commands come in Phase 6.)
        const Call* call = std::get_if<Call>(&frame);
        if (call == nullptr) {
            spdlog::info("[{}] ignoring non-CALL frame", charge_point_id);
            return std::nullopt;
        }

        // 3. Find the handler.
        auto it = handlers_.find(call->action);
        if (it == handlers_.end()) {
            spdlog::warn("[{}] no handler for action '{}'", charge_point_id, call->action);
            return error_reply(call->unique_id, "NotImplemented", "Action not implemented");
        }

        // 4. Run it and translate the outcome.
        try {
            json result = it->second(charge_point_id, call->payload);
            return serialize(CallResult{ call->unique_id, std::move(result) });
        }
        catch (const CallException& e) {  // a handler chose a specific OCPP error code
            spdlog::warn("[{}] {} refused: {} ({})", charge_point_id, call->action, e.code(),
                e.what());
            return error_reply(call->unique_id, e.code().c_str(), e.what());
        }
        catch (const ParseError& e) {
            spdlog::warn("[{}] bad {} payload: {}", charge_point_id, call->action, e.what());
            return error_reply(call->unique_id, "FormationViolation", e.what());
        }
        catch (const std::exception& e) {
            spdlog::error("[{}] handler for {} failed: {}", charge_point_id, call->action, e.what());
            return error_reply(call->unique_id, "InternalError", "");
        }
    }

}  // namespace chargelink::ocpp