#pragma once

#include "taz/net.hpp"
#include "taz/tls.hpp"

#include <memory>

namespace taz {

// Adapts an already-established TLS connection to the message-layer Transport interface.
// The caller owns the Socket and must keep it alive for this transport's lifetime.
class TlsTransport final : public Transport {
public:
    TlsTransport(const Socket& socket, std::unique_ptr<TlsContext> context,
                 std::unique_ptr<TlsContext::Conn> connection) noexcept;
    ~TlsTransport() override = default;

    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;

    bool send_all(std::string_view bytes, std::string* error, Deadline deadline) override;
    bool recv_exact(std::size_t count, std::string& out, std::string* error, Deadline deadline) override;
    void shutdown() noexcept override;

private:
    const Socket& socket_;
    std::unique_ptr<TlsContext> context_;
    std::unique_ptr<TlsContext::Conn> connection_;
};

}  // namespace taz
