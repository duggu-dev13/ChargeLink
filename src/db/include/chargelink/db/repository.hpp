#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pqxx { class connection; }

namespace chargelink::db {

    struct BootInfo {
        std::string vendor;
        std::string model;
        std::string serial;
        std::string firmware;
    };

    struct StartOutcome {
        enum class Kind { Created, Duplicate, ConnectorBusy };
        Kind kind = Kind::Created;
        int id = 0;  // 0 when ConnectorBusy
    };

    enum class StopResult { Stopped, AlreadyStopped, NotFound, WrongCharger, MeterDecreased };

    // Thread-safe. Holds a small pool of connections; every call is one short transaction
    // (commit on success, rollback on exception). Timestamps are ISO-8601 strings, parsed by
    // PostgreSQL. Methods throw std::exception subclasses (pqxx::sql_error, ...) on failure.
    // A call that hits a broken connection is NOT retried: the connection is dropped and the
    // next call gets a fresh one.
    class Repository {
    public:
        explicit Repository(std::string conninfo, std::size_t pool_size = 4);
        ~Repository();
        Repository(const Repository&) = delete;
        Repository& operator=(const Repository&) = delete;

        void upsert_boot(const std::string& cp, const BootInfo& info);
        void set_online(const std::string& cp, bool online);
        void heartbeat(const std::string& cp);
        void record_status(const std::string& cp, int connector, const std::string& status,
            const std::string& error_code, const std::string& timestamp);

        // Idempotent: the same start sent again returns Duplicate with the original id.
        StartOutcome start_transaction(const std::string& cp, int connector,
            const std::string& id_tag, std::int64_t meter_start_wh,
            const std::string& timestamp);
        StopResult stop_transaction(const std::string& cp, int tx_id, std::int64_t meter_stop_wh,
            const std::string& timestamp, const std::string& reason);

        void add_meter_value(const std::string& cp, std::optional<int> tx_id,
            const std::string& timestamp, const std::string& measurand,
            double value, const std::string& unit);

        // "Accepted" | "Blocked" | "Expired" | "Invalid" (unknown tag)
        std::string authorize(const std::string& id_tag);

    private:
        std::unique_ptr<pqxx::connection> acquire();
        void release(std::unique_ptr<pqxx::connection> conn, bool healthy);
        template <class F>
        auto run(F&& f);

        std::string conninfo_;
        std::size_t max_;
        std::mutex mu_;
        std::condition_variable cv_;
        std::vector<std::unique_ptr<pqxx::connection>> idle_;
        std::size_t created_ = 0;
    };

}  // namespace chargelink::db