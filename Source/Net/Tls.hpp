#pragma once

#include "Socket.hpp"

#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>

#include <string>
#include <vector>

namespace Net {
    class TlsStream {
    public:
        explicit TlsStream(TcpSocket& socket) : socket_(socket) {}
        ~TlsStream();
        TlsStream(const TlsStream&) = delete;
        TlsStream& operator=(const TlsStream&) = delete;

        bool Handshake(const std::string& host, bool verify, std::string& error);
        bool Write(const void* data, size_t size);
        int Read(char* buffer, size_t size);

    private:
        TcpSocket& socket_;
        CredHandle credentials_{};
        CtxtHandle context_{};
        bool hasCredentials_ = false;
        bool hasContext_ = false;
        bool closed_ = false;
        SecPkgContext_StreamSizes sizes_{};
        std::vector<char> encrypted_;
        std::vector<char> plain_;
        size_t plainOffset_ = 0;

        bool ReceiveMore();
    };
}
