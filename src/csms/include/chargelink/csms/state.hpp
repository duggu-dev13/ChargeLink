#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace chargelink::csms {

    // Which chargers have completed BootNotification. Thread-safe.
    class ChargerStates {
    public:
        void mark_booted(const std::string& id);
        bool is_booted(const std::string& id) const;
        void forget(const std::string& id);
        std::size_t booted_count() const;

    private:
        mutable std::mutex mutex_;
        std::unordered_set<std::string> booted_;
    };

    struct Transaction {
        int id = 0;
        std::string charge_point_id;
        int connector_id = 0;
        std::string id_tag;
        int meter_start = 0;
        std::string start_time;
        bool active = true;
        int meter_stop = 0;
        std::string stop_time;
        int last_meter_wh = 0;  // newest Energy.Active.Import.Register sample
    };

    // All transactions the CSMS has handed out. Thread-safe. In memory only (Phase 4 adds SQL).
    class TransactionStore {
    public:
        enum class StopResult { Stopped, Duplicate, Unknown, WrongCharger, MeterDecreased };
        struct StartResult {
            int id = 0;
            bool duplicate = false;  // the same StartTransaction arrived again
        };

        // Idempotent: a retried StartTransaction (same charger, connector, tag, meter and
        // timestamp) gets the SAME id back instead of creating a second billable transaction.
        StartResult start(const std::string& charge_point_id, int connector_id,
            const std::string& id_tag, int meter_start, const std::string& timestamp);

        // Idempotent too: stopping an already stopped transaction is a harmless duplicate.
        StopResult stop(const std::string& charge_point_id, int transaction_id, int meter_stop,
            const std::string& timestamp);

        // False if the transaction is unknown, belongs to another charger, or has ended.
        bool record_meter(const std::string& charge_point_id, int transaction_id, int energy_wh);

        std::optional<Transaction> find(int transaction_id) const;
        std::size_t active_count() const;
        std::size_t total() const;

    private:
        using StartKey = std::tuple<std::string, int, std::string, int, std::string>;

        mutable std::mutex mutex_;
        std::unordered_map<int, Transaction> by_id_;
        std::map<StartKey, int> by_start_;
        int next_id_ = 1000;
    };

}  // namespace chargelink::csms