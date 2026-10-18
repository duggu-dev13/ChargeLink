#include "chargelink/sim/ocpp_link.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace chargelink::sim {

    using ocpp::CallOutcome;

    namespace {

        // A typed view of a reply, or nullopt (plus a log line) when the call failed or the
        // reply cannot be read.
        template <class Response>
        std::optional<Response> read_reply(const char* action, const CallOutcome& outcome) {
            if (outcome.status != CallOutcome::Status::Result) {
                spdlog::warn("{} failed: {} {}", action, ocpp::to_string(outcome.status),
                    outcome.error_code);
                return std::nullopt;
            }
            try {
                return ocpp::payload_as<Response>(outcome.payload);
            }
            catch (const ocpp::ParseError& e) {
                spdlog::warn("{}: unreadable reply: {}", action, e.what());
                return std::nullopt;
            }
        }

        // For messages whose answer carries nothing we need: just log failures.
        ocpp::CallTracker::Callback log_failures(const char* action) {
            return [action](const CallOutcome& outcome) {
                if (outcome.status != CallOutcome::Status::Result) {
                    spdlog::warn("{} failed: {} {}", action, ocpp::to_string(outcome.status),
                        outcome.error_code);
                }
                };
        }

    }  // namespace

    void OcppLink::send_status(const ocpp::StatusNotificationRequest& request) {
        const char* action = ocpp::StatusNotificationRequest::kAction;
        caller_.call(action, ocpp::json(request), log_failures(action));
    }

    void OcppLink::authorize(const std::string& id_tag,
        std::function<void(std::optional<ocpp::AuthorizationStatus>)> on_result) {
        ocpp::AuthorizeRequest request;
        request.id_tag = id_tag;

        const char* action = ocpp::AuthorizeRequest::kAction;
        caller_.call(action, ocpp::json(request),
            [action, on_result = std::move(on_result)](const CallOutcome& outcome) {
                const auto response = read_reply<ocpp::AuthorizeResponse>(action, outcome);
                if (response) {
                    on_result(response->id_tag_info.status);
                }
                else {
                    on_result(std::nullopt);
                }
            });
    }

    void OcppLink::start_transaction(
        const ocpp::StartTransactionRequest& request,
        std::function<void(std::optional<ocpp::StartTransactionResponse>)> on_result) {
        const char* action = ocpp::StartTransactionRequest::kAction;
        caller_.call(action, ocpp::json(request),
            [action, on_result = std::move(on_result)](const CallOutcome& outcome) {
                on_result(read_reply<ocpp::StartTransactionResponse>(action, outcome));
            });
    }

    // StopTransaction and MeterValues are fire-and-forget for now. A real charger would keep
    // them in a queue and retry; that is the offline queue, a later step.
    void OcppLink::stop_transaction(const ocpp::StopTransactionRequest& request) {
        const char* action = ocpp::StopTransactionRequest::kAction;
        caller_.call(action, ocpp::json(request), log_failures(action));
    }

    void OcppLink::send_meter_values(const ocpp::MeterValuesRequest& request) {
        const char* action = ocpp::MeterValuesRequest::kAction;
        caller_.call(action, ocpp::json(request), log_failures(action));
    }

}  // namespace chargelink::sim