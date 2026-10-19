#include "chargelink/sim/station.hpp"

#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include <iostream>
#include <utility>

#include "chargelink/util/time.hpp"

namespace chargelink::sim {

    namespace net = boost::asio;
    using SteadyClock = std::chrono::steady_clock;

    namespace {

        const char* status_name(ChargePointStatus status) {
            switch (status) {
            case ChargePointStatus::Available:     return "Available";
            case ChargePointStatus::Preparing:     return "Preparing";
            case ChargePointStatus::Charging:      return "Charging";
            case ChargePointStatus::SuspendedEVSE: return "SuspendedEVSE";
            case ChargePointStatus::SuspendedEV:   return "SuspendedEV";
            case ChargePointStatus::Finishing:     return "Finishing";
            case ChargePointStatus::Reserved:      return "Reserved";
            case ChargePointStatus::Unavailable:   return "Unavailable";
            case ChargePointStatus::Faulted:       return "Faulted";
            }
            return "?";
        }

        void say(const std::string& text) {
            std::cout << text << std::endl;
        }

    }  // namespace

    // ---------- construction ----------

    std::shared_ptr<Station> Station::create(net::io_context& ioc, ClientConfig config,
        StationOptions options) {
        return std::shared_ptr<Station>(new Station(ioc, std::move(config), options));
    }

    // Members are initialised in declaration order: hardware, controller, client, link, bridge,
    // then the timers (which live on the client's strand).
    Station::Station(net::io_context& ioc, ClientConfig config, StationOptions options)
        : options_(options),
        time_scale_(options.time_scale),
        ctl_(hw_),
        client_(ReconnectingClient::create(ioc, std::move(config))),
        link_(*client_),
        bridge_(ctl_, link_, hw_, [] { return util::now_utc_iso8601(); }),
        poll_timer_(client_->executor()),
        meter_timer_(client_->executor()) {
    }

    void Station::start() {
        std::weak_ptr<Station> weak = weak_from_this();
        client_->set_on_registered([weak] {
            if (auto self = weak.lock()) {
                self->link_.set_online(true);
                self->bridge_.set_registered(true);
                say("registered with the CSMS");
            }
            });
        client_->set_on_disconnected([weak] {
            if (auto self = weak.lock()) {
                self->on_connection_lost();
            }
            });
        client_->set_on_stopped([weak] {
            if (auto self = weak.lock()) {
                self->shutdown();
            }
            });

        net::post(client_->executor(), [self = shared_from_this()] {
            self->last_poll_ = SteadyClock::now();
            self->schedule_poll();
            self->schedule_meter();
            });
        client_->start();
    }

    void Station::submit(std::string line) {
        net::post(client_->executor(), [self = shared_from_this(), line = std::move(line)] {
            self->execute(parse_command(line));
            });
    }

    // ---------- the two timers ----------

    void Station::schedule_poll() {
        poll_timer_.expires_after(options_.poll_interval);
        poll_timer_.async_wait(
            [self = shared_from_this()](boost::system::error_code ec) { self->on_poll(ec); });
    }

    // Let simulated time pass, then let the controller look at the hardware.
    void Station::on_poll(boost::system::error_code ec) {
        if (ec || stopped_) {
            return;
        }
        const auto now = SteadyClock::now();
        const double real_seconds = std::chrono::duration<double>(now - last_poll_).count();
        last_poll_ = now;

        hw_.tick(real_seconds * time_scale_);
        ctl_.tick();
        schedule_poll();
    }

    void Station::schedule_meter() {
        meter_timer_.expires_after(options_.meter_interval);
        meter_timer_.async_wait(
            [self = shared_from_this()](boost::system::error_code ec) { self->on_meter(ec); });
    }

    void Station::on_meter(boost::system::error_code ec) {
        if (ec || stopped_) {
            return;
        }
        bridge_.sample_meter_values();
        schedule_meter();
    }

    // ---------- connection events and shutdown ----------

    // Called for every lost connection and every failed attempt. The charger itself keeps
    // working: the controller does not depend on the network.
    void Station::on_connection_lost() {
        const bool was_registered = bridge_.registered();
        link_.set_online(false);
        bridge_.set_registered(false);
        if (was_registered && !stopped_) {
            say("connection lost: the charger keeps working locally and will reconnect");
        }
    }

    void Station::shutdown() {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        poll_timer_.cancel();
        meter_timer_.cancel();
    }

    // ---------- the console ----------

    void Station::execute(const Command& command) {
        switch (command.type) {
        case CommandType::Empty:
            break;

        case CommandType::Invalid:
            say(command.text);
            break;

        case CommandType::Help:
            std::cout << help_text() << std::flush;
            break;

        case CommandType::Plug:
            hw_.plug_car(command.has_number ? command.number : 32.0);
            say("car plugged in");
            break;

        case CommandType::Unplug:
            hw_.unplug_car();
            say("cable removed");
            break;

        case CommandType::Tap:
            switch (bridge_.tap(command.text)) {
            case SessionBridge::TapResult::NotRegistered:
                say("not registered with the CSMS yet");
                break;
            case SessionBridge::TapResult::Busy:
                say("cannot use a card now (busy, or a different card is in use)");
                break;
            case SessionBridge::TapResult::StoppedSession:
                say("stopping the session");
                break;
            case SessionBridge::TapResult::Asked:
                say("asking the CSMS to authorize " + command.text);
                break;
            }
            break;

        case CommandType::Pause:
            hw_.car_wants_charge(false);
            say("the car stops asking for energy");
            break;

        case CommandType::Resume:
            hw_.car_wants_charge(true);
            say("the car asks for energy");
            break;

        case CommandType::Fault:
            hw_.inject_pilot_error(true);
            say("pilot fault injected");
            break;

        case CommandType::Clear:
            hw_.inject_pilot_error(false);
            say(ctl_.clear_fault() ? "fault cleared"
                : "nothing to clear, or the cause is still there");
            break;

        case CommandType::Heat:
            hw_.set_ambient_c(command.number);
            say("ambient temperature set");
            break;

        case CommandType::Speed:
            time_scale_ = command.number;
            say("simulated time now runs faster than real time");
            break;

        case CommandType::Status:
            print_status();
            break;

        case CommandType::Quit:
            say("bye");
            shutdown();       // stop our timers first, so on_connection_lost stays quiet
            client_->stop();  // closes the connection, then reports on_stopped
            break;
        }
    }

    void Station::print_status() const {
        std::cout << "  charger   : " << status_name(ctl_.status()) << "\n"
            << "  pilot     : " << to_string(hw_.read_pilot()) << ", cable "
            << (hw_.car_plugged() ? "in" : "out") << "\n"
            << "  contactor : " << (hw_.contactor_closed() ? "CLOSED" : "open") << ", "
            << hw_.current_amps() << " A\n"
            << "  energy    : " << hw_.energy_wh() << " Wh\n"
            << "  temp      : " << hw_.temperature_c() << " C\n"
            << "  csms      : " << (bridge_.registered() ? "registered" : "not registered") << "\n"
            << "  queued    : " << link_.queued() << " message(s) waiting for the CSMS\n"
            << "  session   : " << (ctl_.session_active() ? "active" : "none")
            << ", simulated time x" << time_scale_ << std::endl;
    }

}  // namespace chargelink::sim