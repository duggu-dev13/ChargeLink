#include "chargelink/sim/reconnecting_client.hpp"

#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>
#include <spdlog/spdlog.h>

#include <utility>

namespace chargelink::sim {

    namespace net = boost::asio;

    std::shared_ptr<ReconnectingClient> ReconnectingClient::create(net::io_context& ioc,
        ClientConfig config,
        ReconnectOptions options) {
        return std::shared_ptr<ReconnectingClient>(
            new ReconnectingClient(ioc, std::move(config), options));
    }

    ReconnectingClient::ReconnectingClient(net::io_context& ioc, ClientConfig config,
        ReconnectOptions options)
        : strand_(net::make_strand(ioc)),
        config_(std::move(config)),
        backoff_(options.base_delay, options.max_delay, util::make_random_source()),
        retry_timer_(strand_) {
    }

    // ---------- public API (each hops onto the strand) ----------

    void ReconnectingClient::start() {
        net::post(strand_, [self = shared_from_this()] {
            if (self->running_ || self->stopped_) {
                return;
            }
            self->running_ = true;
            self->connect();
            });
    }

    void ReconnectingClient::stop() {
        net::post(strand_, [self = shared_from_this()] { self->do_stop(); });
    }

    void ReconnectingClient::call(std::string action, ocpp::json payload, ReplyCallback on_reply) {
        net::post(strand_, [self = shared_from_this(), action = std::move(action),
            payload = std::move(payload), on_reply = std::move(on_reply)]() mutable {
                if (!self->current_) {
                    // Between attempts: nobody to talk to.
                    if (on_reply) {
                        ocpp::CallOutcome outcome;
                        outcome.status = ocpp::CallOutcome::Status::Disconnected;
                        on_reply(outcome);
                    }
                    return;
                }
                self->current_->call(std::move(action), std::move(payload), std::move(on_reply));
            });
    }

    // ---------- one connection attempt ----------

    void ReconnectingClient::connect() {
        if (stopped_) {
            return;
        }
        const unsigned generation = ++generation_;
        current_ = OcppClient::create(strand_, config_);

        // The callbacks remember which connection they belong to, so a late callback from an
        // old connection can never disturb the current one.
        std::weak_ptr<ReconnectingClient> weak = weak_from_this();
        current_->set_on_registered([weak, generation] {
            if (auto self = weak.lock()) {
                self->on_connection_registered(generation);
            }
            });
        current_->set_on_closed([weak, generation] {
            if (auto self = weak.lock()) {
                self->on_connection_closed(generation);
            }
            });
        current_->start();
    }

    void ReconnectingClient::on_connection_registered(unsigned generation) {
        if (generation != generation_) {
            return;
        }
        backoff_.reset();  // a real, working connection: the next failure starts from 1 s again
        if (on_registered_) {
            on_registered_();
        }
    }

    void ReconnectingClient::on_connection_closed(unsigned generation) {
        if (generation != generation_) {
            return;  // a stale connection
        }

        // We are being called from inside the dead connection's shutdown(). Keep it alive a
        // little longer and let the strand destroy it afterwards.
        net::post(strand_, [finished = std::move(current_)] {});
        current_.reset();

        if (on_disconnected_) {
            on_disconnected_();
        }

        if (stopped_) {
            if (on_stopped_) {
                on_stopped_();
            }
            return;
        }

        const auto delay = backoff_.next();
        spdlog::warn("[{}] not connected; next attempt in {} ms", config_.charge_point_id,
            delay.count());
        retry_timer_.expires_after(delay);
        retry_timer_.async_wait([self = shared_from_this()](boost::system::error_code ec) {
            if (ec || self->stopped_) {
                return;  // cancelled
            }
            self->connect();
            });
    }

    // ---------- stopping ----------

    void ReconnectingClient::do_stop() {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        retry_timer_.cancel();

        if (current_) {
            current_->stop();  // ends in on_connection_closed, which reports on_stopped
        }
        else if (on_stopped_) {
            on_stopped_();     // we were waiting between attempts: nothing to close
        }
    }

}  // namespace chargelink::sim