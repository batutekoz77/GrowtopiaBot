#pragma once

#include "../Net/Https.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace Login {
    struct ClientInfo {
        std::string gameVersion;
        int protocol = 225;
        int32_t fhash = -716928004;
    };

    struct ServerData {
        std::string server;
        uint16_t port = 0;
        std::string loginUrl;
        std::string meta;
        std::string maintenance;
    };

    enum class Error {
        None,
        Network,
        WrongCredentials,
        Exhausted,
        Other,
    };

    struct Result {
        Error error = Error::None;
        std::string message;
        explicit operator bool() const noexcept { return error == Error::None; }
    };

    std::optional<ServerData> FetchServerData(Net::HttpClient& http, const ClientInfo& client, bool alternate, std::string& error);

    Result LegacyToken(Net::HttpClient& http, const ClientInfo& client, const ServerData& server,
        const std::string& growId, const std::string& password, std::string& token);

    Result CheckToken(Net::HttpClient& http, const std::string& token, const std::string& clientData, std::string& newToken);
}
