#include "Socket.hpp"
#include "../Core/Text.hpp"

#include <algorithm>
#include <format>
#include <mutex>
#include <vector>

namespace Net {
    void Startup() {
        static std::once_flag once;
        std::call_once(once, [] {
            WSADATA data{};
            WSAStartup(MAKEWORD(2, 2), &data);
        });
    }

    std::string Socks5Config::ToString() const {
        return std::format("{}:{}{}", host, port, HasAuth() ? " (auth)" : "");
    }

    std::optional<Socks5Config> Socks5Config::Parse(std::string_view text) {
        text = Text::Trim(text);
        if (text.empty()) return std::nullopt;

        Socks5Config config;
        std::string_view rest = text;
        const bool url = rest.starts_with("socks5://") || rest.starts_with("socks5h://");
        if (url) rest.remove_prefix(rest.find("://") + 3);
        while (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);

        const auto setPort = [&](std::string_view port) {
            const auto value = Text::ToInt(port);
            if (!value || *value <= 0 || *value > 65535) return false;
            config.port = static_cast<uint16_t>(*value);
            return true;
        };

        if (const size_t at = rest.rfind('@'); at != std::string_view::npos) {
            const std::string_view credentials = rest.substr(0, at);
            const std::string_view endpoint = rest.substr(at + 1);
            const size_t colon = credentials.find(':');
            config.username = std::string(credentials.substr(0, colon));
            if (colon != std::string_view::npos) config.password = std::string(credentials.substr(colon + 1));
            const size_t portColon = endpoint.rfind(':');
            if (portColon == std::string_view::npos || !setPort(endpoint.substr(portColon + 1))) return std::nullopt;
            config.host = std::string(endpoint.substr(0, portColon));
        }
        else {
            const auto parts = Text::Split(rest, ':');
            if (parts.size() != 2 && parts.size() != 4) return std::nullopt;
            config.host = std::string(parts[0]);
            if (!setPort(parts[1])) return std::nullopt;
            if (parts.size() == 4) {
                config.username = std::string(parts[2]);
                config.password = std::string(parts[3]);
            }
        }
        if (config.host.empty() || config.username.size() > 255 || config.password.size() > 255) return std::nullopt;
        return config;
    }

    std::optional<sockaddr_in> Resolve(const std::string& host, uint16_t port) {
        Startup();
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) return std::nullopt;
        sockaddr_in address = *reinterpret_cast<const sockaddr_in*>(result->ai_addr);
        freeaddrinfo(result);
        address.sin_port = htons(port);
        return address;
    }

    std::string ToString(const sockaddr_in& address) {
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));
        return std::format("{}:{}", ip, ntohs(address.sin_port));
    }

    const char* Socks5ReplyText(uint8_t code) noexcept {
        switch (code) {
            case 0x01: return "general failure";
            case 0x02: return "connection not allowed by ruleset";
            case 0x03: return "network unreachable";
            case 0x04: return "host unreachable";
            case 0x05: return "connection refused";
            case 0x06: return "TTL expired";
            case 0x07: return "command not supported";
            case 0x08: return "address type not supported";
            default:   return "unknown error";
        }
    }

    TcpSocket::~TcpSocket() { Close(); }

    TcpSocket::TcpSocket(TcpSocket&& other) noexcept : socket_(other.socket_) { other.socket_ = INVALID_SOCKET; }

    TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
        if (this != &other) {
            Close();
            socket_ = other.socket_;
            other.socket_ = INVALID_SOCKET;
        }
        return *this;
    }

    void TcpSocket::Close() {
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    bool TcpSocket::ConnectRaw(const sockaddr_in& address, int timeoutMs, std::string& error) {
        Close();
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) {
            error = std::format("socket() failed ({})", WSAGetLastError());
            return false;
        }

        u_long nonBlocking = 1;
        ioctlsocket(socket_, FIONBIO, &nonBlocking);
        const int result = connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
            error = std::format("connect to {} failed ({})", ToString(address), WSAGetLastError());
            Close();
            return false;
        }

        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(socket_, &writable);
        FD_SET(socket_, &failed);
        timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        if (select(0, nullptr, &writable, &failed, &tv) <= 0 || FD_ISSET(socket_, &failed)) {
            error = std::format("connect to {} timed out / refused", ToString(address));
            Close();
            return false;
        }

        nonBlocking = 0;
        ioctlsocket(socket_, FIONBIO, &nonBlocking);
        const DWORD timeout = static_cast<DWORD>(timeoutMs);
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        const BOOL noDelay = TRUE;
        setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        return true;
    }

    bool TcpSocket::Connect(const std::string& host, uint16_t port, const Socks5Config* proxy, std::string& error, int timeoutMs) {
        Startup();
        const std::string& connectHost = proxy ? proxy->host : host;
        const uint16_t connectPort = proxy ? proxy->port : port;

        const auto address = Resolve(connectHost, connectPort);
        if (!address) {
            error = std::format("couldn't resolve {}", connectHost);
            return false;
        }
        if (!ConnectRaw(*address, timeoutMs, error)) {
            if (proxy) error = "SOCKS5 proxy " + proxy->ToString() + ": " + error;
            return false;
        }
        if (!proxy) return true;

        if (!Socks5Greet(*this, *proxy, error)) {
            Close();
            return false;
        }

        if (host.size() > 255) {
            error = "host name too long for SOCKS5";
            Close();
            return false;
        }
        std::vector<uint8_t> request = { 0x05, 0x01, 0x00, 0x03, static_cast<uint8_t>(host.size()) };
        request.insert(request.end(), host.begin(), host.end());
        request.push_back(static_cast<uint8_t>(port >> 8));
        request.push_back(static_cast<uint8_t>(port & 0xFF));

        uint8_t reply[4]{};
        if (!SendAll(request.data(), request.size()) || !ReceiveExact(reply, sizeof(reply))) {
            error = "SOCKS5 CONNECT: no reply from " + proxy->ToString();
            Close();
            return false;
        }
        if (reply[0] != 0x05 || reply[1] != 0x00) {
            error = std::format("SOCKS5 CONNECT to {}:{} failed: {}", host, port, Socks5ReplyText(reply[1]));
            Close();
            return false;
        }

        size_t skip = 0;
        if (reply[3] == 0x01) skip = 4 + 2;
        else if (reply[3] == 0x04) skip = 16 + 2;
        else if (reply[3] == 0x03) {
            uint8_t length = 0;
            if (!ReceiveExact(&length, 1)) { Close(); return false; }
            skip = length + 2u;
        }
        std::vector<uint8_t> bound(skip);
        if (skip && !ReceiveExact(bound.data(), skip)) {
            error = "SOCKS5 CONNECT: truncated reply";
            Close();
            return false;
        }
        return true;
    }

    bool TcpSocket::SendAll(const void* data, size_t size) {
        const char* bytes = static_cast<const char*>(data);
        while (size > 0) {
            const int sent = send(socket_, bytes, static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
            if (sent <= 0) return false;
            bytes += sent;
            size -= static_cast<size_t>(sent);
        }
        return true;
    }

    int TcpSocket::Receive(void* buffer, size_t size) {
        const int received = recv(socket_, static_cast<char*>(buffer), static_cast<int>(size), 0);
        return received == SOCKET_ERROR ? -1 : received;
    }

    bool TcpSocket::ReceiveExact(void* buffer, size_t size) {
        char* bytes = static_cast<char*>(buffer);
        while (size > 0) {
            const int received = Receive(bytes, size);
            if (received <= 0) return false;
            bytes += received;
            size -= static_cast<size_t>(received);
        }
        return true;
    }

    bool Socks5Greet(TcpSocket& stream, const Socks5Config& proxy, std::string& error) {
        const uint8_t withAuth[] = { 0x05, 0x02, 0x00, 0x02 };
        const uint8_t noAuth[] = { 0x05, 0x01, 0x00 };
        const bool useAuth = proxy.HasAuth();
        uint8_t choice[2]{};
        if (!stream.SendAll(useAuth ? withAuth : noAuth, useAuth ? sizeof(withAuth) : sizeof(noAuth)) || !stream.ReceiveExact(choice, sizeof(choice))) {
            error = "SOCKS5 " + proxy.ToString() + ": no greeting reply";
            return false;
        }
        if (choice[0] != 0x05) {
            error = "SOCKS5 " + proxy.ToString() + ": not a SOCKS5 server";
            return false;
        }
        if (choice[1] == 0x00) return true;
        if (choice[1] != 0x02) {
            error = "SOCKS5 " + proxy.ToString() + ": no acceptable auth method";
            return false;
        }
        if (!proxy.HasAuth()) {
            error = "SOCKS5 " + proxy.ToString() + ": proxy wants a username/password";
            return false;
        }

        std::vector<uint8_t> auth = { 0x01, static_cast<uint8_t>(proxy.username.size()) };
        auth.insert(auth.end(), proxy.username.begin(), proxy.username.end());
        auth.push_back(static_cast<uint8_t>(proxy.password.size()));
        auth.insert(auth.end(), proxy.password.begin(), proxy.password.end());
        uint8_t status[2]{};
        if (!stream.SendAll(auth.data(), auth.size()) || !stream.ReceiveExact(status, sizeof(status)) || status[1] != 0x00) {
            error = "SOCKS5 " + proxy.ToString() + ": wrong username/password";
            return false;
        }
        return true;
    }
}
