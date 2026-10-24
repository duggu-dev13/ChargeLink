#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <cstddef>
#include <deque>
#include <memory>
#include <string>

#include "chargelink/csms/charger_registry.hpp"
#include "chargelink/csms/lifecycle.hpp"
#include "chargelink/ocpp/dispatcher.hpp"

namespace chargelink::csms {

    // One connected charge point. Its socket has its own strand, so its handlers never run
    // concurrently. Kept alive by its own pending operations.
    class Session : public ISession, public std::enable_shared_from_this<Session> {
    public:
        Session(boost::asio::ip::tcp::socket socket, std::shared_ptr<const ocpp::Dispatcher> dispatcher,
            std::shared_ptr<ChargerRegistry> registry,
            std::shared_ptr<const ConnectionHooks> hooks = nullptr);

        void run();
        const std::string& id() const override { return id_; }  // set once, before registration
        void close() override;                                  // any thread

    private:
        void do_read_request();
        void on_read_request(boost::beast::error_code ec, std::size_t bytes);
        void on_accept(boost::beast::error_code ec);
        void do_read();
        void on_read(boost::beast::error_code ec, std::size_t bytes);
        void send(std::string text);
        void do_write();
        void on_write(boost::beast::error_code ec, std::size_t bytes);
        void do_close();
        void start_close();
        void fail(const char* what, boost::beast::error_code ec);
        void shutdown();

        boost::beast::websocket::stream<boost::beast::tcp_stream> ws_;
        boost::beast::flat_buffer buffer_;
        boost::beast::http::request<boost::beast::http::string_body> request_;
        std::string id_;
        std::shared_ptr<const ocpp::Dispatcher> dispatcher_;
        std::shared_ptr<ChargerRegistry> registry_;
        std::shared_ptr<const ConnectionHooks> hooks_;
        std::deque<std::string> write_queue_;
        bool writing_ = false;
        bool registered_ = false;
        bool closing_ = false;
        bool shut_down_ = false;
    };

}  // namespace chargelink::csms