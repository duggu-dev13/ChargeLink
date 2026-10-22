#include "chargelink/db/repository.hpp"

#include <pqxx/pqxx>

#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace chargelink::db {

    namespace {

        std::unique_ptr<pqxx::connection> make_connection(const std::string& conninfo) {
            auto c = std::make_unique<pqxx::connection>(conninfo);

            c->prepare("upsert_boot",
                "INSERT INTO charge_points(id,vendor,model,serial_number,firmware_version,last_boot_at,online) "
                "VALUES($1,$2,$3,$4,$5,now(),true) "
                "ON CONFLICT (id) DO UPDATE SET vendor=EXCLUDED.vendor, model=EXCLUDED.model, "
                "serial_number=EXCLUDED.serial_number, firmware_version=EXCLUDED.firmware_version, "
                "last_boot_at=now(), online=true");
            c->prepare("set_online", "UPDATE charge_points SET online=$2 WHERE id=$1");
            c->prepare("all_offline", "UPDATE charge_points SET online=false WHERE online");
            c->prepare("heartbeat", "UPDATE charge_points SET last_heartbeat_at=now() WHERE id=$1");
            c->prepare("upsert_connector",
                "INSERT INTO connectors(charge_point_id,connector_id,status,error_code,updated_at) "
                "VALUES($1,$2,$3,$4,$5::timestamptz) "
                "ON CONFLICT (charge_point_id,connector_id) DO UPDATE SET status=EXCLUDED.status, "
                "error_code=EXCLUDED.error_code, updated_at=EXCLUDED.updated_at");
            c->prepare("status_event",
                "INSERT INTO status_events(charge_point_id,connector_id,status,error_code,ts) "
                "VALUES($1,$2,$3,$4,$5::timestamptz)");
            c->prepare("ensure_connector",
                "INSERT INTO connectors(charge_point_id,connector_id) VALUES($1,$2) ON CONFLICT DO NOTHING");
            // No conflict target: either unique index (one open tx per connector, or the exact
            // same start sent twice) makes this return no row instead of failing.
            c->prepare("start_tx",
                "INSERT INTO transactions(charge_point_id,connector_id,id_tag,meter_start_wh,started_at) "
                "VALUES($1,$2,$3,$4,$5::timestamptz) ON CONFLICT DO NOTHING RETURNING id");
            c->prepare("find_start",
                "SELECT id FROM transactions WHERE charge_point_id=$1 AND connector_id=$2 "
                "AND id_tag=$3 AND meter_start_wh=$4 AND started_at=$5::timestamptz");
            c->prepare("lock_tx",
                "SELECT charge_point_id, meter_start_wh, stopped_at IS NOT NULL "
                "FROM transactions WHERE id=$1 FOR UPDATE");
            // GREATEST: a charger with a slow clock must not violate stopped_at >= started_at.
            c->prepare("stop_tx",
                "UPDATE transactions SET meter_stop_wh=$2, stopped_at=GREATEST($3::timestamptz, started_at), "
                "stop_reason=NULLIF($4,'') WHERE id=$1");
            // Stores the sample only if it has no transaction, or the transaction is this charger's.
            c->prepare("meter_value",
                "INSERT INTO meter_values(charge_point_id,transaction_id,ts,measurand,value,unit) "
                "SELECT $1::text,$2::int,$3::timestamptz,$4::text,$5::numeric,$6::text "
                "WHERE $2::int IS NULL OR EXISTS "
                "(SELECT 1 FROM transactions WHERE id=$2::int AND charge_point_id=$1::text) "
                "RETURNING id");
            c->prepare("authorize",
                "SELECT CASE WHEN status='Accepted' AND expires_at IS NOT NULL AND expires_at < now() "
                "THEN 'Expired' ELSE status END FROM id_tags WHERE id_tag=$1");

            c->prepare("list_chargers",
                "SELECT id, coalesce(vendor,''), coalesce(model,''), coalesce(firmware_version,''), online, "
                "coalesce(to_char(last_heartbeat_at AT TIME ZONE 'UTC','YYYY-MM-DD HH24:MI:SS'),'') "
                "FROM charge_points ORDER BY id");
            c->prepare("recent_transactions",
                "SELECT id, charge_point_id, connector_id, id_tag, meter_start_wh, meter_stop_wh, "
                "to_char(started_at AT TIME ZONE 'UTC','YYYY-MM-DD HH24:MI:SS'), "
                "coalesce(to_char(stopped_at AT TIME ZONE 'UTC','YYYY-MM-DD HH24:MI:SS'),''), "
                "coalesce(stop_reason,'') FROM transactions "
                "WHERE $1::text IS NULL OR charge_point_id=$1::text ORDER BY id DESC LIMIT $2::int");
            c->prepare("energy_per_charger",
                "SELECT charge_point_id, count(*), coalesce(sum(meter_stop_wh - meter_start_wh),0) "
                "FROM transactions WHERE stopped_at IS NOT NULL "
                "GROUP BY charge_point_id ORDER BY charge_point_id");

            return c;
        }

    }  // namespace

    Repository::Repository(std::string conninfo, std::size_t pool_size)
        : conninfo_(std::move(conninfo)), max_(pool_size == 0 ? 1 : pool_size) {
        idle_.push_back(make_connection(conninfo_));  // fail fast on bad credentials / DB down
        created_ = 1;
    }

    Repository::~Repository() = default;

    std::unique_ptr<pqxx::connection> Repository::acquire() {
        std::unique_lock<std::mutex> lock(mu_);
        if (!cv_.wait_for(lock, std::chrono::seconds(5),
            [this] { return !idle_.empty() || created_ < max_; })) {
            throw std::runtime_error("database connection pool exhausted");
        }
        if (!idle_.empty()) {
            auto conn = std::move(idle_.back());
            idle_.pop_back();
            return conn;
        }
        ++created_;
        lock.unlock();
        try {
            return make_connection(conninfo_);
        }
        catch (...) {
            lock.lock();
            --created_;
            cv_.notify_one();
            throw;
        }
    }

    void Repository::release(std::unique_ptr<pqxx::connection> conn, bool healthy) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (healthy && conn && conn->is_open()) {
                idle_.push_back(std::move(conn));
            }
            else {
                --created_;  // dropped; a replacement is made on demand
            }
        }
        cv_.notify_one();
    }

    template <class F>
    auto Repository::run(F&& f) {
        auto conn = acquire();
        bool healthy = true;
        struct Lease {  // always hands the connection back, whichever way we leave
            Repository& repo;
            std::unique_ptr<pqxx::connection>& conn;
            bool& healthy;
            ~Lease() { repo.release(std::move(conn), healthy); }
        } lease{ *this, conn, healthy };

        try {
            pqxx::work tx{ *conn };
            if constexpr (std::is_void_v<decltype(f(tx))>) {
                f(tx);
                tx.commit();
            }
            else {
                auto result = f(tx);
                tx.commit();
                return result;
            }
        }
        catch (const pqxx::broken_connection&) {
            healthy = false;
            throw;
        }
        catch (const pqxx::data_exception& e) {
            throw BadInputError(e.what());  // SQLSTATE class 22: the value itself was bad
        }
    }

    void Repository::upsert_boot(const std::string& cp, const BootInfo& b) {
        run([&](pqxx::work& tx) {
            tx.exec_prepared0("upsert_boot", cp, b.vendor, b.model, b.serial, b.firmware);
            });
    }

    void Repository::set_online(const std::string& cp, bool online) {
        run([&](pqxx::work& tx) { tx.exec_prepared0("set_online", cp, online); });
    }

    void Repository::mark_all_offline() {
        run([&](pqxx::work& tx) { tx.exec_prepared0("all_offline"); });
    }

    void Repository::heartbeat(const std::string& cp) {
        run([&](pqxx::work& tx) { tx.exec_prepared0("heartbeat", cp); });
    }

    void Repository::record_status(const std::string& cp, int connector, const std::string& status,
        const std::string& error_code, const std::string& timestamp) {
        run([&](pqxx::work& tx) {
            if (connector >= 1) {  // connector 0 means "the whole charger": history only
                tx.exec_prepared0("upsert_connector", cp, connector, status, error_code, timestamp);
            }
            tx.exec_prepared0("status_event", cp, connector, status, error_code, timestamp);
            });
    }

    StartOutcome Repository::start_transaction(const std::string& cp, int connector,
        const std::string& id_tag,
        std::int64_t meter_start_wh,
        const std::string& timestamp) {
        return run([&](pqxx::work& tx) {
            tx.exec_prepared0("ensure_connector", cp, connector);
            pqxx::result created =
                tx.exec_prepared("start_tx", cp, connector, id_tag, meter_start_wh, timestamp);
            if (!created.empty()) {
                return StartOutcome{ StartOutcome::Kind::Created, created[0][0].as<int>() };
            }
            pqxx::result same =
                tx.exec_prepared("find_start", cp, connector, id_tag, meter_start_wh, timestamp);
            if (!same.empty()) {
                return StartOutcome{ StartOutcome::Kind::Duplicate, same[0][0].as<int>() };
            }
            return StartOutcome{ StartOutcome::Kind::ConnectorBusy, 0 };
            });
    }

    StopResult Repository::stop_transaction(const std::string& cp, int tx_id,
        std::int64_t meter_stop_wh, const std::string& timestamp,
        const std::string& reason) {
        return run([&](pqxx::work& tx) {
            pqxx::result row = tx.exec_prepared("lock_tx", tx_id);  // row lock serialises racing stops
            if (row.empty()) return StopResult::NotFound;
            if (row[0][0].as<std::string>() != cp) return StopResult::WrongCharger;
            if (row[0][2].as<bool>()) return StopResult::AlreadyStopped;
            if (meter_stop_wh < row[0][1].as<std::int64_t>()) return StopResult::MeterDecreased;
            tx.exec_prepared0("stop_tx", tx_id, meter_stop_wh, timestamp, reason);
            return StopResult::Stopped;
            });
    }

    bool Repository::add_meter_value(const std::string& cp, std::optional<int> tx_id,
        const std::string& timestamp, const std::string& measurand,
        double value, const std::string& unit) {
        return run([&](pqxx::work& tx) {
            pqxx::result r =
                tx.exec_prepared("meter_value", cp, tx_id, timestamp, measurand, value, unit);
            return !r.empty();
            });
    }

    std::string Repository::authorize(const std::string& id_tag) {
        return run([&](pqxx::work& tx) -> std::string {
            pqxx::result r = tx.exec_prepared("authorize", id_tag);
            return r.empty() ? std::string("Invalid") : r[0][0].as<std::string>();
            });
    }

    std::vector<ChargerRow> Repository::list_chargers() {
        return run([&](pqxx::work& tx) {
            std::vector<ChargerRow> out;
            for (const auto& r : tx.exec_prepared("list_chargers")) {
                ChargerRow c;
                c.id = r[0].as<std::string>();
                c.vendor = r[1].as<std::string>();
                c.model = r[2].as<std::string>();
                c.firmware = r[3].as<std::string>();
                c.online = r[4].as<bool>();
                c.last_heartbeat = r[5].as<std::string>();
                out.push_back(std::move(c));
            }
            return out;
            });
    }

    std::vector<TransactionRow> Repository::recent_transactions(
        int limit, const std::optional<std::string>& cp) {
        if (limit < 1) limit = 1;
        if (limit > 1000) limit = 1000;
        return run([&](pqxx::work& tx) {
            std::vector<TransactionRow> out;
            for (const auto& r : tx.exec_prepared("recent_transactions", cp, limit)) {
                TransactionRow t;
                t.id = r[0].as<int>();
                t.charge_point_id = r[1].as<std::string>();
                t.connector_id = r[2].as<int>();
                t.id_tag = r[3].as<std::string>();
                t.meter_start_wh = r[4].as<std::int64_t>();
                if (!r[5].is_null()) t.meter_stop_wh = r[5].as<std::int64_t>();
                t.started_at = r[6].as<std::string>();
                t.stopped_at = r[7].as<std::string>();
                t.stop_reason = r[8].as<std::string>();
                out.push_back(std::move(t));
            }
            return out;
            });
    }

    std::vector<EnergyRow> Repository::energy_per_charger() {
        return run([&](pqxx::work& tx) {
            std::vector<EnergyRow> out;
            for (const auto& r : tx.exec_prepared("energy_per_charger")) {
                EnergyRow e;
                e.charge_point_id = r[0].as<std::string>();
                e.sessions = r[1].as<std::int64_t>();
                e.wh = r[2].as<std::int64_t>();
                out.push_back(std::move(e));
            }
            return out;
            });
    }

}  // namespace chargelink::db