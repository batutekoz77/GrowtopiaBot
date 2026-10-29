#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Net {
    void Startup();

    struct Socks5Config {
        std::string host;
        uint16_t port = 0;
        std::string username;
        std::string password;

        bool HasAuth() const noexcept { return !username.empty(); }
        std::string ToString() const;

        static std::optional<Socks5Config> Parse(std::string_view text);
    };

    std::optional<sockaddr_in> Resolve(const std::string& host, uint16_t port);
    std::string ToString(const sockaddr_in& address);

    class TcpSocket {
    public:
        TcpSocket() = default;
        ~TcpSocket();
        TcpSocket(TcpSocket&& other) noexcept;
        TcpSocket& operator=(TcpSocket&& other) noexcept;
        TcpSocket(const TcpSocket&) = delete;
        TcpSocket& operator=(const TcpSocket&) = delete;

        bool Connect(const std::string& host, uint16_t port, const Socks5Config* proxy, std::string& error, int timeoutMs = 15000);

        bool SendAll(const void* data, size_t size);
        int Receive(void* buffer, size_t size);
        bool ReceiveExact(void* buffer, size_t size);

        void Close();
        bool IsOpen() const noexcept { return socket_ != INVALID_SOCKET; }
        SOCKET Handle() const noexcept { return socket_; }

    private:
        SOCKET socket_ = INVALID_SOCKET;

        bool ConnectRaw(const sockaddr_in& address, int timeoutMs, std::string& error);
    };

    bool Socks5Greet(TcpSocket& stream, const Socks5Config& proxy, std::string& error);
    const char* Socks5ReplyText(uint8_t code) noexcept;
}
