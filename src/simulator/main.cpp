#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <spdlog/spdlog.h>

#include <string>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

int main() {
    const std::string host = "127.0.0.1";
    const std::string port = "9000";
    const std::string cp_id = "CP001";

    try {
        net::io_context ioc;
        tcp::resolver resolver{ ioc };
        websocket::stream<tcp::socket> ws{ ioc };

        // TODO 1: resolve host/port into endpoints
        auto const results = resolver.resolve(host, port);

        // TODO 2: connect ws.next_layer() to those endpoints
        net::connect(ws.next_layer(), results);

        // TODO 3: set a decorator that adds "ocpp1.6" to the REQUEST header
        ws.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::sec_websocket_protocol, "ocpp1.6");
            }));
         
        // 4. Do the websocket handshake: GET /ocpp/CP001
        const std::string target = "/ocpp/" + cp_id;
        ws.handshake(host + ":" + port, target);
        spdlog::info("connected to ws://{}:{}{}", host, port, target);

        // 5. Send one OCPP CALL frame (a Heartbeat)
        const std::string msg = R"([2,"1","Heartbeat",{}])";
        ws.write(net::buffer(msg));
        spdlog::info("tx: {}", msg);

        // 6. Read the reply (our server echoes it back)
        beast::flat_buffer buffer;
        ws.read(buffer);
        spdlog::info("rx: {}", beast::buffers_to_string(buffer.data()));

        // 7. Close cleanly
        ws.close(websocket::close_code::normal);
        spdlog::info("closed");
    }
    catch (std::exception const& e) {
        spdlog::critical("fatal: {}", e.what());
        return 1;
    }
    return 0;
}