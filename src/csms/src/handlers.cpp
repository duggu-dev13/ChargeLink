#include "chargelink/csms/handlers.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <string>

#include "chargelink/ocpp/messages.hpp"
#include "chargelink/util/time.hpp"

namespace chargelink::csms {

    ocpp::Dispatcher make_dispatcher() {
        using namespace chargelink::ocpp;
        Dispatcher dispatcher;

        auto next_transaction_id = std::make_shared<std::atomic<int>>(1000);  // shared by all threads

        dispatcher.on<BootNotificationRequest, BootNotificationResponse>(
            [](const std::string& cp_id, const BootNotificationRequest& request) {
                spdlog::info("[{}] BootNotification from {} {}", cp_id, request.charge_point_vendor,
                    request.charge_point_model);
                BootNotificationResponse response;
                response.status = RegistrationStatus::Accepted;
                response.current_time = chargelink::util::now_utc_iso8601();
                response.interval = 5;  // short on purpose, so heartbeats are easy to watch
                return response;
            });

        dispatcher.on<HeartbeatRequest, HeartbeatResponse>(
            [](const std::string& cp_id, const HeartbeatRequest&) {
                spdlog::debug("[{}] Heartbeat", cp_id);
                HeartbeatResponse response;
                response.current_time = chargelink::util::now_utc_iso8601();
                return response;
            });

        dispatcher.on<StatusNotificationRequest, StatusNotificationResponse>(
            [](const std::string& cp_id, const StatusNotificationRequest& request) {
                spdlog::info("[{}] StatusNotification {}", cp_id, json(request).dump());
                return StatusNotificationResponse{};
            });

        // Tags starting with BAD are refused.
        dispatcher.on<AuthorizeRequest, AuthorizeResponse>(
            [](const std::string& cp_id, const AuthorizeRequest& request) {
                AuthorizeResponse response;
                const bool bad = request.id_tag.rfind("BAD", 0) == 0;
                response.id_tag_info.status =
                    bad ? AuthorizationStatus::Blocked : AuthorizationStatus::Accepted;
                spdlog::info("[{}] Authorize {} -> {}", cp_id, request.id_tag,
                    bad ? "Blocked" : "Accepted");
                return response;
            });

        // Tags starting with LATE pass Authorize but are refused here (tests DeAuthorized).
        dispatcher.on<StartTransactionRequest, StartTransactionResponse>(
            [next_transaction_id](const std::string& cp_id, const StartTransactionRequest& request) {
                StartTransactionResponse response;
                response.transaction_id = (*next_transaction_id)++;
                const bool refuse = request.id_tag.rfind("LATE", 0) == 0;
                response.id_tag_info.status =
                    refuse ? AuthorizationStatus::Blocked : AuthorizationStatus::Accepted;
                spdlog::info("[{}] StartTransaction {} meterStart={} -> id {} ({})", cp_id,
                    request.id_tag, request.meter_start, response.transaction_id,
                    refuse ? "Blocked" : "Accepted");
                return response;
            });

        dispatcher.on<StopTransactionRequest, StopTransactionResponse>(
            [](const std::string& cp_id, const StopTransactionRequest& request) {
                spdlog::info("[{}] StopTransaction id {} meterStop={} Wh: {}", cp_id,
                    request.transaction_id, request.meter_stop, json(request).dump());
                return StopTransactionResponse{};
            });

        dispatcher.on<MeterValuesRequest, MeterValuesResponse>(
            [](const std::string& cp_id, const MeterValuesRequest& request) {
                for (const auto& value : request.meter_value) {
                    for (const auto& sample : value.sampled_value) {
                        spdlog::info("[{}]   transaction {}: {} = {} {}", cp_id,
                            request.transaction_id.value_or(-1),
                            sample.measurand.value_or("?"), sample.value,
                            sample.unit.value_or(""));
                    }
                }
                return MeterValuesResponse{};
            });

        return dispatcher;
    }

}  // namespace chargelink::csms