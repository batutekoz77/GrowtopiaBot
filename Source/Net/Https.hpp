#pragma once

#include "Socket.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Net {
    struct HttpResponse {
        int status = 0;
        std::map<std::string, std::string> headers;
        std::string body;
        std::string finalUrl;
    };

    using Headers = std::vector<std::pair<std::string, std::string>>;

    class HttpClient {
    public:
        explicit HttpClient(std::optional<Socks5Config> proxy = std::nullopt, int timeoutMs = 20000)
            : proxy_(std::move(proxy)), timeoutMs_(timeoutMs) {}

        std::optional<HttpResponse> Get(const std::string& url, const Headers& headers, std::string& error, bool verify = true);
        std::optional<HttpResponse> Post(const std::string& url, const Headers& headers, const std::string& body,
            std::string& error, bool verify = true);

        void ClearCookies() { cookies_.clear(); }

    private:
        struct Cookie {
            std::string domain;
            std::string value;
        };

        std::optional<Socks5Config> proxy_;
        int timeoutMs_;
        std::map<std::string, Cookie> cookies_;

        std::optional<HttpResponse> Send(std::string method, std::string url, const Headers& headers, std::string body,
            std::string& error, bool verify);
        std::optional<HttpResponse> SendOnce(const std::string& method, const std::string& url, const Headers& headers,
            const std::string& body, std::string& error, bool verify);

        std::string CookieHeader(const std::string& host) const;
        void StoreCookie(const std::string& host, const std::string& setCookie);
    };
}
