#pragma once

#include "chargelink/ocpp/dispatcher.hpp"

namespace chargelink::csms {

	// Shared by every session thread and only read after creation: handlers must be thread-safe.
	ocpp::Dispatcher make_dispatcher();

}  // namespace chargelink::csms