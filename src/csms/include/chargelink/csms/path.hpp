#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace chargelink::csms {

	// "/ocpp/CP001" -> "CP001". Strict: 1 to 48 characters from [A-Za-z0-9._~-].
	std::optional<std::string> parse_charge_point_path(std::string_view target);

	// Does the Sec-WebSocket-Protocol header (a comma-separated list) offer exactly this protocol?
	bool offers_subprotocol(std::string_view header_value, std::string_view protocol);

	// Network text made safe to print: control characters become '?', long text is cut.
	std::string safe_for_log(std::string_view text, std::size_t max_length = 64);

}  // namespace chargelink::csms