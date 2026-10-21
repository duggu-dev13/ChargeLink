#pragma once

#include <memory>

#include "chargelink/ocpp/dispatcher.hpp"

namespace chargelink::db { class Repository; }

namespace chargelink::csms {

	// The one the server uses. If the environment variable CHARGELINK_DB (a libpq connection
	// string) is set it persists everything in PostgreSQL, and exits if the database cannot be
	// reached. If it is not set, it falls back to the in-memory handlers.
	ocpp::Dispatcher make_dispatcher();

	ocpp::Dispatcher make_memory_dispatcher();
	ocpp::Dispatcher make_db_dispatcher(std::shared_ptr<db::Repository> repo);

}  // namespace chargelink::csms