#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "chargelink/csms/handlers.hpp"
#include "chargelink/csms/server.hpp"

namespace net = boost::asio;

namespace {

    // A handler that throws must not kill the thread: log it and keep serving.
    void run_io(net::io_context& ioc) {
        for (;;) {
            try {
                ioc.run();
                return;
            }
            catch (const std::exception& e) {
                spdlog::error("a handler threw: {}", e.what());
            }
        }
    }

}  // namespace

// usage: csms [port] [threads] [-v]
int main(int argc, char** argv) {
    unsigned short port = 9000;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    bool verbose = false;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-v") {
            verbose = true;
        }
        else if (positional == 0) {
            port = static_cast<unsigned short>(std::atoi(arg.c_str()));
            ++positional;
        }
        else if (positional == 1) {
            threads = static_cast<unsigned>(std::max(1, std::min(64, std::atoi(arg.c_str()))));
            ++positional;
        }
    }
    if (port == 0) {
        spdlog::error("usage: csms [port] [threads] [-v]");
        return 2;
    }
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);

    try {
        auto csms = chargelink::csms::make_csms();
        auto dispatcher =
            std::make_shared<const chargelink::ocpp::Dispatcher>(std::move(csms.dispatcher));
        auto hooks =
            std::make_shared<const chargelink::csms::ConnectionHooks>(std::move(csms.hooks));

        net::io_context ioc{ static_cast<int>(threads) };
        auto server = chargelink::csms::Server::create(
            ioc, boost::asio::ip::tcp::endpoint{ boost::asio::ip::tcp::v4(), port }, dispatcher, hooks);
        server->start();
        spdlog::info("CSMS listening on port {} with {} thread(s); Ctrl+C to stop", port, threads);

        net::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](boost::system::error_code ec, int) {
            if (ec) {
                return;
            }
            spdlog::info("shutting down (press Ctrl+C again to force)");
            signals.clear();  // default handling is back: a second Ctrl+C kills the process
            server->stop();
            });

        std::vector<std::thread> pool;
        for (unsigned i = 1; i < threads; ++i) {
            pool.emplace_back([&ioc] { run_io(ioc); });
        }
        run_io(ioc);
        for (auto& thread : pool) {
            thread.join();
        }
        if (hooks->on_shutdown) {
            hooks->on_shutdown();  // every connection is gone: mark all chargers offline
        }
        spdlog::info("CSMS stopped");
        return 0;
    }
    catch (const std::exception& e) {
        spdlog::critical("fatal: {}", e.what());
        return 1;
    }
}