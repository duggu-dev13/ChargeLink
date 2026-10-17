#include "chargelink/sim/session_bridge.hpp"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <utility>

namespace chargelink::sim {

    using ocpp::AuthorizationStatus;
    using ocpp::StopReason;

    namespace {

        std::string fixed(double value, int decimals) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
            return buffer;
        }

        ocpp::SampledValue sample(const std::string& value, const char* measurand, const char* unit) {
            ocpp::SampledValue s;
            s.value = value;
            s.context = "Sample.Periodic";
            s.measurand = measurand;
            s.unit = unit;
            return s;
        }

    }  // namespace

    SessionBridge::SessionBridge(ChargerController& controller, IStationLink& link, IHardware& hardware,
        Clock clock, int connector_id)
        : ctl_(controller),
        link_(link),
        hw_(hardware),
        clock_(std::move(clock)),
        connector_id_(connector_id) {
        ctl_.set_on_status_change(
            [this](ChargePointStatus, ChargePointStatus to, ChargePointErrorCode error) {
                on_status_change(to, error);
            });
        ctl_.set_on_session_started([this](int meter_start_wh) { on_session_started(meter_start_wh); });
        ctl_.set_on_session_ended(
            [this](int meter_stop_wh, StopReason reason) { on_session_ended(meter_stop_wh, reason); });
    }

    SessionBridge::~SessionBridge() {
        // The controller may outlive us: do not leave callbacks pointing at a dead object.
        ctl_.set_on_status_change(nullptr);
        ctl_.set_on_session_started(nullptr);
        ctl_.set_on_session_ended(nullptr);
    }

    // ---------- registration ----------

    void SessionBridge::set_registered(bool registered) {
        registered_ = registered;
        if (!registered_) {
            return;
        }
        // A snapshot of the present, not a replay of the past:
        // connector 0 is the charge point itself, then the real connector.
        send_status(0, ChargePointStatus::Available, ChargePointErrorCode::NoError);
        send_status(connector_id_, ctl_.status(), ctl_.error_code());
    }

    // ---------- status ----------

    void SessionBridge::send_status(int connector_id, ChargePointStatus status,
        ChargePointErrorCode error) {
        ocpp::StatusNotificationRequest request;
        request.connector_id = connector_id;
        request.status = status;
        request.error_code = error;
        request.timestamp = clock_();
        link_.send_status(request);
    }

    void SessionBridge::on_status_change(ChargePointStatus to, ChargePointErrorCode error) {
        if (!registered_) {
            return;  // the snapshot at registration will cover it
        }
        send_status(connector_id_, to, error);
    }

    // ---------- authorization ----------

    SessionBridge::TapResult SessionBridge::tap(const std::string& id_tag) {
        if (!registered_) {
            return TapResult::NotRegistered;
        }

        if (ctl_.session_active()) {
            if (id_tag == current_tag_) {
                ctl_.stop(StopReason::Local);  // tapping the same card again ends the session
                return TapResult::StoppedSession;
            }
            return TapResult::Busy;
        }

        const ChargePointStatus status = ctl_.status();
        if (status != ChargePointStatus::Available && status != ChargePointStatus::Preparing) {
            return TapResult::Busy;
        }

        std::weak_ptr<bool> alive = alive_;
        link_.authorize(id_tag, [this, alive, id_tag](std::optional<AuthorizationStatus> result) {
            if (alive.expired()) {
                return;  // the bridge is gone
            }
            if (!result || *result != AuthorizationStatus::Accepted) {
                spdlog::warn("tag {} was not authorized", id_tag);
                return;
            }
            if (ctl_.authorize()) {
                current_tag_ = id_tag;
            }
            else {
                spdlog::warn("tag {} was accepted but the connector cannot use it now", id_tag);
            }
            });
        return TapResult::Asked;
    }

    // ---------- transactions ----------

    void SessionBridge::on_session_started(int meter_start_wh) {
        Transaction txn;
        txn.id_tag = current_tag_;

        if (!registered_) {
            txn.start_failed = true;  // nobody to tell
            txn_ = std::move(txn);
            spdlog::warn("session started while unregistered: it will not be reported");
            return;
        }
        txn_ = std::move(txn);

        ocpp::StartTransactionRequest request;
        request.connector_id = connector_id_;
        request.id_tag = txn_->id_tag;
        request.meter_start = meter_start_wh;
        request.timestamp = clock_();

        std::weak_ptr<bool> alive = alive_;
        link_.start_transaction(
            request, [this, alive](std::optional<ocpp::StartTransactionResponse> reply) {
                if (alive.expired()) {
                    return;
                }
                on_start_reply(reply);
            });
    }

    void SessionBridge::on_start_reply(const std::optional<ocpp::StartTransactionResponse>& reply) {
        if (!txn_) {
            return;
        }
        if (!reply) {
            spdlog::warn("StartTransaction failed: this transaction will not be reported");
            txn_->start_failed = true;
            if (txn_->pending_stop) {
                txn_.reset();  // it already ended: nothing left to track
            }
            return;
        }

        txn_->transaction_id = reply->transaction_id;

        if (txn_->pending_stop) {
            // The session ended while we waited for the id: now we can report it.
            const PendingStop stop = *txn_->pending_stop;  // copy: send_stop resets txn_
            send_stop(stop);
            return;
        }

        if (reply->id_tag_info.status != AuthorizationStatus::Accepted) {
            spdlog::warn("the CSMS rejected the tag for this transaction: stopping");
            ctl_.stop(StopReason::DeAuthorized);  // ends up in on_session_ended -> send_stop
        }
    }

    void SessionBridge::on_session_ended(int meter_stop_wh, StopReason reason) {
        if (!txn_) {
            return;  // nothing was ever reported for this session
        }

        PendingStop stop;
        stop.meter_stop = meter_stop_wh;
        stop.reason = reason;
        stop.timestamp = clock_();

        if (txn_->start_failed) {
            spdlog::warn("a transaction ended that the CSMS never learned about");
            txn_.reset();
            return;
        }
        if (!txn_->transaction_id) {
            txn_->pending_stop = stop;  // the StartTransaction reply is still on its way
            return;
        }
        send_stop(stop);
    }

    void SessionBridge::send_stop(const PendingStop& stop) {
        ocpp::StopTransactionRequest request;
        if (!txn_->id_tag.empty()) {
            request.id_tag = txn_->id_tag;
        }
        request.meter_stop = stop.meter_stop;
        request.timestamp = stop.timestamp;
        request.transaction_id = *txn_->transaction_id;
        request.reason = stop.reason;

        txn_.reset();  // the transaction is over from our side
        link_.stop_transaction(request);
    }

    // ---------- meter values ----------

    void SessionBridge::sample_meter_values() {
        if (!registered_ || !ctl_.session_active() || !txn_ || !txn_->transaction_id) {
            return;
        }

        ocpp::MeterValue value;
        value.timestamp = clock_();
        value.sampled_value.push_back(
            sample(std::to_string(hw_.energy_wh()), "Energy.Active.Import.Register", "Wh"));
        value.sampled_value.push_back(sample(fixed(hw_.current_amps(), 1), "Current.Import", "A"));
        value.sampled_value.push_back(sample(fixed(hw_.temperature_c(), 1), "Temperature", "Celsius"));

        ocpp::MeterValuesRequest request;
        request.connector_id = connector_id_;
        request.transaction_id = txn_->transaction_id;
        request.meter_value.push_back(std::move(value));
        link_.send_meter_values(request);
    }

}  // namespace chargelink::sim