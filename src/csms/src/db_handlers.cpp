#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS  // getenv
#endif

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>

#include "chargelink/csms/handlers.hpp"
#include "chargelink/csms/state.hpp"
#include "chargelink/db/repository.hpp"
#include "chargelink/ocpp/messages.hpp"
#include "chargelink/util/time.hpp"

namespace chargelink::csms {

    namespace {

        struct DbShared {
            ChargerStates chargers;  // the boot-first rule stays in memory (per process)
            std::shared_ptr<db::Repository> repo;
        };

        // OCPP: nothing but BootNotification is allowed before the charger has booted.
        void require_booted(const DbShared& shared, const std::string& cp_id, const char* action) {
            if (!shared.chargers.is_booted(cp_id)) {
                spdlog::warn("[{}] {} before BootNotification: refused", cp_id, action);
                throw ocpp::CallException("SecurityError", "BootNotification required first");
            }
        }

        // Runs a database call; a value PostgreSQL rejects becomes a FormationViolation.
        // Any other failure propagates and the dispatcher answers InternalError.
        template <class F>
        auto guarded(const std::string& cp_id, const char* action, F&& f) {
            try {
                return f();
            }
            catch (const db::BadInputError&) {
                spdlog::warn("[{}] {}: the database rejected a value (bad timestamp or number)", cp_id,
                    action);
                throw ocpp::CallException("FormationViolation", "invalid timestamp or value");
            }
        }

        // Only "Accepted" is accepted. Expired / Invalid / Blocked all become Blocked for now.
        ocpp::AuthorizationStatus to_auth(const std::string& db_status) {
            return db_status == "Accepted" ? ocpp::AuthorizationStatus::Accepted
                : ocpp::AuthorizationStatus::Blocked;
        }

        bool parse_number(const std::string& text, double& out) {
            if (text.empty() || text.size() > 32) return false;
            try {
                std::size_t used = 0;
                out = std::stod(text, &used);
                return used == text.size() && std::isfinite(out);
            }
            catch (const std::exception&) {
                return false;
            }
        }

    }  // namespace

    ocpp::Dispatcher make_db_dispatcher(std::shared_ptr<db::Repository> repo) {
        using namespace chargelink::ocpp;
        Dispatcher dispatcher;
        auto shared = std::make_shared<DbShared>();
        shared->repo = std::move(repo);

        // Whatever the database says about "online" is left over from the previous run.
        shared->repo->mark_all_offline();

        dispatcher.on<BootNotificationRequest, BootNotificationResponse>(
            [shared](const std::string& cp_id, const BootNotificationRequest& request) {
                const json j(request);
                guarded(cp_id, "BootNotification", [&] {
                    shared->repo->upsert_boot(
                        cp_id, { request.charge_point_vendor, request.charge_point_model,
                                j.value("chargePointSerialNumber", std::string()),
                                j.value("firmwareVersion", std::string()) });
                    });
                shared->chargers.mark_booted(cp_id);  // only after the database accepted it
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
                guarded(cp_id, "Heartbeat", [&] { shared->repo->heartbeat(cp_id); });
                spdlog::debug("[{}] Heartbeat", cp_id);
                HeartbeatResponse response;
                response.current_time = chargelink::util::now_utc_iso8601();
                return response;
            });

        dispatcher.on<StatusNotificationRequest, StatusNotificationResponse>(
            [shared](const std::string& cp_id, const StatusNotificationRequest& request) {
                require_booted(*shared, cp_id, "StatusNotification");
                const json j(request);
                const std::string now = chargelink::util::now_utc_iso8601();
                const int connector = j.value("connectorId", 0);
                const std::string status = j.value("status", std::string());
                guarded(cp_id, "StatusNotification", [&] {
                    shared->repo->record_status(cp_id, connector, status,
                        j.value("errorCode", std::string("NoError")),
                        j.value("timestamp", now));
                    });
                spdlog::info("[{}] StatusNotification connector {} -> {}", cp_id, connector, status);
                return StatusNotificationResponse{};
            });

        dispatcher.on<AuthorizeRequest, AuthorizeResponse>(
            [shared](const std::string& cp_id, const AuthorizeRequest& request) {
                require_booted(*shared, cp_id, "Authorize");
                const std::string tag_status = guarded(
                    cp_id, "Authorize", [&] { return shared->repo->authorize(request.id_tag); });
                AuthorizeResponse response;
                response.id_tag_info.status = to_auth(tag_status);
                spdlog::info("[{}] Authorize {} -> {}", cp_id, request.id_tag, tag_status);
                return response;
            });

        dispatcher.on<StartTransactionRequest, StartTransactionResponse>(
            [shared](const std::string& cp_id, const StartTransactionRequest& request) {
                require_booted(*shared, cp_id, "StartTransaction");
                //if (request.connector_id < 1) {
                //    throw CallException("PropertyConstraintViolation", "connectorId must be 1 or higher");
                //}
                //if (request.meter_start < 0) {
                //    throw CallException("PropertyConstraintViolation", "meterStart must not be negative");
                //}
                const db::StartOutcome outcome = guarded(cp_id, "StartTransaction", [&] {
                    return shared->repo->start_transaction(cp_id, request.connector_id,
                        request.id_tag, request.meter_start,
                        request.timestamp);
                    });
                if (outcome.kind == db::StartOutcome::Kind::ConnectorBusy) {
                    spdlog::warn("[{}] StartTransaction on connector {}: it already has an open "
                        "transaction",
                        cp_id, request.connector_id);
                    throw CallException("GenericError", "connector already has an open transaction");
                }
                const std::string tag_status = guarded(
                    cp_id, "StartTransaction", [&] { return shared->repo->authorize(request.id_tag); });
                StartTransactionResponse response;
                response.transaction_id = outcome.id;
                response.id_tag_info.status = to_auth(tag_status);
                spdlog::info("[{}] StartTransaction {} meterStart={} -> id {} ({}){}", cp_id,
                    request.id_tag, request.meter_start, outcome.id, tag_status,
                    outcome.kind == db::StartOutcome::Kind::Duplicate
                    ? " [retry: same id as before]"
                    : "");
                return response;
            });

        dispatcher.on<StopTransactionRequest, StopTransactionResponse>(
            [shared](const std::string& cp_id, const StopTransactionRequest& request) {
                require_booted(*shared, cp_id, "StopTransaction");
                const json j(request);
                const std::string reason = j.value("reason", std::string());
                using Stop = db::StopResult;
                const Stop result = guarded(cp_id, "StopTransaction", [&] {
                    return shared->repo->stop_transaction(cp_id, request.transaction_id,
                        request.meter_stop, request.timestamp,
                        reason);
                    });
                switch (result) {
                case Stop::Stopped:
                    spdlog::info("[{}] StopTransaction id {} meterStop={} Wh reason '{}'", cp_id,
                        request.transaction_id, request.meter_stop, reason);
                    break;
                case Stop::AlreadyStopped:
                    spdlog::info("[{}] StopTransaction id {} again: already stopped", cp_id,
                        request.transaction_id);
                    break;  // a retry: answer normally so the charger can drop it from its queue
                case Stop::NotFound:
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
                const json j(request);
                const std::string now = chargelink::util::now_utc_iso8601();
                const auto& entries = j.at("meterValue");  // same order as request.meter_value
                std::size_t index = 0;
                for (const auto& value : request.meter_value) {
                    const std::string ts = entries.at(index++).value("timestamp", now);
                    for (const auto& sample : value.sampled_value) {
                        double number = 0;
                        if (!parse_number(sample.value, number)) {
                            spdlog::warn("[{}] MeterValues: skipping a sample that is not a number",
                                cp_id);
                            continue;
                        }
                        const std::string measurand =
                            sample.measurand.value_or("Energy.Active.Import.Register");
                        const std::string unit = sample.unit.value_or("Wh");
                        const bool stored = guarded(cp_id, "MeterValues", [&] {
                            return shared->repo->add_meter_value(cp_id, request.transaction_id, ts,
                                measurand, number, unit);
                            });
                        if (stored) {
                            spdlog::info("[{}]   transaction {}: {} = {} {}", cp_id,
                                request.transaction_id.value_or(-1), measurand, number, unit);
                        }
                        else {
                            spdlog::warn("[{}] meter value for unknown transaction {}: not stored",
                                cp_id, request.transaction_id.value_or(-1));
                        }
                    }
                }
                return MeterValuesResponse{};
            });

        return dispatcher;
    }

    ocpp::Dispatcher make_dispatcher() {
        const char* conninfo = std::getenv("CHARGELINK_DB");
        if (conninfo == nullptr || *conninfo == '\0') {
            spdlog::warn("CHARGELINK_DB is not set: using the in-memory store (nothing is persisted)");
            return make_memory_dispatcher();
        }
        try {
            auto repo = std::make_shared<db::Repository>(conninfo);
            spdlog::info("connected to PostgreSQL");
            return make_db_dispatcher(std::move(repo));
        }
        catch (const std::exception& e) {
            spdlog::critical("cannot start: database unavailable: {}", e.what());
            std::exit(EXIT_FAILURE);  // fail fast rather than run half-broken
        }
    }

}  // namespace chargelink::csms