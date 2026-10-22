#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS  // getenv
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "chargelink/db/repository.hpp"

namespace {

    using Row = std::vector<std::string>;

    void print_table(const Row& headers, const std::vector<Row>& rows) {
        std::vector<std::size_t> width(headers.size());
        for (std::size_t i = 0; i < headers.size(); ++i) width[i] = headers[i].size();
        for (const Row& r : rows) {
            for (std::size_t i = 0; i < r.size(); ++i) width[i] = std::max(width[i], r[i].size());
        }
        auto line = [&](const Row& r) {
            for (std::size_t i = 0; i < r.size(); ++i) {
                std::cout << std::left << std::setw(static_cast<int>(width[i])) << r[i];
                if (i + 1 < r.size()) std::cout << "  ";
            }
            std::cout << '\n';
            };
        line(headers);
        Row dashes;
        for (std::size_t w : width) dashes.emplace_back(w, '-');
        line(dashes);
        for (const Row& r : rows) line(r);
        if (rows.empty()) std::cout << "(no rows)\n";
    }

    std::string kwh(std::int64_t wh) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(wh) / 1000.0);
        return buf;
    }

    void usage() {
        std::cerr << "usage: report chargers\n"
            "       report transactions [limit] [charger-id]\n"
            "       report energy\n"
            "needs the environment variable CHARGELINK_DB (a libpq connection string)\n";
    }

}  // namespace

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "";
    const char* conninfo = std::getenv("CHARGELINK_DB");
    if (command.empty() || conninfo == nullptr || *conninfo == '\0') {
        usage();
        return 2;
    }

    try {
        chargelink::db::Repository repo(conninfo, 1);

        if (command == "chargers") {
            std::vector<Row> rows;
            for (const auto& c : repo.list_chargers()) {
                rows.push_back({ c.id, c.vendor, c.model, c.firmware, c.online ? "yes" : "no",
                                c.last_heartbeat.empty() ? "-" : c.last_heartbeat });
            }
            print_table({ "CHARGER", "VENDOR", "MODEL", "FIRMWARE", "ONLINE", "LAST HEARTBEAT (UTC)" }, rows);
            return 0;
        }

        if (command == "transactions") {
            int limit = 20;
            std::optional<std::string> charger;
            for (int i = 2; i < argc; ++i) {  // all-digit word = limit, anything else = charger id
                const std::string arg = argv[i];
                const bool number = !arg.empty() && arg.size() <= 4 &&
                    std::all_of(arg.begin(), arg.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
                if (number) limit = std::stoi(arg);
                else charger = arg;
            }
            std::vector<Row> rows;
            for (const auto& t : repo.recent_transactions(limit, charger)) {
                rows.push_back({ std::to_string(t.id), t.charge_point_id, std::to_string(t.connector_id),
                                t.id_tag, std::to_string(t.meter_start_wh),
                                t.meter_stop_wh ? std::to_string(*t.meter_stop_wh) : "-",
                                t.meter_stop_wh ? kwh(*t.meter_stop_wh - t.meter_start_wh) : "open",
                                t.started_at, t.stopped_at.empty() ? "-" : t.stopped_at,
                                t.stop_reason.empty() ? "-" : t.stop_reason });
            }
            print_table({ "ID", "CHARGER", "CONN", "TAG", "START Wh", "STOP Wh", "kWh", "STARTED (UTC)",
                         "STOPPED (UTC)", "REASON" }, rows);
            return 0;
        }

        if (command == "energy") {
            std::vector<Row> rows;
            std::int64_t total_wh = 0;
            for (const auto& e : repo.energy_per_charger()) {
                rows.push_back({ e.charge_point_id, std::to_string(e.sessions), kwh(e.wh) });
                total_wh += e.wh;
            }
            print_table({ "CHARGER", "SESSIONS", "kWh" }, rows);
            if (!rows.empty()) std::cout << "total: " << kwh(total_wh) << " kWh\n";
            return 0;
        }

        usage();
        return 2;
    }
    catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}