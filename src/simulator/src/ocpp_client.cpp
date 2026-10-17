#include "chargelink/sim/ocpp_client.hpp"

#include <boost/asio/post.hpp>
#include <boost/beast/http.hpp>
#include <spdlog/spdlog.h>

#include <utility>
#include <variant>

namespace chargelink::sim {

    namespace net = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace websocket = beast::websocket;
    using tcp = net::ip::tcp;

    using ocpp::CallOutcome;

    // ---------- construction and public API ----------

    std::shared_ptr<OcppClient> OcppClient::create(net::io_context& ioc, ClientConfig config) {
        return std::shared_ptr<OcppClient>(new OcppClient(ioc, std::move(config)));
    }

    OcppClient::OcppClient(net::io_context& ioc, ClientConfig config)
        : strand_(net::make_strand(ioc)),
        resolver_(strand_),
        ws_(strand_),
        heartbeat_timer_(strand_),
        retry_timer_(strand_),
        housekeeping_timer_(strand_),
        config_(std::move(config)) {
        target_ = "/ocpp/" + config_.charge_point_id;
        heartbeat_interval_ = config_.default_heartbeat_interval;
    }

    void OcppClient::start() {
        net::post(strand_, [self = shared_from_this()] { self->do_resolve(); });
    }

    void OcppClient::stop() {
        net::post(strand_, [self = shared_from_this()] { self->shutdown(); });
    }

    void OcppClient::call(std::string action, ocpp::json payload, ReplyCallback on_reply) {
        net::post(strand_, [self = shared_from_this(), action = std::move(action),
            payload = std::move(payload), on_reply = std::move(on_reply)]() mutable {
                self->do_call(std::move(action), std::move(payload), std::move(on_reply));
            });
    }

    // ---------- the connection chain: resolve -> connect -> handshake ----------

    void OcppClient::do_resolve() {
        spdlog::info("[{}] connecting to {}:{}", config_.charge_point_id, config_.host, config_.port);
        resolver_.async_resolve(
            config_.host, config_.port,
            beast::bind_front_handler(&OcppClient::on_resolve, shared_from_this()));
    }

    void OcppClient::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) {
            fail("resolve", ec);
            return;
        }
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(ws_).async_connect(
            results, beast::bind_front_handler(&OcppClient::on_connect, shared_from_this()));
    }

    void OcppClient::on_connect(beast::error_code ec, tcp::endpoint endpoint) {
        if (ec) {
            fail("connect", ec);
            return;
        }
        beast::get_lowest_layer(ws_).expires_never();  // the websocket has its own timeouts

        // Dead-peer detection: ping when idle, give up if nothing comes back for 120 s.
        // (The "suggested" client setting has no pings and would drop a connection whose
        // heartbeat interval is longer than its idle timeout.)
        websocket::stream_base::timeout timeouts{};
        timeouts.handshake_timeout = std::chrono::seconds(30);
        timeouts.idle_timeout = std::chrono::seconds(120);
        timeouts.keep_alive_pings = true;
        ws_.set_option(timeouts);

        // OCPP 1.6J requires the "ocpp1.6" subprotocol.
        ws_.set_option(websocket::stream_base::decorator([](websocket::request_type& request) {
            request.set(http::field::sec_websocket_protocol, "ocpp1.6");
            }));

        host_header_ = config_.host + ":" + std::to_string(endpoint.port());
        ws_.async_handshake(handshake_response_, host_header_, target_,
            beast::bind_front_handler(&OcppClient::on_handshake, shared_from_this()));
    }

    void OcppClient::on_handshake(beast::error_code ec) {
        if (ec) {
            fail("handshake", ec);
            return;
        }

        // The server must confirm the subprotocol, or it does not speak OCPP 1.6J.
        const auto protocol = handshake_response_[http::field::sec_websocket_protocol];
        if (std::string(protocol.data(), protocol.size()) != "ocpp1.6") {
            spdlog::error("[{}] the server did not accept the ocpp1.6 subprotocol",
                config_.charge_point_id);
            shutdown();
            return;
        }

        connected_ = true;
        spdlog::info("[{}] connected, subprotocol ocpp1.6", config_.charge_point_id);
        do_read();
        schedule_housekeeping();
        send_boot_notification();
    }

    // ---------- reading ----------

    void OcppClient::do_read() {
        ws_.async_read(buffer_, beast::bind_front_handler(&OcppClient::on_read, shared_from_this()));
    }

    void OcppClient::on_read(beast::error_code ec, std::size_t) {
        if (ec) {
            fail("read", ec);  // also lands here when the server closes: websocket::error::closed
            return;
        }

        const bool is_text = ws_.got_text();
        const std::string text = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());

        if (is_text) {
            handle_text(text);
        }
        else {
            spdlog::warn("[{}] ignoring a binary message", config_.charge_point_id);
        }

        if (!stopped_) {
            do_read();
        }
    }

    void OcppClient::handle_text(const std::string& text) {
        ocpp::Frame frame;
        try {
            frame = ocpp::parse(text);
        }
        catch (const ocpp::ParseError& e) {
            spdlog::warn("[{}] dropping unparseable frame: {}", config_.charge_point_id, e.what());
            return;
        }

        if (std::holds_alternative<ocpp::Call>(frame)) {
            // The CSMS is asking us to do something: same dispatcher as on the server side.
            if (auto reply = dispatcher_.handle(config_.charge_point_id, text)) {
                send_text(std::move(*reply));
            }
        }
        else if (!tracker_.resolve(frame)) {
            spdlog::warn("[{}] ignoring a reply that matches no pending call",
                config_.charge_point_id);
        }
    }

    // ---------- writing: Beast allows only ONE async_write at a time ----------

    void OcppClient::send_text(std::string text) {
        if (stopped_ || !connected_) {
            return;
        }
        write_queue_.push_back(std::move(text));
        if (!writing_) {
            do_write();
        }
    }

    void OcppClient::do_write() {
        writing_ = true;
        ws_.text(true);
        ws_.async_write(net::buffer(write_queue_.front()),
            beast::bind_front_handler(&OcppClient::on_write, shared_from_this()));
    }

    void OcppClient::on_write(beast::error_code ec, std::size_t) {
        if (ec) {
            fail("write", ec);
            return;
        }
        write_queue_.pop_front();
        if (!write_queue_.empty()) {
            do_write();
        }
        else {
            writing_ = false;
        }
    }

    // ---------- protocol ----------

    void OcppClient::do_call(std::string action, ocpp::json payload, ReplyCallback on_reply) {
        if (stopped_ || !connected_) {
            if (on_reply) {
                CallOutcome outcome;
                outcome.status = CallOutcome::Status::Disconnected;
                on_reply(outcome);
            }
            return;
        }
        const std::string id =
            tracker_.add(ocpp::CallTracker::Clock::now(), config_.call_timeout, std::move(on_reply));
        send_text(ocpp::serialize(ocpp::Call{ id, std::move(action), std::move(payload) }));
    }

    void OcppClient::send_boot_notification() {
        ocpp::BootNotificationRequest request;
        request.charge_point_vendor = config_.vendor;
        request.charge_point_model = config_.model;
        request.firmware_version = config_.firmware;

        spdlog::info("[{}] sending BootNotification", config_.charge_point_id);
        std::weak_ptr<OcppClient> weak = weak_from_this();
        do_call(ocpp::BootNotificationRequest::kAction, ocpp::json(request),
            [weak](const CallOutcome& outcome) {
                if (auto self = weak.lock()) {
                    self->on_boot_reply(outcome);
                }
            });
    }

    void OcppClient::on_boot_reply(const CallOutcome& outcome) {
        if (stopped_ || outcome.status == CallOutcome::Status::Disconnected) {
            return;
        }
        const auto& id = config_.charge_point_id;

        if (outcome.status != CallOutcome::Status::Result) {
            spdlog::warn("[{}] BootNotification failed ({} {}), retrying in {} s", id,
                ocpp::to_string(outcome.status), outcome.error_code,
                config_.boot_retry_interval.count());
            schedule_boot_retry(config_.boot_retry_interval);
            return;
        }

        ocpp::BootNotificationResponse response;
        try {
            response = ocpp::payload_as<ocpp::BootNotificationResponse>(outcome.payload);
        }
        catch (const ocpp::ParseError& e) {
            spdlog::warn("[{}] invalid BootNotification reply: {}", id, e.what());
            schedule_boot_retry(config_.boot_retry_interval);
            return;
        }

        if (response.status == ocpp::RegistrationStatus::Accepted) {
            if (response.interval > 0) {
                heartbeat_interval_ = std::chrono::seconds(response.interval);
            }
            spdlog::info("[{}] boot accepted, heartbeat every {} s", id, heartbeat_interval_.count());
            schedule_heartbeat();
        }
        else {
            // Pending or Rejected: OCPP says to wait and send BootNotification again.
            const std::chrono::seconds wait = response.interval > 0
                ? std::chrono::seconds(response.interval)
                : config_.boot_retry_interval;
            spdlog::warn("[{}] boot {}, retrying in {} s", id, ocpp::to_string(response.status),
                wait.count());
            schedule_boot_retry(wait);
        }
    }

    void OcppClient::schedule_boot_retry(std::chrono::seconds wait) {
        retry_timer_.expires_after(wait);
        retry_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (ec || self->stopped_) {
                return;  // cancelled
            }
            self->send_boot_notification();
            });
    }

    void OcppClient::send_heartbeat() {
        std::weak_ptr<OcppClient> weak = weak_from_this();
        do_call(ocpp::HeartbeatRequest::kAction, ocpp::json(ocpp::HeartbeatRequest{}),
            [weak](const CallOutcome& outcome) {
                if (auto self = weak.lock()) {
                    self->on_heartbeat_reply(outcome);
                }
            });
    }

    void OcppClient::on_heartbeat_reply(const CallOutcome& outcome) {
        const auto& id = config_.charge_point_id;
        if (outcome.status != CallOutcome::Status::Result) {
            spdlog::warn("[{}] heartbeat failed: {} {}", id, ocpp::to_string(outcome.status),
                outcome.error_code);
            return;
        }
        try {
            const auto response = ocpp::payload_as<ocpp::HeartbeatResponse>(outcome.payload);
            spdlog::info("[{}] heartbeat ok, server time {}", id, response.current_time);
        }
        catch (const ocpp::ParseError& e) {
            spdlog::warn("[{}] invalid heartbeat reply: {}", id, e.what());
        }
    }

    void OcppClient::schedule_heartbeat() {
        heartbeat_timer_.expires_after(heartbeat_interval_);
        heartbeat_timer_.async_wait(
            beast::bind_front_handler(&OcppClient::on_heartbeat_timer, shared_from_this()));
    }

    void OcppClient::on_heartbeat_timer(beast::error_code ec) {
        if (ec || stopped_) {
            return;  // cancelled
        }
        send_heartbeat();
        schedule_heartbeat();
    }

    // Once a second: fail the calls whose answer never came.
    void OcppClient::schedule_housekeeping() {
        housekeeping_timer_.expires_after(std::chrono::seconds(1));
        housekeeping_timer_.async_wait(
            beast::bind_front_handler(&OcppClient::on_housekeeping, shared_from_this()));
    }

    void OcppClient::on_housekeeping(beast::error_code ec) {
        if (ec || stopped_) {
            return;
        }
        tracker_.expire(ocpp::CallTracker::Clock::now());
        schedule_housekeeping();
    }

    // ---------- teardown ----------

    void OcppClient::fail(const char* what, beast::error_code ec) {
        if (stopped_) {
            return;  // we closed the socket ourselves: the aborted operations land here
        }
        if (ec == websocket::error::closed) {
            spdlog::info("[{}] connection closed by the server", config_.charge_point_id);
        }
        else {
            spdlog::error("[{}] {} failed: {}", config_.charge_point_id, what, ec.message());
        }
        shutdown();
    }

    void OcppClient::shutdown() {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        connected_ = false;

        heartbeat_timer_.cancel();
        retry_timer_.cancel();
        housekeeping_timer_.cancel();
        resolver_.cancel();

        // Closing the socket aborts any pending read or write.
        beast::error_code ignored;
        beast::get_lowest_layer(ws_).socket().close(ignored);

        // Anyone waiting for an answer learns that none will come.
        tracker_.fail_all(CallOutcome::Status::Disconnected);
        spdlog::info("[{}] client stopped", config_.charge_point_id);
    }

}  // namespace chargelink::sim