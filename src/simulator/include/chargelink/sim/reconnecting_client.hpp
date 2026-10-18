#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "chargelink/ocpp/caller.hpp"
#include "chargelink/sim/ocpp_client.hpp"
#include "chargelink/util/backoff.hpp"

namespace chargelink::sim {

    struct ReconnectOptions {
        std::chrono::milliseconds base_delay{ 1000 };
        std::chrono::milliseconds max_delay{ 60000 };
    };

    // Keeps a connection to the CSMS alive. Every attempt uses a brand-new OcppClient (a
    // WebSocket stream cannot be reused once it has closed); this class owns what must
    // survive between attempts: the backoff, the retry timer and the callbacks.
    //
    // It implements ICaller, so OcppLink can talk to it without knowing about reconnects:
    // calls made while there is no connection simply fail with "Disconnected".
    class ReconnectingClient : public ocpp::ICaller,
        public std::enable_shared_from_this<ReconnectingClient> {
    public:
        using ReplyCallback = ocpp::CallTracker::Callback;
        using Executor = OcppClient::Executor;

        static std::shared_ptr<ReconnectingClient> create(boost::asio::io_context& ioc,
            ClientConfig config,
            ReconnectOptions options = {});

        // The strand shared with every connection. Use it for anything that must not run
        // concurrently with the client.
        Executor executor() const { return strand_; }

        // Set these before start(). They run on the strand.
        void set_on_registered(std::function<void()> callback) { on_registered_ = std::move(callback); }
        void set_on_disconnected(std::function<void()> callback) { on_disconnected_ = std::move(callback); }
        void set_on_stopped(std::function<void()> callback) { on_stopped_ = std::move(callback); }

        void start();  // connect, and keep reconnecting until stop()
        void stop();   // close the connection and stop retrying; on_stopped runs when done

        void call(std::string action, ocpp::json payload, ReplyCallback on_reply) override;

    private:
        ReconnectingClient(boost::asio::io_context& ioc, ClientConfig config, ReconnectOptions options);

        void connect();
        void on_connection_registered(unsigned generation);
        void on_connection_closed(unsigned generation);
        void do_stop();

        Executor strand_;
        ClientConfig config_;
        util::Backoff backoff_;
        boost::asio::steady_timer retry_timer_;

        std::shared_ptr<OcppClient> current_;  // null between attempts
        unsigned generation_ = 0;              // identifies the current connection
        bool running_ = false;
        bool stopped_ = false;                 // stop() was called: no more attempts

        std::function<void()> on_registered_;
        std::function<void()> on_disconnected_;
        std::function<void()> on_stopped_;
    };

}  // namespace chargelink::sim