#include "taz/tls_transport.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace taz {

TlsTransport::TlsTransport(const Socket& socket, std::unique_ptr<TlsContext::Conn> connection) noexcept
    : socket_(socket), connection_(std::move(connection)) {}

bool TlsTransport::send_all(std::string_view bytes, std::string* error, Deadline deadline) {
    if (!connection_) {
        if (error) *error = "TLS connection is not established";
        return false;
    }
    return connection_->send_all(bytes, error, deadline);
}

bool TlsTransport::recv_exact(std::size_t count, std::string& out, std::string* error, Deadline deadline) {
    if (!connection_) {
        out.clear();
        if (error) *error = "TLS connection is not established";
        return false;
    }
    return connection_->recv_exact(count, out, error, deadline);
}

void TlsTransport::shutdown() noexcept {
    if (socket_.valid()) {
#ifdef _WIN32
        ::shutdown(static_cast<SOCKET>(socket_.raw()), SD_BOTH);
#else
        ::shutdown(socket_.raw(), SHUT_RDWR);
#endif
    }
    connection_.reset();
}

}  // namespace taz
