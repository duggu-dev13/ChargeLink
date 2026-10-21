#include "chargelink/csms/path.hpp"

namespace chargelink::csms {

    std::optional<std::string> parse_charge_point_path(std::string_view target) {
        constexpr std::string_view kPrefix = "/ocpp/";
        constexpr std::size_t kMaxIdLength = 48;

        if (target.substr(0, kPrefix.size()) != kPrefix) {
            return std::nullopt;
        }
        const std::string_view id = target.substr(kPrefix.size());
        if (id.empty() || id.size() > kMaxIdLength) {
            return std::nullopt;
        }
        for (const char c : id) {
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
                c == '~';
            if (!allowed) {
                return std::nullopt;
            }
        }
        return std::string(id);
    }

    bool offers_subprotocol(std::string_view header_value, std::string_view protocol) {
        while (!header_value.empty()) {
            const std::size_t comma = header_value.find(',');
            std::string_view token = header_value.substr(0, comma);
            header_value = comma == std::string_view::npos ? std::string_view{}
            : header_value.substr(comma + 1);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
                token.remove_prefix(1);
            }
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
                token.remove_suffix(1);
            }
            if (token == protocol) {
                return true;
            }
        }
        return false;
    }

    std::string safe_for_log(std::string_view text, std::size_t max_length) {
        std::string out;
        const std::size_t count = text.size() < max_length ? text.size() : max_length;
        out.reserve(count + 3);
        for (std::size_t i = 0; i < count; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            out.push_back(c >= 0x20 && c <= 0x7E ? static_cast<char>(c) : '?');
        }
        if (text.size() > max_length) {
            out += "...";
        }
        return out;
    }

}  // namespace chargelink::csms