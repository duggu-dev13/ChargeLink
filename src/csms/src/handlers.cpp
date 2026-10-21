#include "chargelink/csms/handlers.hpp"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "chargelink/csms/state.hpp"
#include "chargelink/ocpp/messages.hpp"
#include "chargelink/util/time.hpp"

namespace chargelink::csms {

    namespace {

        struct Shared {
            ChargerStates chargers;
            TransactionStore transactions;
        };

        // OCPP: nothing but BootNotification is allowed before the charger has booted.
        void require_booted(const Shared& shared, const std::string& cp_id, const char* action) {
            if (!shared.chargers.is_booted(cp_id)) {
                spdlog::warn("[{}] {} before BootNotification: refused", cp_id, action);
                throw ocpp::CallException("SecurityError", "BootNotification required first");
            }
        }

        // "1840" -> 1840. Rejects anything that is not a plain non-negative integer.
        bool parse_wh(const std::string& text, int& out) {
            if (text.empty() || text.size() > 9) return false;
            for (char c : text) {
                if (c < '0' || c > '9') return false;
            }
            out = std::atoi(text.c_str());
            return true;
        }

    }  // namespace

    ocpp::Dispatcher make_dispatcher() {
        using namespace chargelink::ocpp;
        Dispatcher dispatcher;
        auto shared = std::make_shared<Shared>();

        dispatcher.on<BootNotificationRequest, BootNotificationResponse>(
            [shared](const std::string& cp_id, const BootNotificationRequest& request) {
                shared->chargers.mark_booted(cp_id);
                spdlog::info("[{}] BootNotification from {} {}", cp_id, request.charge_point_vendor,
                    request.charge_point_model);
                BootNotificationResponse response;
                response.status = RegistrationStatus::Accepted;
                response.current_time = chargelink::util::now_utc_iso8601();
                response.interval = 5;  // short on purpose, so heartbeats are easy to watch
                return response;
            });

        dispatcher.on<HeartbeatRequest, HeartbeatResponse>(
            [shared](const std::string& cp_id, const HeartbeatRequest&) {
                require_booted(*shared, cp_id, "Heartbeat");
                spdlog::debug("[{}] Heartbeat", cp_id);
                HeartbeatResponse response;
                response.current_time = chargelink::util::now_utc_iso8601();
                return response;
            });

        dispatcher.on<StatusNotificationRequest, StatusNotificationResponse>(
            [shared](const std::string& cp_id, const StatusNotificationRequest& request) {
                require_booted(*shared, cp_id, "StatusNotification");
                spdlog::info("[{}] StatusNotification {}", cp_id, json(request).dump());
                return StatusNotificationResponse{};
            });

        // Tags starting with BAD are refused.
        dispatcher.on<AuthorizeRequest, AuthorizeResponse>(
            [shared](const std::string& cp_id, const AuthorizeRequest& request) {
                require_booted(*shared, cp_id, "Authorize");
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
            [shared](const std::string& cp_id, const StartTransactionRequest& request) {
                require_booted(*shared, cp_id, "StartTransaction");
                const auto started = shared->transactions.start(
                    cp_id, request.connector_id, request.id_tag, request.meter_start,
                    request.timestamp);
                StartTransactionResponse response;
                response.transaction_id = started.id;
                const bool refuse = request.id_tag.rfind("LATE", 0) == 0;
                response.id_tag_info.status =
                    refuse ? AuthorizationStatus::Blocked : AuthorizationStatus::Accepted;
                spdlog::info("[{}] StartTransaction {} meterStart={} -> id {} ({}){}", cp_id,
                    request.id_tag, request.meter_start, started.id,
                    refuse ? "Blocked" : "Accepted",
                    started.duplicate ? " [retry: same id as before]" : "");
                return response;
            });

        dispatcher.on<StopTransactionRequest, StopTransactionResponse>(
            [shared](const std::string& cp_id, const StopTransactionRequest& request) {
                require_booted(*shared, cp_id, "StopTransaction");
                using Stop = TransactionStore::StopResult;
                const Stop result = shared->transactions.stop(
                    cp_id, request.transaction_id, request.meter_stop, request.timestamp);
                switch (result) {
                case Stop::Stopped:
                    spdlog::info("[{}] StopTransaction id {} meterStop={} Wh: {}", cp_id,
                        request.transaction_id, request.meter_stop,
                        json(request).dump());
                    break;
                case Stop::Duplicate:
                    spdlog::info("[{}] StopTransaction id {} again: already stopped", cp_id,
                        request.transaction_id);
                    break;  // a retry: answer normally so the charger can drop it from its queue
                case Stop::Unknown:
                case Stop::WrongCharger:  // do not reveal which one it was
                    spdlog::warn("[{}] StopTransaction for unknown transaction {}", cp_id,
                        request.transaction_id);
                    throw CallException("PropertyConstraintViolation", "unknown transactionId");
                case Stop::MeterDecreased:
                    spdlog::warn("[{}] StopTransaction id {}: meterStop {} is below meterStart",
                        cp_id, request.transaction_id, request.meter_stop);
                    throw CallException("PropertyConstraintViolation", "meterStop below meterStart");
                }
                return StopTransactionResponse{};
            });

        dispatcher.on<MeterValuesRequest, MeterValuesResponse>(
            [shared](const std::string& cp_id, const MeterValuesRequest& request) {
                require_booted(*shared, cp_id, "MeterValues");
                for (const auto& value : request.meter_value) {
                    for (const auto& sample : value.sampled_value) {
                        spdlog::info("[{}]   transaction {}: {} = {} {}", cp_id,
                            request.transaction_id.value_or(-1),
                            sample.measurand.value_or("?"), sample.value,
                            sample.unit.value_or(""));
                        int wh = 0;
                        if (request.transaction_id &&
                            sample.measurand.value_or("") == "Energy.Active.Import.Register" &&
                            parse_wh(sample.value, wh) &&
                            !shared->transactions.record_meter(cp_id, *request.transaction_id, wh)) {
                            spdlog::warn("[{}] meter value for unknown or ended transaction {}", cp_id,
                                *request.transaction_id);
                        }
                    }
                }
                return MeterValuesResponse{};
            });

        return dispatcher;
    }

}  // namespace chargelink::csms