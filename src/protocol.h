#pragma once

#include <string>

enum class CommandType {
    Ping,
    Get,
    Set,
    Delete,
    Stats,
    Quit,
    Invalid,
};

struct Command {
    CommandType type{CommandType::Invalid};
    std::string key;
    std::string value;
    std::string error;
};

[[nodiscard]] Command parse_command(std::string line);
