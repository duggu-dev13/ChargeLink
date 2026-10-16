#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <spdlog/spdlog.h>

#include <thread>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

void do_session(tcp::socket socket) {
    try {
        websocket::stream<tcp::socket> ws{ std::move(socket) };

        // TODO A: runs while the handshake response is being built,
        // so it must be set BEFORE accept()
        ws.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(http::field::sec_websocket_protocol, "ocpp1.6");
            }));

        // TODO B: reads the client's HTTP upgrade request and replies
        ws.accept();
        spdlog::info("client connected");

        // TODO C: echo loop
        for (;;) {
            beast::flat_buffer buffer;
            ws.read(buffer);  // blocks until a full message arrives
            spdlog::info("rx: {}", beast::buffers_to_string(buffer.data()));

            ws.text(ws.got_text());   // reply with the same frame type (text/binary)
            ws.write(buffer.data());  // echo
        }
    }
    catch (beast::system_error const& se) {
        // TODO D: a normal client close is reported as an error code,
        // not as a failure
        if (se.code() == websocket::error::closed) {
            spdlog::info("client disconnected");
        }
        else {
            spdlog::error("session error: {}", se.code().message());
        }
    }
    catch (std::exception const& e) {
        spdlog::error("session exception: {}", e.what());
    }
}

int main() {
    try {
        // TODO E: listen on port 9000 (IPv4)
        net::io_context ioc;
        tcp::acceptor acceptor{ ioc,{ tcp::v4(), 9000 } };
        spdlog::info("CSMS echo server listening on port 9000");

        // TODO F: accept loop, one thread per connection
        for (;;) {
            tcp::socket socket{ ioc };
            acceptor.accept(socket);  // blocks until a client connects
            std::thread{ &do_session, std::move(socket) }.detach();
        }
    }
    catch (std::exception const& e) {
        spdlog::critical("fatal: {}", e.what());
        return 1;
    }
}