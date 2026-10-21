#include "chargelink/csms/session.hpp"

#include <boost/asio/dispatch.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <string_view>
#include <utility>
#include <vector>

#include "chargelink/csms/path.hpp"

namespace chargelink::csms {

    namespace net = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace websocket = beast::websocket;
    using tcp = net::ip::tcp;

    namespace {

        std::string join(const std::vector<std::string>& items) {
            std::string out;
            for (const auto& item : items) {
                out += out.empty() ? item : ", " + item;
            }
            return out;
        }

    }  // namespace

    Session::Session(tcp::socket socket, std::shared_ptr<const ocpp::Dispatcher> dispatcher,
        std::shared_ptr<ChargerRegistry> registry)
        : ws_(std::move(socket)), dispatcher_(std::move(dispatcher)), registry_(std::move(registry)) {
    }

    void Session::run() {
        net::dispatch(ws_.get_executor(),
            beast::bind_front_handler(&Session::do_read_request, shared_from_this()));
    }

    // ---------- handshake: read the HTTP upgrade request, validate it, accept ----------

    void Session::do_read_request() {
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
        http::async_read(ws_.next_layer(), buffer_, request_,
            beast::bind_front_handler(&Session::on_read_request, shared_from_this()));
    }

    void Session::on_read_request(beast::error_code ec, std::size_t) {
        if (ec) {
            fail("reading the request", ec);
            return;
        }
        if (!websocket::is_upgrade(request_)) {
            spdlog::warn("rejecting a request that is not a websocket upgrade");
            shutdown();
            return;
        }

        const auto target = request_.target();
        const std::string_view target_view(target.data(), target.size());
        const auto parsed = parse_charge_point_path(target_view);
        if (!parsed) {
            spdlog::warn("rejecting bad path '{}'", safe_for_log(target_view));
            shutdown();
            return;
        }

        const auto offered = request_[http::field::sec_websocket_protocol];
        if (!offers_subprotocol(std::string_view(offered.data(), offered.size()), "ocpp1.6")) {
            spdlog::warn("[{}] rejecting: the ocpp1.6 subprotocol was not offered", *parsed);
            shutdown();
            return;
        }

        id_ = *parsed;
        beast::get_lowest_layer(ws_).expires_never();
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(websocket::stream_base::decorator([](websocket::response_type& response) {
            response.set(http::field::sec_websocket_protocol, "ocpp1.6");
            }));
        ws_.read_message_max(64 * 1024);  // OCPP messages are small: refuse anything huge
        ws_.async_accept(request_, beast::bind_front_handler(&Session::on_accept, shared_from_this()));
    }

    void Session::on_accept(beast::error_code ec) {
        if (ec) {
            fail("handshake", ec);
            return;
        }
        const auto added = registry_->add(shared_from_this());
        if (!added.accepted) {
            spdlog::info("[{}] refused: the server is shutting down", id_);
            shutdown();
            return;
        }
        registered_ = true;
        if (added.replaced) {
            spdlog::warn("[{}] replacing an older connection", id_);
            added.replaced->close();  // outside the registry's lock
        }
        spdlog::info("[{}] connected; online: {}", id_, join(registry_->ids()));
        do_read();
    }

    // ---------- reading ----------

    void Session::do_read() {
        ws_.async_read(buffer_, beast::bind_front_handler(&Session::on_read, shared_from_this()));
    }

    void Session::on_read(beast::error_code ec, std::size_t) {
        if (ec) {
            fail("read", ec);
            return;
        }
        const bool is_text = ws_.got_text();
        const std::string text = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());

        if (is_text) {
            spdlog::debug("[{}] rx: {}", id_, safe_for_log(text, 300));
            if (auto reply = dispatcher_->handle(id_, text)) {
                spdlog::debug("[{}] tx: {}", id_, safe_for_log(*reply, 300));
                send(std::move(*reply));
            }
        }
        else {
            spdlog::warn("[{}] ignoring a binary message", id_);
        }

        if (!shut_down_) {
            do_read();  // keep reading while closing: the close handshake needs it
        }
    }

    // ---------- writing: one async_write at a time ----------

    void Session::send(std::string text) {
        if (shut_down_ || closing_) {
            return;
        }
        write_queue_.push_back(std::move(text));
        if (!writing_) {
            do_write();
        }
    }

    void Session::do_write() {
        writing_ = true;
        ws_.text(true);
        ws_.async_write(net::buffer(write_queue_.front()),
            beast::bind_front_handler(&Session::on_write, shared_from_this()));
    }

    void Session::on_write(beast::error_code ec, std::size_t) {
        if (ec) {
            fail("write", ec);
            return;
        }
        write_queue_.pop_front();
        if (!write_queue_.empty()) {
            do_write();
            return;
        }
        writing_ = false;
        if (closing_) {
            start_close();  // a close was requested while we were writing
        }
    }

    // ---------- closing and teardown ----------

    void Session::close() {
        net::dispatch(ws_.get_executor(), [self = shared_from_this()] { self->do_close(); });
    }

    void Session::do_close() {
        if (closing_ || shut_down_) {
            return;
        }
        closing_ = true;
        if (!writing_) {
            start_close();  // a close counts as a write: never overlap it with one
        }
    }

    void Session::start_close() {
        ws_.async_close(websocket::close_code::going_away, [self = shared_from_this()](beast::error_code ec) {
            if (ec) {
                self->fail("close", ec);
            }
            });
    }

    void Session::fail(const char* what, beast::error_code ec) {
        if (shut_down_) {
            return;  // we closed the socket ourselves: aborted operations land here
        }
        const std::string who = id_.empty() ? "?" : id_;
        if (ec == websocket::error::closed || ec == http::error::end_of_stream) {
            spdlog::info("[{}] disconnected", who);
        }
        else {
            spdlog::warn("[{}] {} failed: {}", who, what, ec.message());
        }
        shutdown();
    }

    void Session::shutdown() {
        if (shut_down_) {
            return;
        }
        shut_down_ = true;
        if (registered_) {
            registered_ = false;
            registry_->remove(*this);
            spdlog::info("[{}] gone; online: {}", id_, join(registry_->ids()));
        }
        beast::error_code ignored;
        beast::get_lowest_layer(ws_).socket().close(ignored);  // aborts any pending operation
    }

}  // namespace chargelink::csms