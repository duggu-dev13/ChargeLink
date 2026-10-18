#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <memory>
#include <string>

#include "chargelink/sim/charger_controller.hpp"
#include "chargelink/sim/console.hpp"
#include "chargelink/sim/ocpp_link.hpp"
#include "chargelink/sim/reconnecting_client.hpp"
#include "chargelink/sim/session_bridge.hpp"
#include "chargelink/sim/sim_hardware.hpp"

namespace chargelink::sim {

    struct StationOptions {
        std::chrono::milliseconds poll_interval{ 100 };  // how often the controller looks at the hardware
        std::chrono::seconds meter_interval{ 10 };       // real seconds between MeterValues
        double time_scale = 60.0;                      // simulated seconds per real second
    };

    // A complete simulated charge point: hardware, controller, OCPP bridge and a reconnecting
    // network client, all running on one strand, plus a text console to play with it.
    class Station : public std::enable_shared_from_this<Station> {
    public:
        static std::shared_ptr<Station> create(boost::asio::io_context& ioc, ClientConfig config,
            StationOptions options = {});

        void start();                   // connect and start the timers
        void submit(std::string line);  // run a console line; safe to call from any thread

    private:
        Station(boost::asio::io_context& ioc, ClientConfig config, StationOptions options);

        void execute(const Command& command);
        void print_status() const;
        void on_connection_lost();
        void shutdown();

        void schedule_poll();
        void on_poll(boost::system::error_code ec);
        void schedule_meter();
        void on_meter(boost::system::error_code ec);

        StationOptions options_;
        double time_scale_;

        SimHardware hw_;
        ChargerController ctl_;
        std::shared_ptr<ReconnectingClient> client_;
        OcppLink link_;
        SessionBridge bridge_;

        boost::asio::steady_timer poll_timer_;
        boost::asio::steady_timer meter_timer_;
        std::chrono::steady_clock::time_point last_poll_;
        bool stopped_ = false;
    };

}  // namespace chargelink::sim