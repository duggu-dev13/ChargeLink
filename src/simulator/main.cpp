#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <boost/version.hpp>

int main() {
    spdlog::info("ChargeLink v0.1");
    spdlog::info("Boost version: {}.{}.{}",
        BOOST_VERSION / 100000,
        BOOST_VERSION / 100 % 1000,
        BOOST_VERSION % 100);

    // An OCPP CALL frame: [MessageTypeId, UniqueId, Action, Payload]
    auto frame = nlohmann::json::parse(R"([2,"abc123","Heartbeat",{}])");
    spdlog::info("Action: {}", frame[2].get<std::string>());
    spdlog::info("Frame:  {}", frame.dump());
    return 0;
}