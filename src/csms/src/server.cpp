#include "chargelink/csms/server.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/system_error.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

#include "chargelink/csms/session.hpp"

namespace chargelink::csms {

    namespace net = boost::asio;
    namespace beast = boost::beast;
    using tcp = net::ip::tcp;

    std::shared_ptr<Server> Server::create(net::io_context& ioc, tcp::endpoint endpoint,
        std::shared_ptr<const ocpp::Dispatcher> dispatcher) {
        return std::shared_ptr<Server>(new Server(ioc, endpoint, std::move(dispatcher)));
    }

    Server::Server(net::io_context& ioc, tcp::endpoint endpoint,
        std::shared_ptr<const ocpp::Dispatcher> dispatcher)
        : ioc_(ioc),
        endpoint_(endpoint),
        acceptor_(net::make_strand(ioc)),
        retry_timer_(acceptor_.get_executor()),
        dispatcher_(std::move(dispatcher)) {
    }

    void Server::start() {
        beast::error_code ec;
        acceptor_.open(endpoint_.protocol(), ec);
        if (ec) throw boost::system::system_error(ec, "open");

#ifndef _WIN32
        // Not on Windows: there SO_REUSEADDR lets ANOTHER process bind the same port.
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) throw boost::system::system_error(ec, "set_option");
#endif

        acceptor_.bind(endpoint_, ec);
        if (ec) throw boost::system::system_error(ec, "bind");
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) throw boost::system::system_error(ec, "listen");

        do_accept();
    }

    void Server::stop() {
        net::post(acceptor_.get_executor(), [self = shared_from_this()] {
            if (self->stopping_) {
                return;
            }
            self->stopping_ = true;
            beast::error_code ignored;
            self->acceptor_.close(ignored);
            self->retry_timer_.cancel();
            for (auto& session : self->registry_->close_all()) {
                session->close();
            }
            });
    }

    void Server::do_accept() {
        // Every connection gets its own strand: that is what lets sessions run in parallel
        // while each one stays single-threaded.
        acceptor_.async_accept(net::make_strand(ioc_),
            beast::bind_front_handler(&Server::on_accept, shared_from_this()));
    }

    void Server::on_accept(beast::error_code ec, tcp::socket socket) {
        if (stopping_) {
            return;
        }
        if (ec) {
            spdlog::warn("accept failed: {}; retrying shortly", ec.message());
            retry_timer_.expires_after(std::chrono::milliseconds(200));  // no hot loop on a persistent error
            retry_timer_.async_wait([self = shared_from_this()](beast::error_code wait_ec) {
                if (!wait_ec && !self->stopping_) {
                    self->do_accept();
                }
                });
            return;
        }
        std::make_shared<Session>(std::move(socket), dispatcher_, registry_)->run();
        do_accept();
    }

}  // namespace chargelink::csms