#include "protocol.h"

#include <sstream>
#include <utility>

namespace {

Command invalid(std::string message) {
    return Command{CommandType::Invalid, {}, {}, std::move(message)};
}

bool has_extra(std::istringstream& input) {
    std::string extra;
    return static_cast<bool>(input >> extra);
}

}  // namespace

Command parse_command(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream input(line);
    std::string verb;
    if (!(input >> verb)) {
        return invalid("empty command");
    }

    if (verb == "PING") {
        return has_extra(input) ? invalid("PING takes no arguments")
                                : Command{CommandType::Ping, {}, {}, {}};
    }
    if (verb == "STATS") {
        return has_extra(input) ? invalid("STATS takes no arguments")
                                : Command{CommandType::Stats, {}, {}, {}};
    }
    if (verb == "QUIT") {
        return has_extra(input) ? invalid("QUIT takes no arguments")
                                : Command{CommandType::Quit, {}, {}, {}};
    }

    std::string key;
    if (!(input >> key)) {
        return invalid(verb + " requires a key");
    }

    if (verb == "GET") {
        return has_extra(input) ? invalid("GET takes one key")
                                : Command{CommandType::Get, std::move(key), {}, {}};
    }
    if (verb == "DEL") {
        return has_extra(input) ? invalid("DEL takes one key")
                                : Command{CommandType::Delete, std::move(key), {}, {}};
    }
    if (verb == "SET") {
        std::string value;
        std::getline(input >> std::ws, value);
        if (value.empty()) {
            return invalid("SET requires a key and value");
        }
        return Command{CommandType::Set, std::move(key), std::move(value), {}};
    }

    return invalid("unknown command");
}
