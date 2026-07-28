#pragma once

#include <string>
#include <functional>

#include <stdint.h>

#include "external/rapidjson/document.h"

namespace api {

    struct ClientState;

    class Request {
    public:
        uint64_t id;
        std::string module;
        std::string function;
        rapidjson::Value params;
        bool parse_error;
        ClientState *client_state = nullptr;

        Request(rapidjson::Document &document);
    };
}
