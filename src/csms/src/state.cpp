#include "chargelink/csms/state.hpp"

#include <algorithm>

namespace chargelink::csms {

    void ChargerStates::mark_booted(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        booted_.insert(id);
    }

    bool ChargerStates::is_booted(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return booted_.count(id) != 0;
    }

    void ChargerStates::forget(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        booted_.erase(id);
    }

    std::size_t ChargerStates::booted_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return booted_.size();
    }

    TransactionStore::StartResult TransactionStore::start(const std::string& charge_point_id,
        int connector_id, const std::string& id_tag,
        int meter_start,
        const std::string& timestamp) {
        std::lock_guard<std::mutex> lock(mutex_);
        const StartKey key{ charge_point_id, connector_id, id_tag, meter_start, timestamp };
        const auto existing = by_start_.find(key);
        if (existing != by_start_.end()) {
            return { existing->second, true };
        }

        Transaction txn;
        txn.id = next_id_++;
        txn.charge_point_id = charge_point_id;
        txn.connector_id = connector_id;
        txn.id_tag = id_tag;
        txn.meter_start = meter_start;
        txn.start_time = timestamp;
        txn.last_meter_wh = meter_start;
        by_start_.emplace(key, txn.id);
        by_id_.emplace(txn.id, txn);
        return { txn.id, false };
    }

    TransactionStore::StopResult TransactionStore::stop(const std::string& charge_point_id,
        int transaction_id, int meter_stop,
        const std::string& timestamp) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = by_id_.find(transaction_id);
        if (it == by_id_.end()) {
            return StopResult::Unknown;
        }
        Transaction& txn = it->second;
        if (txn.charge_point_id != charge_point_id) {
            return StopResult::WrongCharger;  // one charger must not close another's transaction
        }
        if (!txn.active) {
            return StopResult::Duplicate;
        }
        if (meter_stop < txn.meter_start) {
            return StopResult::MeterDecreased;  // an energy register only ever increases
        }
        txn.active = false;
        txn.meter_stop = meter_stop;
        txn.stop_time = timestamp;
        return StopResult::Stopped;
    }

    bool TransactionStore::record_meter(const std::string& charge_point_id, int transaction_id,
        int energy_wh) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = by_id_.find(transaction_id);
        if (it == by_id_.end() || it->second.charge_point_id != charge_point_id || !it->second.active) {
            return false;
        }
        it->second.last_meter_wh = std::max(it->second.last_meter_wh, energy_wh);
        return true;
    }

    std::optional<Transaction> TransactionStore::find(int transaction_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = by_id_.find(transaction_id);
        if (it == by_id_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::size_t TransactionStore::active_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<std::size_t>(std::count_if(
            by_id_.begin(), by_id_.end(), [](const auto& e) { return e.second.active; }));
    }

    std::size_t TransactionStore::total() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return by_id_.size();
    }

}  // namespace chargelink::csms