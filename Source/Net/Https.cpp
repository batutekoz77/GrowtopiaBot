#include "Https.hpp"
#include "Tls.hpp"
#include "../Core/Text.hpp"

#include <format>
#include <memory>

namespace Net {
    namespace {
        struct Url {
            bool https = true;
            std::string host;
            uint16_t port = 443;
            std::string target = "/";
        };

        std::optional<Url> ParseUrl(std::string_view text) {
            Url url;
            if (text.starts_with("https://")) text.remove_prefix(8);
            else if (text.starts_with("http://")) {
                text.remove_prefix(7);
                url.https = false;
                url.port = 80;
            }
            else return std::nullopt;

            const size_t slash = text.find_first_of("/?");
            std::string_view authority = text.substr(0, slash);
            if (slash != std::string_view::npos) {
                url.target = std::string(text.substr(slash));
                if (url.target.front() == '?') url.target = "/" + url.target;
            }
            if (const size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
                const auto port = Text::ToInt(authority.substr(colon + 1));
                if (!port || *port <= 0 || *port > 65535) return std::nullopt;
                url.port = static_cast<uint16_t>(*port);
                authority = authority.substr(0, colon);
            }
            url.host = Text::Lower(authority);
            if (url.host.empty()) return std::nullopt;
            return url;
        }

        std::string Resolve(const Url& base, const std::string& location) {
            if (location.starts_with("http://") || location.starts_with("https://")) return location;
            const std::string origin = std::format("{}://{}{}", base.https ? "https" : "http", base.host,
                (base.https && base.port == 443) || (!base.https && base.port == 80) ? "" : ":" + std::to_string(base.port));
            if (location.starts_with('/')) return origin + location;
            const size_t lastSlash = base.target.rfind('/', base.target.find('?'));
            return origin + base.target.substr(0, lastSlash + 1) + location;
        }

        bool DomainMatches(const std::string& host, const std::string& domain) {
            return host == domain || (host.size() > domain.size() && host.ends_with(domain) && host[host.size() - domain.size() - 1] == '.');
        }

        class Stream {
        public:
            Stream(TcpSocket& socket, TlsStream* tls) : socket_(socket), tls_(tls) {}
            int Read(char* buffer, size_t size) { return tls_ ? tls_->Read(buffer, size) : socket_.Receive(buffer, size); }
            bool Write(const std::string& data) { return tls_ ? tls_->Write(data.data(), data.size()) : socket_.SendAll(data.data(), data.size()); }
        private:
            TcpSocket& socket_;
            TlsStream* tls_;
        };

        bool DecodeChunked(std::string_view raw, std::string& out) {
            size_t pos = 0;
            for (;;) {
                const size_t lineEnd = raw.find("\r\n", pos);
                if (lineEnd == std::string_view::npos) return false;
                std::string_view sizeText = raw.substr(pos, lineEnd - pos);
                if (const size_t semicolon = sizeText.find(';'); semicolon != std::string_view::npos) sizeText = sizeText.substr(0, semicolon);
                size_t size = 0;
                try { size = std::stoul(std::string(Text::Trim(sizeText)), nullptr, 16); }
                catch (...) { return false; }
                pos = lineEnd + 2;
                if (size == 0) return true;
                if (pos + size > raw.size()) return false;
                out.append(raw.substr(pos, size));
                pos += size + 2;
            }
        }
    }

    std::optional<HttpResponse> HttpClient::Get(const std::string& url, const Headers& headers, std::string& error, bool verify) {
        return Send("GET", url, headers, {}, error, verify);
    }

    std::optional<HttpResponse> HttpClient::Post(const std::string& url, const Headers& headers, const std::string& body,
        std::string& error, bool verify) {
        return Send("POST", url, headers, body, error, verify);
    }

    std::optional<HttpResponse> HttpClient::Send(std::string method, std::string url, const Headers& headers, std::string body,
        std::string& error, bool verify) {
        for (int redirects = 0; redirects < 8; ++redirects) {
            auto response = SendOnce(method, url, headers, body, error, verify);
            if (!response) return std::nullopt;
            response->finalUrl = url;

            const int s = response->status;
            const auto location = response->headers.find("location");
            if ((s == 301 || s == 302 || s == 303 || s == 307 || s == 308) && location != response->headers.end()) {
                const auto base = ParseUrl(url);
                url = Resolve(*base, location->second);
                if (s != 307 && s != 308) {
                    method = "GET";
                    body.clear();
                }
                continue;
            }
            return response;
        }
        error = "too many redirects";
        return std::nullopt;
    }

    std::optional<HttpResponse> HttpClient::SendOnce(const std::string& method, const std::string& urlText, const Headers& headers,
        const std::string& body, std::string& error, bool verify) {
        const auto url = ParseUrl(urlText);
        if (!url) {
            error = "bad URL: " + urlText;
            return std::nullopt;
        }

        TcpSocket socket;
        if (!socket.Connect(url->host, url->port, proxy_ ? &*proxy_ : nullptr, error, timeoutMs_)) return std::nullopt;

        std::unique_ptr<TlsStream> tls;
        if (url->https) {
            tls = std::make_unique<TlsStream>(socket);
            if (!tls->Handshake(url->host, verify, error)) return std::nullopt;
        }
        Stream stream(socket, tls.get());

        std::string request = std::format("{} {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\nAccept: */*\r\n", method, url->target, url->host);
        bool hasContentType = false;
        for (const auto& [name, value] : headers) {
            if (Text::Lower(name) == "content-type") hasContentType = true;
            request += name + ": " + value + "\r\n";
        }
        if (const std::string cookies = CookieHeader(url->host); !cookies.empty()) request += "Cookie: " + cookies + "\r\n";
        if (method == "POST") {
            if (!hasContentType) request += "Content-Type: application/x-www-form-urlencoded\r\n";
            request += std::format("Content-Length: {}\r\n", body.size());
        }
        request += "\r\n";
        request += body;

        if (!stream.Write(request)) {
            error = "couldn't send the request to " + url->host;
            return std::nullopt;
        }

        std::string raw;
        char buffer[16384];
        const auto complete = [&raw] {
            const size_t end = raw.find("\r\n\r\n");
            if (end == std::string::npos) return false;
            const std::string head = Text::Lower(std::string_view(raw.data(), end));
            if (const size_t at = head.find("\ncontent-length:"); at != std::string::npos) {
                const size_t lineEnd = head.find('\r', at + 1);
                const auto length = Text::ToInt(std::string_view(head).substr(at + 16, lineEnd - at - 16));
                return length && raw.size() >= end + 4 + static_cast<size_t>(*length);
            }
            if (head.find("\ntransfer-encoding: chunked") != std::string::npos) return raw.ends_with("0\r\n\r\n");
            return false;
        };
        for (;;) {
            const int received = stream.Read(buffer, sizeof(buffer));
            if (received <= 0) break;
            raw.append(buffer, static_cast<size_t>(received));
            if (complete()) break;
        }

        const size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            error = "no HTTP response from " + url->host;
            return std::nullopt;
        }

        HttpResponse response;
        const std::string_view head(raw.data(), headerEnd);
        const auto lines = Text::Split(head, '\n');
        {
            const auto statusParts = Text::Split(Text::Trim(lines[0]), ' ');
            response.status = statusParts.size() > 1 ? static_cast<int>(Text::ToInt(statusParts[1]).value_or(0)) : 0;
        }
        for (size_t i = 1; i < lines.size(); ++i) {
            const std::string_view line = Text::Trim(lines[i]);
            const size_t colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            const std::string name = Text::Lower(Text::Trim(line.substr(0, colon)));
            const std::string value(Text::Trim(line.substr(colon + 1)));
            if (name == "set-cookie") StoreCookie(url->host, value);
            else response.headers[name] = value;
        }

        const std::string_view payload(raw.data() + headerEnd + 4, raw.size() - headerEnd - 4);
        const auto encoding = response.headers.find("transfer-encoding");
        if (encoding != response.headers.end() && Text::IContains(encoding->second, "chunked")) {
            if (!DecodeChunked(payload, response.body)) response.body.assign(payload);
        }
        else if (const auto length = response.headers.find("content-length"); length != response.headers.end()) {
            const size_t size = static_cast<size_t>(Text::ToInt(length->second).value_or(static_cast<long long>(payload.size())));
            response.body.assign(payload.substr(0, std::min(size, payload.size())));
        }
        else response.body.assign(payload);

        return response;
    }

    std::string HttpClient::CookieHeader(const std::string& host) const {
        std::string header;
        for (const auto& [key, cookie] : cookies_) {
            if (!DomainMatches(host, cookie.domain)) continue;
            if (!header.empty()) header += "; ";
            header += key.substr(key.find('\n') + 1) + "=" + cookie.value;
        }
        return header;
    }

    void HttpClient::StoreCookie(const std::string& host, const std::string& setCookie) {
        const auto parts = Text::Split(setCookie, ';');
        const std::string_view pair = Text::Trim(parts[0]);
        const size_t equals = pair.find('=');
        if (equals == std::string_view::npos) return;
        const std::string name(Text::Trim(pair.substr(0, equals)));
        const std::string value(Text::Trim(pair.substr(equals + 1)));

        std::string domain = host;
        bool expired = false;
        for (size_t i = 1; i < parts.size(); ++i) {
            const std::string attribute = Text::Lower(Text::Trim(parts[i]));
            if (attribute.starts_with("domain=")) {
                domain = attribute.substr(7);
                while (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
            }
            else if (attribute.starts_with("max-age=") && Text::ToInt(attribute.substr(8)).value_or(1) <= 0) expired = true;
        }

        const std::string key = domain + "\n" + name;
        if (expired || value.empty() || value == "deleted") cookies_.erase(key);
        else cookies_[key] = Cookie{ domain, value };
    }
}
