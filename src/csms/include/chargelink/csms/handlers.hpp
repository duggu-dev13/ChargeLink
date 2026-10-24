#pragma once

#include <memory>

#include "chargelink/csms/lifecycle.hpp"
#include "chargelink/ocpp/dispatcher.hpp"

namespace chargelink::db { class Repository; }

namespace chargelink::csms {

    struct CsmsHandlers {
        ocpp::Dispatcher dispatcher;
        ConnectionHooks hooks;
    };

    // What the server uses. If the environment variable CHARGELINK_DB (a libpq connection
    // string) is set it persists everything in PostgreSQL, with connection hooks, and exits if
    // the database cannot be reached. If it is not set, it falls back to the in-memory handlers
    // (no hooks).
    CsmsHandlers make_csms();

    ocpp::Dispatcher make_dispatcher();  // make_csms().dispatcher, kept for older callers
    ocpp::Dispatcher make_memory_dispatcher();
    CsmsHandlers make_db_handlers(std::shared_ptr<db::Repository> repo);
    ocpp::Dispatcher make_db_dispatcher(std::shared_ptr<db::Repository> repo);

}  // namespace chargelink::csms