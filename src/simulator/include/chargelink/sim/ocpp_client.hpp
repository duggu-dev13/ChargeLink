#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>

#include "chargelink/ocpp/call_tracker.hpp"
#include "chargelink/ocpp/dispatcher.hpp"
#include "chargelink/ocpp/messages.hpp"

namespace chargelink::sim {

    struct ClientConfig {
        std::string host = "127.0.0.1";
        std::string port = "9000";
        std::string charge_point_id = "CP001";

        std::string vendor = "SCharge";
        std::string model = "A1-7.4kW";
        std::string firmware = "0.1.0";

        std::chrono::seconds default_heartbeat_interval{ 60 };  // used when the CSMS sends 0
        std::chrono::seconds boot_retry_interval{ 30 };         // used when we are not accepted and the CSMS sends 0
        std::chrono::milliseconds call_timeout{ 30000 };
    };

    // Connects to a CSMS, registers with BootNotification, then sends heartbeats.
    // Everything runs on one strand, so no locks are needed.
    class OcppClient : public std::enable_shared_from_this<OcppClient> {
    public:
        using ReplyCallback = ocpp::CallTracker::Callback;

        // Always create through this function: the client must be owned by a shared_ptr.
        static std::shared_ptr<OcppClient> create(boost::asio::io_context& ioc, ClientConfig config);

        // Handlers for CALLs sent BY the CSMS (RemoteStartTransaction, Reset, ...).
        // Register them before start(): this is not synchronised with the network strand.
        ocpp::Dispatcher& dispatcher() { return dispatcher_; }

        void start();  // resolve, connect, handshake, BootNotification, heartbeats
        void stop();   // drop the connection and cancel everything

        // Send a CALL and be told what happened to it. Safe to call from any thread.
        void call(std::string action, ocpp::json payload, ReplyCallback on_reply);

    private:
        using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;
        using Tcp = boost::asio::ip::tcp;

        OcppClient(boost::asio::io_context& ioc, ClientConfig config);

        // connection chain
        void do_resolve();
        void on_resolve(boost::beast::error_code ec, Tcp::resolver::results_type results);
        void on_connect(boost::beast::error_code ec, Tcp::endpoint endpoint);
        void on_handshake(boost::beast::error_code ec);

        // reading
        void do_read();
        void on_read(boost::beast::error_code ec, std::size_t bytes);
        void handle_text(const std::string& text);

        // writing (one async_write at a time)
        void send_text(std::string text);
        void do_write();
        void on_write(boost::beast::error_code ec, std::size_t bytes);

        // protocol
        void do_call(std::string action, ocpp::json payload, ReplyCallback on_reply);
        void send_boot_notification();
        void on_boot_reply(const ocpp::CallOutcome& outcome);
        void schedule_boot_retry(std::chrono::seconds wait);
        void send_heartbeat();
        void on_heartbeat_reply(const ocpp::CallOutcome& outcome);
        void schedule_heartbeat();
        void on_heartbeat_timer(boost::beast::error_code ec);
        void schedule_housekeeping();
        void on_housekeeping(boost::beast::error_code ec);

        // teardown
        void fail(const char* what, boost::beast::error_code ec);
        void shutdown();

        Strand strand_;
        Tcp::resolver resolver_;
        boost::beast::websocket::stream<boost::beast::tcp_stream> ws_;
        boost::beast::flat_buffer buffer_;
        boost::beast::websocket::response_type handshake_response_;
        boost::asio::steady_timer heartbeat_timer_;
        boost::asio::steady_timer retry_timer_;
        boost::asio::steady_timer housekeeping_timer_;

        ClientConfig config_;
        std::string host_header_;  // kept alive for the async handshake
        std::string target_;       // "/ocpp/<id>", also kept alive for the handshake

        ocpp::CallTracker tracker_;
        ocpp::Dispatcher dispatcher_;

        std::deque<std::string> write_queue_;
        bool writing_ = false;
        bool connected_ = false;
        bool stopped_ = false;
        std::chrono::seconds heartbeat_interval_{ 60 };
    };

}  // namespace chargelink::sim