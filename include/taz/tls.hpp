#pragma once
#include "taz/net.hpp"
#include "taz/types.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace taz {

// v9.3: TLS 1.3 for the wire. This is the foundation only — nothing calls it yet.
//
// What it gives: nobody between two nodes can read what they say.
// What it does NOT give: proof of who is on the other end. That is the existing hello/auth handshake, which
// signs a fresh nonce. A certificate here is self-signed and is not trusted; verify the fingerprint separately
// (next change) or the connection is encrypted but unauthenticated, and that difference must be said out loud.
class TlsContext {
public:
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    // Server side: makes its own self-signed certificate and key in memory at startup.
    static std::unique_ptr<TlsContext> make_server(std::string* error);
    // Client side: no certificate of its own; does not verify the peer's certificate (see the note above).
    static std::unique_ptr<TlsContext> make_client(std::string* error);

    struct Conn;
    // Takes over an already connected socket. Returns null on failure, with the reason in error.
    std::unique_ptr<Conn> accept(const Socket& socket, std::string* error);
    std::unique_ptr<Conn> connect(const Socket& socket, std::string* error);

    // SHA-256 of this context's own certificate, DER encoded. Empty for a client context.
    const Hash32& certificate_fingerprint() const { return fingerprint_; }

private:
    struct Impl;
    TlsContext();
    std::unique_ptr<Impl> impl_;
    Hash32 fingerprint_{};
};

// One encrypted connection.
struct TlsContext::Conn {
    ~Conn();
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    bool send_all(std::string_view bytes, std::string* error);
    bool recv_exact(std::size_t count, std::string& out, std::string* error);

private:
    friend class TlsContext;
    Conn();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace taz
