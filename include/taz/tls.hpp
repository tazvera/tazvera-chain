#pragma once
#include "taz/net.hpp"
#include "taz/types.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace taz {

// v9.3: TLS 1.3 for the wire. Foundation only — nothing calls it yet, so the chain behaves exactly as before.
class TlsContext {
public:
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    static std::unique_ptr<TlsContext> make_server(std::string* error);
    static std::unique_ptr<TlsContext> make_client(std::string* error);

    struct Conn;
    std::unique_ptr<Conn> accept(const Socket& socket, std::string* error, Deadline deadline = no_deadline());
    std::unique_ptr<Conn> connect(const Socket& socket, std::string* error, Deadline deadline = no_deadline());

    const Hash32& certificate_fingerprint() const { return fingerprint_; }

private:
    struct Impl;
    TlsContext();
    std::unique_ptr<Impl> impl_;
    Hash32 fingerprint_{};
};

struct TlsContext::Conn {
    ~Conn();
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    bool send_all(std::string_view bytes, std::string* error, Deadline deadline = no_deadline());
    bool recv_exact(std::size_t count, std::string& out, std::string* error, Deadline deadline = no_deadline());

    Hash32 peer_certificate_fingerprint() const;

private:
    friend class TlsContext;
    Conn();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace taz
