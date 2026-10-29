#pragma once

#include "Socket.hpp"

#include <string>

namespace Net {
    class Socks5UdpRelay {
    public:
        Socks5UdpRelay() = default;
        ~Socks5UdpRelay();
        Socks5UdpRelay(const Socks5UdpRelay&) = delete;
        Socks5UdpRelay& operator=(const Socks5UdpRelay&) = delete;

        bool Open(const Socks5Config& proxy, std::string& error);
        void Close();
        bool IsOpen() const noexcept { return control_.IsOpen(); }

        void SetTarget(const sockaddr_in& target);
        sockaddr_in LocalAddress() const noexcept { return localAddress_; }

        bool Pump();

        SOCKET LocalSocket() const noexcept { return local_; }
        SOCKET ProxySocket() const noexcept { return proxy_; }

    private:
        TcpSocket control_;
        SOCKET local_ = INVALID_SOCKET;
        SOCKET proxy_ = INVALID_SOCKET;
        sockaddr_in localAddress_{};
        sockaddr_in relayAddress_{};
        sockaddr_in target_{};
        sockaddr_in enetAddress_{};
        bool hasEnet_ = false;
        bool hasTarget_ = false;
    };
}
