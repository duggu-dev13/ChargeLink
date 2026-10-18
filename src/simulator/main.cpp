#include <boost/asio/io_context.hpp>
#include <spdlog/spdlog.h>

#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "chargelink/sim/station.hpp"

int main(int argc, char** argv) {
    chargelink::sim::ClientConfig config;
    if (argc > 1) config.host = argv[1];
    if (argc > 2) config.port = argv[2];
    if (argc > 3) config.charge_point_id = argv[3];

    try {
        boost::asio::io_context ioc;
        auto station = chargelink::sim::Station::create(ioc, config);
        station->start();

        std::cout << "ChargeLink simulator " << config.charge_point_id
            << ". Type 'help' for commands." << std::endl;

        // getline blocks, so it gets its own thread. It holds only a weak_ptr and hands every
        // line to the station's strand; it is detached because we cannot interrupt getline.
        std::weak_ptr<chargelink::sim::Station> weak = station;
        std::thread([weak] {
            std::string line;
            while (std::getline(std::cin, line)) {
                auto station = weak.lock();
                if (!station) return;
                station->submit(line);
            }
            if (auto station = weak.lock()) {
                station->submit("quit");  // stdin closed
            }
            }).detach();

        ioc.run();  // returns once the station has stopped and nothing is pending
        return 0;
    }
    catch (const std::exception& e) {
        spdlog::critical("fatal: {}", e.what());
        return 1;
    }
}