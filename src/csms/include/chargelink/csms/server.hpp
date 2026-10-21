#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>

#include <memory>

#include "chargelink/csms/charger_registry.hpp"
#include "chargelink/ocpp/dispatcher.hpp"

namespace chargelink::csms {

    class Server : public std::enable_shared_from_this<Server> {
    public:
        static std::shared_ptr<Server> create(boost::asio::io_context& ioc,
            boost::asio::ip::tcp::endpoint endpoint,
            std::shared_ptr<const ocpp::Dispatcher> dispatcher);

        void start();  // throws boost::system::system_error if the port cannot be opened
        void stop();   // stop accepting and close every connection; any thread

    private:
        Server(boost::asio::io_context& ioc, boost::asio::ip::tcp::endpoint endpoint,
            std::shared_ptr<const ocpp::Dispatcher> dispatcher);

        void do_accept();
        void on_accept(boost::beast::error_code ec, boost::asio::ip::tcp::socket socket);

        boost::asio::io_context& ioc_;
        boost::asio::ip::tcp::endpoint endpoint_;
        boost::asio::ip::tcp::acceptor acceptor_;
        boost::asio::steady_timer retry_timer_;
        std::shared_ptr<const ocpp::Dispatcher> dispatcher_;
        std::shared_ptr<ChargerRegistry> registry_ = std::make_shared<ChargerRegistry>();
        bool stopping_ = false;  // only touched on the acceptor's strand
    };

}  // namespace chargelink::csms