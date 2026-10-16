#pragma once

#include <string>

namespace chargelink::util {

	// Current UTC time as ISO 8601, e.g. "2026-10-16T12:40:06Z"
	std::string now_utc_iso8601();

}  // namespace chargelink::util
