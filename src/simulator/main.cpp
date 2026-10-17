// TEMPORARY CSMS: thread per connection, answers BootNotification and Heartbeat.
// Replaced by the async server in Phase 3.

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>
#include <thread>

#include "chargelink/ocpp/dispatcher.hpp"
#include "chargelink/ocpp/messages.hpp"
#include "chargelink/util/time.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

    chargelink::ocpp::Dispatcher make_dispatcher() {
        using namespace chargelink::ocpp;
        Dispatcher dispatcher;

        dispatcher.on<BootNotificationRequest, BootNotificationResponse>(
            [](const std::string& cp_id, const BootNotificationRequest& request) {
                spdlog::info("[{}] BootNotification from {} {}", cp_id, request.charge_point_vendor,
                    request.charge_point_model);
                BootNotificationResponse response;
                response.status = RegistrationStatus::Accepted;
                response.current_time = chargelink::util::now_utc_iso8601();
                response.interval = 5;  // short on purpose, so heartbeats are easy to watch
                return response;
            });

        dispatcher.on<HeartbeatRequest, HeartbeatResponse>(
            [](const std::string& cp_id, const HeartbeatRequest&) {
                spdlog::info("[{}] Heartbeat", cp_id);
                HeartbeatResponse response;
                response.current_time = chargelink::util::now_utc_iso8601();
                return response;
            });

        return dispatcher;
    }

    void do_session(tcp::socket socket, const chargelink::ocpp::Dispatcher* dispatcher) {
        try {
            websocket::stream<tcp::socket> ws{ std::move(socket) };

            // Read the HTTP upgrade request ourselves, so we can look at its path and headers.
            beast::flat_buffer http_buffer;
            http::request<http::string_body> request;
            http::read(ws.next_layer(), http_buffer, request);

            if (!websocket::is_upgrade(request)) {
                spdlog::warn("rejecting a request that is not a websocket upgrade");
                return;
            }

            // The path must be /ocpp/<chargePointId>.
            const std::string prefix = "/ocpp/";
            const std::string target(request.target().data(), request.target().size());
            if (target.size() <= prefix.size() || target.compare(0, prefix.size(), prefix) != 0) {
                spdlog::warn("rejecting bad path '{}'", target);
                return;
            }
            const std::string cp_id = target.substr(prefix.size());

            // The client must offer the ocpp1.6 subprotocol.
            const auto offered = request[http::field::sec_websocket_protocol];
            if (std::string(offered.data(), offered.size()).find("ocpp1.6") == std::string::npos) {
                spdlog::warn("[{}] rejecting: no ocpp1.6 subprotocol offered", cp_id);
                return;
            }

            ws.set_option(websocket::stream_base::decorator([](websocket::response_type& response) {
                response.set(http::field::sec_websocket_protocol, "ocpp1.6");
                }));
            ws.accept(request);
            spdlog::info("[{}] connected", cp_id);

            for (;;) {
                beast::flat_buffer buffer;
                ws.read(buffer);
                const std::string text = beast::buffers_to_string(buffer.data());
                spdlog::info("[{}] rx: {}", cp_id, text);

                if (auto reply = dispatcher->handle(cp_id, text)) {
                    ws.text(true);
                    ws.write(net::buffer(*reply));
                    spdlog::info("[{}] tx: {}", cp_id, *reply);
                }
            }
        }
        catch (const beast::system_error& e) {
            if (e.code() == websocket::error::closed) {
                spdlog::info("client disconnected");
            }
            else {
                spdlog::warn("session ended: {}", e.code().message());
            }
        }
        catch (const std::exception& e) {
            spdlog::error("session exception: {}", e.what());
        }
    }

}  // namespace

int main(int argc, char** argv) {
    const unsigned short port = argc > 1 ? static_cast<unsigned short>(std::atoi(argv[1])) : 9000;

    try {
        const chargelink::ocpp::Dispatcher dispatcher = make_dispatcher();  // read-only from now on

        net::io_context ioc;
        tcp::acceptor acceptor{ ioc,{ tcp::v4(), port } };
        spdlog::info("temporary CSMS listening on port {}", port);

        for (;;) {
            tcp::socket socket{ ioc };
            acceptor.accept(socket);
            std::thread{ &do_session, std::move(socket), &dispatcher }.detach();
        }
    }
    catch (const std::exception& e) {
        spdlog::critical("fatal: {}", e.what());
        return 1;
    }
}