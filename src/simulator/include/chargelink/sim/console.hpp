#pragma once

#include <string>

namespace chargelink::sim {

    enum class CommandType {
        Empty,    // a blank line
        Help,
        Plug,
        Unplug,
        Tap,
        Pause,
        Resume,
        Fault,
        Clear,
        Heat,
        Speed,
        Status,
        Quit,
        Invalid,  // something we could not understand: `text` explains why
    };

    struct Command {
        CommandType type = CommandType::Empty;
        std::string text;        // the tag for Tap, the error message for Invalid
        double number = 0.0;     // amps, degrees or factor
        bool has_number = false;
    };

    // Turns one line typed by the user into a command. Pure function: no I/O, easy to test.
    Command parse_command(const std::string& line);

    const char* help_text();

}  // namespace chargelink::sim