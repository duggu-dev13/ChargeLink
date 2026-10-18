#include "chargelink/sim/console.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace chargelink::sim {

    namespace {

        Command invalid(const std::string& message) {
            Command c;
            c.type = CommandType::Invalid;
            c.text = message;
            return c;
        }

        // Accepts only a complete, finite number: "16", "16.5", "-3". Rejects "16A", "", "nan".
        bool parse_number(const std::string& text, double& out) {
            if (text.empty()) {
                return false;
            }
            char* end = nullptr;
            const double value = std::strtod(text.c_str(), &end);
            if (end != text.c_str() + text.size() || !std::isfinite(value)) {
                return false;
            }
            out = value;
            return true;
        }

        std::string lower(std::string text) {
            for (char& ch : text) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return text;
        }

    }  // namespace

    Command parse_command(const std::string& line) {
        std::istringstream stream(line);
        std::vector<std::string> words;
        for (std::string word; stream >> word;) {
            words.push_back(word);
        }
        if (words.empty()) {
            return Command{};  // Empty
        }

        const std::string name = lower(words[0]);
        const std::size_t args = words.size() - 1;

        // Commands that take no arguments.
        struct Simple {
            const char* name;
            CommandType type;
        };
        static const Simple kSimple[] = {
            { "help", CommandType::Help },{ "?", CommandType::Help },
            { "unplug", CommandType::Unplug },{ "pause", CommandType::Pause },
            { "resume", CommandType::Resume },{ "fault", CommandType::Fault },
            { "clear", CommandType::Clear },{ "status", CommandType::Status },
            { "quit", CommandType::Quit },{ "exit", CommandType::Quit },
        };
        for (const Simple& simple : kSimple) {
            if (name == simple.name) {
                if (args != 0) {
                    return invalid("'" + name + "' takes no arguments");
                }
                Command c;
                c.type = simple.type;
                return c;
            }
        }

        if (name == "plug") {  // plug [amps]  (the most the car can draw)
            if (args > 1) return invalid("usage: plug [amps]");
            Command c;
            c.type = CommandType::Plug;
            if (args == 1) {
                double amps = 0.0;
                if (!parse_number(words[1], amps) || amps < 6.0 || amps > 80.0) {
                    return invalid("the car current must be a number between 6 and 80 A");
                }
                c.number = amps;
                c.has_number = true;
            }
            return c;
        }

        if (name == "tap") {  // tap [tag]
            if (args > 1) return invalid("usage: tap [tag]");
            Command c;
            c.type = CommandType::Tap;
            c.text = args == 1 ? words[1] : "TAG1";
            if (c.text.size() > 20) {
                return invalid("an id tag has at most 20 characters");
            }
            return c;
        }

        if (name == "heat" || name == "speed") {  // heat <celsius>   speed <factor>
            if (args != 1) return invalid("usage: " + name + " <number>");
            double value = 0.0;
            if (!parse_number(words[1], value)) {
                return invalid("'" + words[1] + "' is not a number");
            }
            Command c;
            c.number = value;
            c.has_number = true;
            if (name == "heat") {
                if (value < -40.0 || value > 150.0) {
                    return invalid("the ambient temperature must be between -40 and 150 C");
                }
                c.type = CommandType::Heat;
            }
            else {
                if (value < 1.0 || value > 3600.0) {
                    return invalid("the speed must be between 1 and 3600");
                }
                c.type = CommandType::Speed;
            }
            return c;
        }

        return invalid("unknown command '" + words[0] + "' (type 'help')");
    }

    const char* help_text() {
        return "commands:\n"
            "  plug [amps]    plug in a car (it can draw at most [amps], default 32)\n"
            "  unplug         pull the cable out\n"
            "  tap [tag]      tap an RFID card (default TAG1); tap it again to stop\n"
            "  pause          the car stops asking for energy\n"
            "  resume         the car asks for energy again\n"
            "  fault          inject a pilot fault\n"
            "  clear          remove the fault and try to clear it\n"
            "  heat <C>       set the ambient temperature (try 90 to cause an over-temperature)\n"
            "  speed <x>      simulated seconds per real second (default 60)\n"
            "  status         show what the charger is doing\n"
            "  quit           stop the simulator\n"
            "tags starting with BAD are refused; tags starting with LATE are refused at start\n";
    }

}  // namespace chargelink::sim