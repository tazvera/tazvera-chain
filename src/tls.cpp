#include "taz/tls.hpp"
#include "taz/crypto.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>

namespace taz {
namespace {

std::once_flag g_init_once;

void init_once() {
    std::call_once(g_init_once, [] {
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
    });
}

std::string last_error() {
    const unsigned long code = ERR_get_error();
    if (code == 0) return "unknown TLS error";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    ERR_clear_error();
    return std::string(buf);
}

// Returns false if the OS refused the timeout: then the blocking call has no bound and must not run.
bool apply_socket_timeout(socket_t fd, int ms) {
    const int clamped = std::max(1, ms);
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(clamped);
    const SOCKET s = static_cast<SOCKET>(fd);
    if (::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value)) != 0) return false;
    if (::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value)) != 0) return false;
    return true;
#else
    timeval tv{};
    tv.tv_sec = clamped / 1000;
    tv.tv_usec = (clamped % 1000) * 1000;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return false;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return false;
    return true;
#endif
}

// TLS needs a socket timeout to cut off a stalled peer, but it must not leave its own behind: the same socket
// is used by the rest of the node, and changing its timeout would quietly change how everything else behaves.
struct TimeoutGuard {
#ifdef _WIN32
    DWORD old_recv{0};
    DWORD old_send{0};
#else
    timeval old_recv{};
    timeval old_send{};
#endif
    socket_t fd{kInvalidSocket};
    bool recv_saved{false};
    bool send_saved{false};

    explicit TimeoutGuard(socket_t handle) : fd(handle) {
        // Saved separately: if only one of them can be read back, only that one is restored. Restoring a value
        // we never read would be worse than leaving the timeout alone.
#ifdef _WIN32
        const SOCKET s = static_cast<SOCKET>(fd);
        int len = static_cast<int>(sizeof(DWORD));
        recv_saved = ::getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&old_recv), &len) == 0;
        len = static_cast<int>(sizeof(DWORD));
        send_saved = ::getsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&old_send), &len) == 0;
#else
        socklen_t len = static_cast<socklen_t>(sizeof(timeval));
        recv_saved = ::getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &old_recv, &len) == 0;
        len = static_cast<socklen_t>(sizeof(timeval));
        send_saved = ::getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &old_send, &len) == 0;
#endif
    }
    ~TimeoutGuard() { restore(); }
    TimeoutGuard(const TimeoutGuard&) = delete;
    TimeoutGuard& operator=(const TimeoutGuard&) = delete;

    void restore() noexcept {
#ifdef _WIN32
        const SOCKET s = static_cast<SOCKET>(fd);
        if (recv_saved) ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&old_recv), sizeof(DWORD));
        if (send_saved) ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&old_send), sizeof(DWORD));
#else
        if (recv_saved) ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &old_recv, sizeof(timeval));
        if (send_saved) ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &old_send, sizeof(timeval));
#endif
        recv_saved = false;
        send_saved = false;
    }
};

bool expired(Deadline deadline) {
    return deadline != no_deadline() && std::chrono::steady_clock::now() >= deadline;
}

int remaining_ms(Deadline deadline) {
    if (deadline == no_deadline()) return 30000;   // no deadline: keep the node's ordinary per-call timeout
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    return left <= 0 ? 1 : static_cast<int>(left);
}

struct CtxDeleter { void operator()(SSL_CTX* p) const { if (p) SSL_CTX_free(p); } };
struct SslDeleter { void operator()(SSL* p) const { if (p) SSL_free(p); } };
struct CertDeleter { void operator()(X509* p) const { if (p) X509_free(p); } };
struct KeyDeleter { void operator()(EVP_PKEY* p) const { if (p) EVP_PKEY_free(p); } };
struct PkeyCtxDeleter { void operator()(EVP_PKEY_CTX* p) const { if (p) EVP_PKEY_CTX_free(p); } };

using CtxPtr = std::unique_ptr<SSL_CTX, CtxDeleter>;
using SslPtr = std::unique_ptr<SSL, SslDeleter>;
using CertPtr = std::unique_ptr<X509, CertDeleter>;
using KeyPtr = std::unique_ptr<EVP_PKEY, KeyDeleter>;

// A throwaway self-signed certificate. It proves nothing on its own; it exists so the peer can pin its
// fingerprint. Identity stays with the signed hello, which uses the node's own Ed25519 key.
CertPtr make_self_signed(KeyPtr& key, std::string* error) {
    std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter> gen(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
    if (!gen || EVP_PKEY_keygen_init(gen.get()) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(gen.get(), NID_X9_62_prime256v1) <= 0) {
        if (error) *error = "could not start TLS key generation: " + last_error();
        return nullptr;
    }
    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(gen.get(), &raw_key) <= 0) {
        if (error) *error = "could not generate TLS key: " + last_error();
        return nullptr;
    }
    key.reset(raw_key);

    CertPtr cert(X509_new());
    if (!cert) { if (error) *error = "could not allocate TLS certificate"; return nullptr; }
    if (!X509_set_version(cert.get(), 2) ||
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) == 0 ||
        !X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60L * 24L * 365L) ||
        !X509_set_pubkey(cert.get(), key.get())) {
        if (error) *error = "could not build TLS certificate: " + last_error();
        return nullptr;
    }
    X509_NAME* name = X509_get_subject_name(cert.get());
    const char* common_name = "tazvera-node";
    if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                            reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0) == 0 ||
        !X509_set_issuer_name(cert.get(), name)) {
        if (error) *error = "could not name TLS certificate: " + last_error();
        return nullptr;
    }
    if (!X509_sign(cert.get(), key.get(), EVP_sha256())) {
        if (error) *error = "could not sign TLS certificate: " + last_error();
        return nullptr;
    }
    return cert;
}

Hash32 fingerprint_of(X509* cert) {
    Hash32 out{};
    if (!cert) return out;
    unsigned char* der = nullptr;
    const int len = i2d_X509(cert, &der);
    if (len <= 0 || der == nullptr) return out;
    const Hash32 digest = sha256(std::span<const std::uint8_t>(der, static_cast<std::size_t>(len)));
    OPENSSL_free(der);
    return digest;
}

// Accept any certificate: the question here is not "is this trusted" but "which key is this", and the answer
// is compared with the allowlist by the caller.
int accept_any_certificate(int /*preverify_ok*/, X509_STORE_CTX* /*store*/) { return 1; }

// A stalled peer and a broken connection both surface as SSL_ERROR_SYSCALL. The errno is what tells them apart,
// and blaming the deadline for a reset connection would hide a real fault.
bool syscall_was_timeout() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
#endif
}

std::string syscall_error_text() {
#ifdef _WIN32
    return "socket error " + std::to_string(WSAGetLastError());
#else
    return std::string(std::strerror(errno));
#endif
}

// "Did we run out of time?" must be answered precisely: a real syscall error before the deadline is not a
// timeout, and a socket timeout is a timeout even when the clock is a millisecond short of the deadline.
bool ran_out_of_time(int ssl_error_code, Deadline deadline) {
    if (deadline == no_deadline()) return false;
    if (ssl_error_code == SSL_ERROR_SYSCALL) return syscall_was_timeout();
    return expired(deadline);
}

// SSL_accept/SSL_connect block, so the OS timeout does the cutting off and this loop does the accounting.
bool run_handshake(SSL* ssl, socket_t fd, bool server, Deadline deadline, std::string* error) {
    TimeoutGuard guard(fd);   // the socket's own timeout goes back when we leave, pass or fail
    for (;;) {
        if (expired(deadline)) {
            if (error) *error = "TLS handshake deadline expired";
            return false;
        }
        if (!apply_socket_timeout(fd, remaining_ms(deadline))) {
            if (error) *error = "could not set the socket timeout for the TLS handshake";
            return false;
        }
        ERR_clear_error();
        const int rc = server ? SSL_accept(ssl) : SSL_connect(ssl);
        if (rc == 1) return true;
        const int code = SSL_get_error(ssl, rc);
        if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) continue;
        if (error) {
            *error = ran_out_of_time(code, deadline)
                         ? "TLS handshake deadline expired"
                         : "TLS handshake failed: " +
                               (code == SSL_ERROR_SYSCALL ? syscall_error_text() : last_error());
        }
        return false;
    }
}

}  // namespace

struct TlsContext::Impl {
    CtxPtr ctx;
    KeyPtr key;
    CertPtr cert;
    bool server{false};
};

struct TlsContext::Conn::Impl {
    SslPtr ssl;
};

TlsContext::TlsContext() : impl_(std::make_unique<Impl>()) {}
TlsContext::~TlsContext() = default;

std::unique_ptr<TlsContext> TlsContext::make_server(std::string* error) {
    init_once();
    std::unique_ptr<TlsContext> self(new TlsContext());
    self->impl_->server = true;
    self->impl_->ctx.reset(SSL_CTX_new(TLS_server_method()));
    if (!self->impl_->ctx) { if (error) *error = "could not create TLS server context: " + last_error(); return nullptr; }
    if (!SSL_CTX_set_min_proto_version(self->impl_->ctx.get(), TLS1_3_VERSION) ||
        !SSL_CTX_set_max_proto_version(self->impl_->ctx.get(), TLS1_3_VERSION)) {
        if (error) *error = "could not pin TLS 1.3: " + last_error();
        return nullptr;
    }
    self->impl_->cert = make_self_signed(self->impl_->key, error);
    if (!self->impl_->cert) return nullptr;
    if (SSL_CTX_use_certificate(self->impl_->ctx.get(), self->impl_->cert.get()) != 1 ||
        SSL_CTX_use_PrivateKey(self->impl_->ctx.get(), self->impl_->key.get()) != 1) {
        if (error) *error = "could not load TLS certificate into the context: " + last_error();
        return nullptr;
    }
    // Ask the peer for its certificate so its fingerprint can be pinned, but treat it as untrusted.
    SSL_CTX_set_verify(self->impl_->ctx.get(), SSL_VERIFY_PEER, accept_any_certificate);
    self->fingerprint_ = fingerprint_of(self->impl_->cert.get());
    return self;
}

std::unique_ptr<TlsContext> TlsContext::make_client(std::string* error) {
    init_once();
    std::unique_ptr<TlsContext> self(new TlsContext());
    self->impl_->ctx.reset(SSL_CTX_new(TLS_client_method()));
    if (!self->impl_->ctx) { if (error) *error = "could not create TLS client context: " + last_error(); return nullptr; }
    if (!SSL_CTX_set_min_proto_version(self->impl_->ctx.get(), TLS1_3_VERSION) ||
        !SSL_CTX_set_max_proto_version(self->impl_->ctx.get(), TLS1_3_VERSION)) {
        if (error) *error = "could not pin TLS 1.3: " + last_error();
        return nullptr;
    }
    // The client presents a certificate too, so the server can pin it — and verifies nothing itself.
    self->impl_->cert = make_self_signed(self->impl_->key, error);
    if (!self->impl_->cert) return nullptr;
    if (SSL_CTX_use_certificate(self->impl_->ctx.get(), self->impl_->cert.get()) != 1 ||
        SSL_CTX_use_PrivateKey(self->impl_->ctx.get(), self->impl_->key.get()) != 1) {
        if (error) *error = "could not load TLS certificate into the context: " + last_error();
        return nullptr;
    }
    SSL_CTX_set_verify(self->impl_->ctx.get(), SSL_VERIFY_NONE, nullptr);
    self->fingerprint_ = fingerprint_of(self->impl_->cert.get());
    return self;
}

std::unique_ptr<TlsContext::Conn> TlsContext::accept(const Socket& socket, std::string* error, Deadline deadline) {
    if (!impl_->server) { if (error) *error = "this TLS context cannot accept connections"; return nullptr; }
    std::unique_ptr<Conn> conn(new Conn());
    conn->impl_->ssl.reset(SSL_new(impl_->ctx.get()));
    if (!conn->impl_->ssl) { if (error) *error = "could not create TLS session: " + last_error(); return nullptr; }
    if (SSL_set_fd(conn->impl_->ssl.get(), static_cast<int>(socket.raw())) != 1) {
        if (error) *error = "could not attach the socket to TLS: " + last_error();
        return nullptr;
    }
    if (!run_handshake(conn->impl_->ssl.get(), socket.raw(), true, deadline, error)) return nullptr;
    return conn;
}

std::unique_ptr<TlsContext::Conn> TlsContext::connect(const Socket& socket, std::string* error, Deadline deadline) {
    if (impl_->server) { if (error) *error = "this TLS context cannot start connections"; return nullptr; }
    std::unique_ptr<Conn> conn(new Conn());
    conn->impl_->ssl.reset(SSL_new(impl_->ctx.get()));
    if (!conn->impl_->ssl) { if (error) *error = "could not create TLS session: " + last_error(); return nullptr; }
    if (SSL_set_fd(conn->impl_->ssl.get(), static_cast<int>(socket.raw())) != 1) {
        if (error) *error = "could not attach the socket to TLS: " + last_error();
        return nullptr;
    }
    if (!run_handshake(conn->impl_->ssl.get(), socket.raw(), false, deadline, error)) return nullptr;
    return conn;
}

TlsContext::Conn::Conn() : impl_(std::make_unique<Impl>()) {}
TlsContext::Conn::~Conn() = default;

Hash32 TlsContext::Conn::peer_certificate_fingerprint() const {
    if (!impl_->ssl) return Hash32{};
    X509* peer = SSL_get_peer_certificate(impl_->ssl.get());
    if (!peer) return Hash32{};
    const Hash32 out = fingerprint_of(peer);
    X509_free(peer);
    return out;
}

bool TlsContext::Conn::send_all(std::string_view bytes, std::string* error, Deadline deadline) {
    const socket_t fd = SSL_get_fd(impl_->ssl.get());
    TimeoutGuard guard(fd);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        if (expired(deadline)) { if (error) *error = "TLS write deadline expired"; return false; }
        if (!apply_socket_timeout(fd, remaining_ms(deadline))) {
            if (error) *error = "could not set the socket timeout for TLS I/O";
            return false;
        }
        const int n = SSL_write(impl_->ssl.get(), bytes.data() + sent,
                                static_cast<int>(std::min<std::size_t>(bytes.size() - sent, 1u << 20)));
        if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
        const int code = SSL_get_error(impl_->ssl.get(), n);
        if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) continue;
        if (error) {
            *error = ran_out_of_time(code, deadline)
                         ? "TLS write deadline expired"
                         : "TLS write failed: " +
                               (code == SSL_ERROR_SYSCALL ? syscall_error_text() : last_error());
        }
        return false;
    }
    return true;
}

bool TlsContext::Conn::recv_exact(std::size_t count, std::string& out, std::string* error, Deadline deadline) {
    const socket_t fd = SSL_get_fd(impl_->ssl.get());
    TimeoutGuard guard(fd);
    out.assign(count, '\0');
    std::size_t got = 0;
    while (got < count) {
        if (expired(deadline)) { if (error) *error = "TLS read deadline expired"; return false; }
        if (!apply_socket_timeout(fd, remaining_ms(deadline))) {
            if (error) *error = "could not set the socket timeout for TLS I/O";
            return false;
        }
        const int n = SSL_read(impl_->ssl.get(), &out[got],
                               static_cast<int>(std::min<std::size_t>(count - got, 1u << 20)));
        if (n > 0) { got += static_cast<std::size_t>(n); continue; }
        const int code = SSL_get_error(impl_->ssl.get(), n);
        if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) continue;
        if (error) {
            *error = ran_out_of_time(code, deadline)
                         ? "TLS read deadline expired"
                         : "TLS read failed: " +
                               (code == SSL_ERROR_SYSCALL ? syscall_error_text() : last_error());
        }
        return false;
    }
    return true;
}

}  // namespace taz
