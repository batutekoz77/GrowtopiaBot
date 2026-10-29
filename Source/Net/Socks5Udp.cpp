#include "Socks5Udp.hpp"

#include <mstcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <cstring>
#include <format>

namespace Net {
    namespace {
        constexpr size_t HEADER_V4 = 10;
        constexpr size_t MAX_DATAGRAM = 65536;

        SOCKET CreateUdp(const sockaddr_in& bindAddress) {
            SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET) return s;
            if (bind(s, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof(bindAddress)) == SOCKET_ERROR) {
                closesocket(s);
                return INVALID_SOCKET;
            }
            u_long nonBlocking = 1;
            ioctlsocket(s, FIONBIO, &nonBlocking);
            BOOL reportReset = FALSE;
            DWORD returned = 0;
            WSAIoctl(s, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &returned, nullptr, nullptr);
            const int buffer = 1 << 20;
            setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
            setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
            return s;
        }

        bool SameEndpoint(const sockaddr_in& a, const sockaddr_in& b) {
            return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
        }
    }

    Socks5UdpRelay::~Socks5UdpRelay() { Close(); }

    void Socks5UdpRelay::Close() {
        if (local_ != INVALID_SOCKET) closesocket(local_);
        if (proxy_ != INVALID_SOCKET) closesocket(proxy_);
        local_ = proxy_ = INVALID_SOCKET;
        control_.Close();
        hasEnet_ = hasTarget_ = false;
    }

    bool Socks5UdpRelay::Open(const Socks5Config& proxy, std::string& error) {
        Startup();
        Close();

        const auto proxyAddress = Resolve(proxy.host, proxy.port);
        if (!proxyAddress) {
            error = "SOCKS5: couldn't resolve " + proxy.host;
            return false;
        }

        if (!control_.Connect(proxy.host, proxy.port, nullptr, error, 10000) || !Socks5Greet(control_, proxy, error)) {
            if (error.find("SOCKS5") == std::string::npos) error = "SOCKS5 " + proxy.ToString() + ": " + error;
            Close();
            return false;
        }

        const uint8_t request[] = { 0x05, 0x03, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
        uint8_t reply[4]{};
        if (!control_.SendAll(request, sizeof(request)) || !control_.ReceiveExact(reply, sizeof(reply))) {
            error = "SOCKS5 UDP ASSOCIATE: no reply from " + proxy.ToString();
            Close();
            return false;
        }
        if (reply[0] != 0x05 || reply[1] != 0x00) {
            error = std::format("SOCKS5 UDP ASSOCIATE refused by {}: {}", proxy.ToString(), Socks5ReplyText(reply[1]));
            Close();
            return false;
        }
        if (reply[3] != 0x01) {
            error = "SOCKS5 UDP ASSOCIATE: proxy returned a non-IPv4 relay address (unsupported)";
            Close();
            return false;
        }
        uint8_t bound[6]{};
        if (!control_.ReceiveExact(bound, sizeof(bound))) {
            error = "SOCKS5 UDP ASSOCIATE: truncated reply";
            Close();
            return false;
        }
        relayAddress_ = {};
        relayAddress_.sin_family = AF_INET;
        std::memcpy(&relayAddress_.sin_addr, bound, 4);
        std::memcpy(&relayAddress_.sin_port, bound + 4, 2);
        if (relayAddress_.sin_addr.s_addr == 0) relayAddress_.sin_addr = proxyAddress->sin_addr;

        sockaddr_in any{};
        any.sin_family = AF_INET;
        proxy_ = CreateUdp(any);

        sockaddr_in loopback{};
        loopback.sin_family = AF_INET;
        loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local_ = CreateUdp(loopback);

        if (proxy_ == INVALID_SOCKET || local_ == INVALID_SOCKET) {
            error = std::format("SOCKS5: couldn't create the UDP sockets ({})", WSAGetLastError());
            Close();
            return false;
        }
        int length = sizeof(localAddress_);
        getsockname(local_, reinterpret_cast<sockaddr*>(&localAddress_), &length);

        u_long nonBlocking = 1;
        ioctlsocket(control_.Handle(), FIONBIO, &nonBlocking);
        return true;
    }

    void Socks5UdpRelay::SetTarget(const sockaddr_in& target) {
        target_ = target;
        hasTarget_ = true;
        hasEnet_ = false;
    }

    bool Socks5UdpRelay::Pump() {
        if (!IsOpen()) return false;

        char probe;
        const int controlState = recv(control_.Handle(), &probe, 1, MSG_PEEK);
        if (controlState == 0 || (controlState == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) return false;

        static thread_local char buffer[MAX_DATAGRAM];

        for (;;) {
            sockaddr_in from{};
            int fromLength = sizeof(from);
            const int received = recvfrom(local_, buffer + HEADER_V4, static_cast<int>(MAX_DATAGRAM - HEADER_V4), 0,
                reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (received <= 0) break;
            if (!hasTarget_) continue;
            if (!hasEnet_) {
                enetAddress_ = from;
                hasEnet_ = true;
            }
            else if (!SameEndpoint(from, enetAddress_)) continue;

            buffer[0] = buffer[1] = buffer[2] = 0;
            buffer[3] = 0x01;
            std::memcpy(buffer + 4, &target_.sin_addr, 4);
            std::memcpy(buffer + 8, &target_.sin_port, 2);
            sendto(proxy_, buffer, received + static_cast<int>(HEADER_V4), 0, reinterpret_cast<const sockaddr*>(&relayAddress_), sizeof(relayAddress_));
        }

        for (;;) {
            sockaddr_in from{};
            int fromLength = sizeof(from);
            const int received = recvfrom(proxy_, buffer, static_cast<int>(MAX_DATAGRAM), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (received <= 0) break;
            if (!hasEnet_ || received < static_cast<int>(HEADER_V4)) continue;
            if (buffer[0] != 0 || buffer[1] != 0 || buffer[2] != 0 || buffer[3] != 0x01) continue;
            if (std::memcmp(buffer + 4, &target_.sin_addr, 4) != 0 || std::memcmp(buffer + 8, &target_.sin_port, 2) != 0) continue;

            sendto(local_, buffer + HEADER_V4, received - static_cast<int>(HEADER_V4), 0,
                reinterpret_cast<const sockaddr*>(&enetAddress_), sizeof(enetAddress_));
        }
        return true;
    }
}
