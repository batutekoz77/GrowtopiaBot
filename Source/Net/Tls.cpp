#include "Tls.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace Net {
    namespace {
        constexpr size_t MAX_RECORD = 16 * 1024 + 512;

        void FreeOutput(SecBuffer& buffer) {
            if (buffer.pvBuffer) FreeContextBuffer(buffer.pvBuffer);
            buffer.pvBuffer = nullptr;
            buffer.cbBuffer = 0;
        }
    }

    TlsStream::~TlsStream() {
        if (hasContext_) DeleteSecurityContext(&context_);
        if (hasCredentials_) FreeCredentialsHandle(&credentials_);
    }

    bool TlsStream::ReceiveMore() {
        char chunk[MAX_RECORD];
        const int received = socket_.Receive(chunk, sizeof(chunk));
        if (received <= 0) return false;
        encrypted_.insert(encrypted_.end(), chunk, chunk + received);
        return true;
    }

    bool TlsStream::Handshake(const std::string& host, bool verify, std::string& error) {
        SCHANNEL_CRED cred{};
        cred.dwVersion = SCHANNEL_CRED_VERSION;
        cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
        cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO |
            (verify ? SCH_CRED_AUTO_CRED_VALIDATION : SCH_CRED_MANUAL_CRED_VALIDATION);

        TimeStamp expiry{};
        SECURITY_STATUS status = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
            nullptr, &cred, nullptr, nullptr, &credentials_, &expiry);
        if (status != SEC_E_OK) {
            error = std::format("TLS: AcquireCredentialsHandle failed (0x{:08X})", static_cast<uint32_t>(status));
            return false;
        }
        hasCredentials_ = true;

        DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
            ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR;
        if (!verify) flags |= ISC_REQ_MANUAL_CRED_VALIDATION;
        DWORD outFlags = 0;
        std::wstring target(host.begin(), host.end());

        SecBuffer outBuffer{ 0, SECBUFFER_TOKEN, nullptr };
        SecBufferDesc outDesc{ SECBUFFER_VERSION, 1, &outBuffer };
        status = InitializeSecurityContextW(&credentials_, nullptr, target.data(), flags, 0, 0, nullptr, 0,
            &context_, &outDesc, &outFlags, &expiry);
        if (status != SEC_I_CONTINUE_NEEDED) {
            error = std::format("TLS: InitializeSecurityContext failed (0x{:08X})", static_cast<uint32_t>(status));
            return false;
        }
        hasContext_ = true;
        const bool sentHello = socket_.SendAll(outBuffer.pvBuffer, outBuffer.cbBuffer);
        FreeOutput(outBuffer);
        if (!sentHello) {
            error = "TLS: couldn't send ClientHello";
            return false;
        }

        bool needData = true;
        for (;;) {
            if (needData && !ReceiveMore()) {
                error = "TLS: connection closed during handshake";
                return false;
            }

            SecBuffer inBuffers[2] = {
                { static_cast<unsigned long>(encrypted_.size()), SECBUFFER_TOKEN, encrypted_.data() },
                { 0, SECBUFFER_EMPTY, nullptr },
            };
            SecBufferDesc inDesc{ SECBUFFER_VERSION, 2, inBuffers };
            outBuffer = { 0, SECBUFFER_TOKEN, nullptr };
            outDesc = { SECBUFFER_VERSION, 1, &outBuffer };

            status = InitializeSecurityContextW(&credentials_, &context_, target.data(), flags, 0, 0, &inDesc, 0,
                nullptr, &outDesc, &outFlags, &expiry);

            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                needData = true;
                continue;
            }

            if (outBuffer.cbBuffer && outBuffer.pvBuffer) {
                const bool sent = socket_.SendAll(outBuffer.pvBuffer, outBuffer.cbBuffer);
                FreeOutput(outBuffer);
                if (!sent) {
                    error = "TLS: send failed during handshake";
                    return false;
                }
            }

            if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED && status != SEC_I_INCOMPLETE_CREDENTIALS) {
                error = std::format("TLS handshake with {} failed (0x{:08X})", host, static_cast<uint32_t>(status));
                return false;
            }

            if (inBuffers[1].BufferType == SECBUFFER_EXTRA && inBuffers[1].cbBuffer > 0) {
                encrypted_.erase(encrypted_.begin(), encrypted_.end() - inBuffers[1].cbBuffer);
                needData = false;
            }
            else {
                encrypted_.clear();
                needData = true;
            }

            if (status == SEC_E_OK) break;
            if (status == SEC_I_INCOMPLETE_CREDENTIALS) needData = false;
        }

        status = QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_);
        if (status != SEC_E_OK) {
            error = "TLS: QueryContextAttributes failed";
            return false;
        }
        return true;
    }

    bool TlsStream::Write(const void* data, size_t size) {
        const char* bytes = static_cast<const char*>(data);
        std::vector<char> message(sizes_.cbHeader + sizes_.cbMaximumMessage + sizes_.cbTrailer);

        while (size > 0) {
            const size_t chunk = std::min<size_t>(size, sizes_.cbMaximumMessage);
            std::memcpy(message.data() + sizes_.cbHeader, bytes, chunk);

            SecBuffer buffers[4] = {
                { sizes_.cbHeader, SECBUFFER_STREAM_HEADER, message.data() },
                { static_cast<unsigned long>(chunk), SECBUFFER_DATA, message.data() + sizes_.cbHeader },
                { sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, message.data() + sizes_.cbHeader + chunk },
                { 0, SECBUFFER_EMPTY, nullptr },
            };
            SecBufferDesc desc{ SECBUFFER_VERSION, 4, buffers };
            if (EncryptMessage(&context_, 0, &desc, 0) != SEC_E_OK) return false;

            const size_t total = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
            if (!socket_.SendAll(message.data(), total)) return false;
            bytes += chunk;
            size -= chunk;
        }
        return true;
    }

    int TlsStream::Read(char* buffer, size_t size) {
        while (plainOffset_ >= plain_.size()) {
            plain_.clear();
            plainOffset_ = 0;
            if (closed_) return 0;

            if (encrypted_.empty() && !ReceiveMore()) return 0;

            SecBuffer buffers[4] = {
                { static_cast<unsigned long>(encrypted_.size()), SECBUFFER_DATA, encrypted_.data() },
                { 0, SECBUFFER_EMPTY, nullptr },
                { 0, SECBUFFER_EMPTY, nullptr },
                { 0, SECBUFFER_EMPTY, nullptr },
            };
            SecBufferDesc desc{ SECBUFFER_VERSION, 4, buffers };
            const SECURITY_STATUS status = DecryptMessage(&context_, &desc, 0, nullptr);

            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                if (!ReceiveMore()) return 0;
                continue;
            }
            if (status == SEC_I_CONTEXT_EXPIRED) {
                closed_ = true;
                return 0;
            }
            if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) return -1;

            std::vector<char> extra;
            for (const SecBuffer& b : buffers) {
                if (b.BufferType == SECBUFFER_DATA && b.cbBuffer) {
                    const char* p = static_cast<const char*>(b.pvBuffer);
                    plain_.insert(plain_.end(), p, p + b.cbBuffer);
                }
                else if (b.BufferType == SECBUFFER_EXTRA && b.cbBuffer) {
                    const char* p = static_cast<const char*>(b.pvBuffer);
                    extra.assign(p, p + b.cbBuffer);
                }
            }
            encrypted_ = std::move(extra);
            if (status == SEC_I_RENEGOTIATE) return -1;
        }

        const size_t count = std::min(size, plain_.size() - plainOffset_);
        std::memcpy(buffer, plain_.data() + plainOffset_, count);
        plainOffset_ += count;
        return static_cast<int>(count);
    }
}
