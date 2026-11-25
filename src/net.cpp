#include "taz/net.hpp"
#include "taz/auth.hpp"
#include "taz/crypto.hpp"
#include <algorithm>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace taz {
namespace {

#ifdef _WIN32
bool would_block() { return WSAGetLastError() == WSAETIMEDOUT; }
#else
bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

void put_u32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}

std::uint32_t get_u32(const std::string& in) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(in[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(in[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(in[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(in[3]));
}

bool set_timeout_ms(const Socket& s, int ms, std::string* error) {
#ifdef _WIN32
    DWORD value = static_cast<DWORD>(ms);
    if (setsockopt(static_cast<SOCKET>(s.raw()), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value)) != 0 ||
        setsockopt(static_cast<SOCKET>(s.raw()), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
        if (error) *error = "could not set the socket timeouts";
        return false;
    }
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (setsockopt(s.raw(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        setsockopt(s.raw(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        if (error) *error = "could not set the socket timeouts";
        return false;
    }
#endif
    return true;
}

bool set_timeout(const Socket& s, std::string* error) {
    // Both directions: a receive that never completes and a send that never drains are the same problem, and
    // a peer that stops reading would otherwise hold this process for as long as it likes.
#ifdef _WIN32
    DWORD ms = kTimeoutMs;
    if (setsockopt(static_cast<SOCKET>(s.raw()), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms)) != 0 ||
        setsockopt(static_cast<SOCKET>(s.raw()), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms)) != 0) {
        if (error) *error = "could not set the socket timeouts";
        return false;
    }
#else
    timeval tv{};
    tv.tv_sec = kTimeoutMs / 1000;
    tv.tv_usec = (kTimeoutMs % 1000) * 1000;
    if (setsockopt(s.raw(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        setsockopt(s.raw(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        if (error) *error = "could not set the socket timeouts";
        return false;
    }
#endif
    return true;
}

int close_handle(socket_t handle) {
#ifdef _WIN32
    return closesocket(static_cast<SOCKET>(handle));
#else
    return ::close(handle);
#endif
}

}  // namespace

bool net_startup(std::string* error) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        if (error) *error = "WSAStartup failed";
        return false;
    }
#else
    (void)error;
#endif
    return true;
}

void net_cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

void Socket::close() noexcept {
    if (handle_ == kInvalidSocket) return;
    close_handle(handle_);
    handle_ = kInvalidSocket;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) { close(); handle_ = other.handle_; other.handle_ = kInvalidSocket; }
    return *this;
}

bool listen_loopback(std::uint16_t port, Listener& out, std::string* error) {
    const socket_t handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle == kInvalidSocket) {
        if (error) *error = "could not create the listening socket";
        return false;
    }
    Socket socket(handle);
    // No SO_REUSEADDR: on Windows it lets another process take the port, and this code never needs it.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 only, never 0.0.0.0
    if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (error) *error = "could not bind 127.0.0.1:" + std::to_string(port);
        return false;
    }
    if (::listen(handle, 4) != 0) {
        if (error) *error = "could not listen";
        return false;
    }
    sockaddr_in bound{};
#ifdef _WIN32
    int len = static_cast<int>(sizeof(bound));
#else
    socklen_t len = sizeof(bound);
#endif
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        if (error) *error = "could not read the bound port";
        return false;
    }
    out.socket = std::move(socket);
    out.port = ntohs(bound.sin_port);
    return true;
}

bool connect_loopback(std::uint16_t port, Socket& out, std::string* error) {
    const socket_t handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle == kInvalidSocket) {
        if (error) *error = "could not create the socket";
        return false;
    }
    Socket socket(handle);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (error) *error = "could not connect to 127.0.0.1:" + std::to_string(port);
        return false;
    }
    if (!set_timeout(socket, error)) return false;
    out = std::move(socket);
    return true;
}

bool accept_one(const Socket& listener, Socket& out, std::string* error, Deadline deadline) {
    if (deadline != no_deadline()) {
        // select() is the portable way to wait for a connection with a limit; accept() alone blocks for ever, and
        // a node that can only be stopped by killing it is a node that leaks processes in CI.
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            if (error) *error = "no peer arrived in time";
            return false;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        timeval tv{};
        tv.tv_sec = static_cast<long>(left / 1000);
        tv.tv_usec = static_cast<long>((left % 1000) * 1000);
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(static_cast<socket_t>(listener.raw()), &readable);
#ifdef _WIN32
        const int ready = ::select(0, &readable, nullptr, nullptr, &tv);
#else
        const int ready = ::select(listener.raw() + 1, &readable, nullptr, nullptr, &tv);
#endif
        if (ready == 0) {
            if (error) *error = "no peer arrived in time";
            return false;
        }
        if (ready < 0) {
            if (error) *error = "waiting for a peer failed";
            return false;
        }
    }
    sockaddr_in peer{};
#ifdef _WIN32
    int len = static_cast<int>(sizeof(peer));
#else
    socklen_t len = sizeof(peer);
#endif
    const socket_t handle = ::accept(listener.raw(), reinterpret_cast<sockaddr*>(&peer), &len);
    if (handle == kInvalidSocket) {
        if (error) *error = "could not accept a connection";
        return false;
    }
    Socket socket(handle);
    if (!set_timeout(socket, error)) return false;
    out = std::move(socket);
    return true;
}

bool send_all(const Socket& socket, std::string_view bytes, std::string* error) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        // MSG_NOSIGNAL: without it a peer that closed the connection kills this process with SIGPIPE instead of
        // returning an error. A node that dies because a stranger hung up is not a node.
#ifdef _WIN32
        const int flags = 0;
#else
        const int flags = MSG_NOSIGNAL;
#endif
        const auto chunk = ::send(socket.raw(), bytes.data() + sent,
                                  static_cast<int>(bytes.size() - sent), flags);
        if (chunk <= 0) {
            if (error) *error = "the connection closed while sending";
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }
    return true;
}

bool recv_exact(const Socket& socket, std::size_t count, std::string& out, std::string* error, Deadline deadline) {
    out.clear();
    // v9.1: no out.reserve(count). `count` is the length the PEER announced; reserving it up front hands any
    // stranger a large allocation for a five-byte header. The string grows as real bytes arrive.
    std::vector<char> buffer(16 * 1024);
    while (out.size() < count) {
        // The per-call socket timeout alone lets a peer that trickles one byte every few seconds hold this
        // process for as long as it likes. With a deadline, the remaining time also becomes the socket timeout,
        // so the cut-off is the deadline and not three seconds after the last byte.
        if (deadline != no_deadline()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                if (error) *error = "the peer took longer than the time allowed for this exchange";
                return false;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const int ms = static_cast<int>(std::min<long long>(left <= 0 ? 1 : left, kTimeoutMs));
            if (!set_timeout_ms(socket, ms, nullptr)) {
                if (error) *error = "could not set the socket timeouts";
                return false;
            }
        }
        const std::size_t want = std::min(buffer.size(), count - out.size());
        const auto got = ::recv(socket.raw(), buffer.data(), static_cast<int>(want), 0);
        if (got == 0) {
            if (error) *error = "the connection closed while receiving";
            return false;
        }
        if (got < 0) {
            if (error) *error = would_block() ? "timed out waiting for the peer" : "receive failed";
            return false;
        }
        out.append(buffer.data(), static_cast<std::size_t>(got));
    }
    return true;
}

namespace {
constexpr std::size_t kChunkMetadataBytes = 13; // original type + three u32 values

void shutdown_connection(const Socket& socket) {
#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(socket.raw()), SD_BOTH);
#else
    ::shutdown(socket.raw(), SHUT_RDWR);
#endif
}

bool read_frame(const Socket& socket, std::uint8_t& type, std::string& payload,
                std::string* error, Deadline deadline) {
    std::string header;
    if (!recv_exact(socket, 4, header, error, deadline)) return false;
    const std::uint32_t length = get_u32(header);
    // The legacy length field counts payload bytes only; the type byte and prefix also consume wire space.
    if (static_cast<std::uint64_t>(length) + 5 > kMaxWireMessageBytes) {
        if (error) *error = "message too large";
        shutdown_connection(socket);
        return false;
    }
    std::string type_byte;
    if (!recv_exact(socket, 1, type_byte, error, deadline)) return false;
    type = static_cast<std::uint8_t>(type_byte[0]);
    payload.clear();
    if (length == 0) return true;
    if (!recv_exact(socket, length, payload, error, deadline)) return false;
    return true;
}
} // namespace

bool send_message(const Socket& socket, std::uint8_t type, std::string_view payload, std::string* error,
                  Deadline deadline) {
    // The send side gets the same treatment as the receive side: if the connection has a deadline, the time left
    // becomes the socket timeout, so a peer that stops reading cannot hold a send past it.
    if (deadline != no_deadline()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            if (error) *error = "the peer took longer than the time allowed for this exchange";
            return false;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int ms = static_cast<int>(std::min<long long>(left <= 0 ? 1 : left, kTimeoutMs));
        if (!set_timeout_ms(socket, ms, nullptr)) {
            if (error) *error = "could not set the socket timeouts";
            return false;
        }
    }
    if (payload.size() > kMaxPayloadBytes) {
        if (error) *error = "the message is larger than this transport allows";
        return false;
    }
    std::string frame;
    frame.reserve(payload.size() + 5);
    put_u32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.push_back(static_cast<char>(type));
    frame.append(payload.data(), payload.size());
    return send_all(socket, frame, error);
}

bool recv_message(const Socket& socket, std::uint8_t& type, std::string& payload, std::string* error,
                  Deadline deadline) {
    std::uint8_t wire_type = 0;
    std::string wire_payload;
    if (!read_frame(socket, wire_type, wire_payload, error, deadline)) return false;
    if (wire_type != kMsgChunk) {
        if (wire_payload.size() > kMaxStreamBytes) {
            if (error) *error = "message too large";
            shutdown_connection(socket);
            return false;
        }
        type = wire_type;
        payload = std::move(wire_payload);
        return true;
    }

    auto reject = [&](const char* reason) {
        if (error) *error = reason;
        shutdown_connection(socket);
        return false;
    };
    auto incomplete = [&]() {
        if (error) *error = "incomplete stream";
        shutdown_connection(socket);
        return false;
    };
    if (wire_payload.size() < kChunkMetadataBytes) return reject("message too large");

    const std::uint8_t original_type = static_cast<std::uint8_t>(wire_payload[0]);
    if (original_type == kMsgChunk) return reject("invalid original message type");
    const std::uint32_t stream_id = get_u32(wire_payload.substr(1, 4));
    const std::uint32_t first_index = get_u32(wire_payload.substr(5, 4));
    const std::uint32_t total_chunks = get_u32(wire_payload.substr(9, 4));
    if (total_chunks == 0 || total_chunks > kMaxStreamBytes)
        return reject("stream too large");
    if (first_index != 0) return reject("chunk out of order");
    if (wire_payload.size() == kChunkMetadataBytes) return reject("message too large");

    std::string assembled;
    const std::size_t first_data = wire_payload.size() - kChunkMetadataBytes;
    if (first_data > kMaxStreamBytes) return reject("stream too large");
    assembled.append(wire_payload.data() + kChunkMetadataBytes, first_data);

    for (std::uint32_t expected = 1; expected < total_chunks; ++expected) {
        std::uint8_t next_type = 0;
        std::string next_payload;
        if (!read_frame(socket, next_type, next_payload, error, deadline)) {
            if (error && error->find("message too large") == 0) {
                shutdown_connection(socket);
                return false;
            }
            return incomplete();
        }
        if (next_type != kMsgChunk) return reject("incomplete stream");
        if (next_payload.size() < kChunkMetadataBytes) return reject("message too large");
        const std::uint8_t next_original = static_cast<std::uint8_t>(next_payload[0]);
        if (next_original == kMsgChunk) return reject("invalid original message type");
        if (next_original != original_type) return reject("original_type changed");
        const std::uint32_t next_stream = get_u32(next_payload.substr(1, 4));
        const std::uint32_t next_index = get_u32(next_payload.substr(5, 4));
        const std::uint32_t next_total = get_u32(next_payload.substr(9, 4));
        if (next_total != total_chunks) return reject("total_chunks changed");
        if (next_stream != stream_id || next_index != expected) return reject("chunk out of order");
        const std::size_t data_size = next_payload.size() - kChunkMetadataBytes;
        if (data_size == 0) return reject("message too large");
        if (data_size > kMaxStreamBytes - assembled.size()) return reject("stream too large");
        assembled.append(next_payload.data() + kChunkMetadataBytes, data_size);
    }
    if (assembled.size() > kMaxStreamBytes) return reject("stream too large");
    type = original_type;
    payload = std::move(assembled);
    return true;
}

bool exchange_hello(const Socket& socket, const Hello& ours, Hello& theirs, bool send_first, std::string* error,
                    Deadline deadline, std::string* their_raw_hello) {
    std::uint8_t type = 0;
    std::string payload;
    auto send_ours = [&]() { return send_message(socket, kMsgHello, encode_hello(ours), error, deadline); };
    auto recv_theirs = [&]() {
        if (!recv_message(socket, type, payload, error, deadline)) return false;
        if (type != kMsgHello) {
            if (error) *error = "the peer did not start with a hello";
            return false;
        }
        std::string why;
        if (!decode_hello(payload, theirs, &why)) {
            if (error) *error = "the peer sent a hello this node cannot read: " + why;
            return false;
        }
        // The bytes that actually arrived: the transcript signs these, not a re-encoding of them.
        if (their_raw_hello) *their_raw_hello = payload;
        return true;
    };
    if (send_first) return send_ours() && recv_theirs();
    return recv_theirs() && send_ours();
}

bool exchange_authenticated_hello(const Socket& socket, Hello& ours, const PrivKey& private_seed,
                                  Hello& theirs, bool send_first, std::string* error, Deadline deadline) {
    auto fail = [&](const std::string& reason) {
        if (error) *error = reason;
        return false;
    };

    // Never trust a caller-supplied nonce here: a non-zero value may still be stale or reused.
    // Generate a fresh unpredictable nonce for every authenticated connection before signing.
    if (!random_bytes(ours.nonce)) return fail("could not generate a fresh local nonce");
    if (std::all_of(private_seed.begin(), private_seed.end(), [](std::uint8_t b) { return b == 0; }))
        return fail("local identity key is empty");
    if (!sign_hello(ours, private_seed)) return fail("could not sign the local hello");

    const std::string our_raw_hello = encode_hello(ours);
    std::string their_raw_hello;
    if (!exchange_hello(socket, ours, theirs, send_first, error, deadline, &their_raw_hello)) return false;

    const HandshakeDecision decision = check_peer(ours, theirs);
    if (!decision.ok) return fail("peer hello refused: " + decision.reason);
    if (!hello_has_identity(theirs)) return fail("peer did not prove an identity");
    if (!hello_signature_ok(theirs)) return fail("peer hello identity signature is invalid");

    // The second signature binds both exact hello frames to this connection, not merely to a reusable key.
    const std::string transcript = auth_transcript(ours, our_raw_hello, theirs, their_raw_hello);
    if (transcript.empty()) return fail("could not build the authentication transcript");

    AuthMessage our_auth;
    if (!sign_auth(our_auth, private_seed, transcript)) return fail("could not sign the authentication transcript");
    if (!send_message(socket, kMsgAuth, encode_auth(our_auth), error, deadline)) return false;

    std::uint8_t type = 0;
    std::string payload;
    if (!recv_message(socket, type, payload, error, deadline)) return false;
    if (type != kMsgAuth) return fail("the peer did not send an authentication proof");

    AuthMessage their_auth;
    std::string why;
    if (!decode_auth(payload, their_auth, &why)) return fail("the peer sent an invalid authentication proof: " + why);
    if (their_auth.pubkey != theirs.pubkey) return fail("the authentication key does not match the signed hello");
    if (!auth_ok(their_auth, transcript)) return fail("the peer's connection-bound identity proof is invalid");
    return true;
}

}  // namespace taz
