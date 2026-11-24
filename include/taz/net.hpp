#pragma once
#include "taz/hello.hpp"
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace taz {

// v9.0: loopback transport for the handshake. Two processes on one machine, nothing on the internet.
//
// Deliberate limits of this step:
//   - it binds 127.0.0.1 only; there is no way to ask it to listen on 0.0.0.0;
//   - it carries one message at a time and no peer discovery;
//   - a successful handshake means "same chain", never "trusted peer". Hello is not authenticated, so any
//     stranger can claim to be a node. Peer identity is a separate milestone.
//
// Framing is decided here and used by every later message: u32 big-endian payload length, u8 message type, then
// the payload. Bounded on both ends, so a peer cannot make the node allocate more than kMaxPayloadBytes.

#ifdef _WIN32
using socket_t = std::uintptr_t;
inline constexpr socket_t kInvalidSocket = static_cast<socket_t>(~static_cast<std::uintptr_t>(0));
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

// v9.1: 32 MiB (was 64 KiB). Consensus allows MAX_BLOCK_TXS (10000) transactions in a block, and a block of ~150
// ordinary transfers is already ~128 KB of JSON: with a 64 KiB limit a perfectly valid block, made by anyone
// (or by a producer), could never be synced to any other node. test_hardening91 measures the worst-case block and
// requires it to fit. The bound is still checked BEFORE anything is allocated, and recv_exact no longer reserves
// what the peer announced, so a four-byte header cannot buy a 32 MiB allocation.
inline constexpr std::uint32_t kMaxPayloadBytes = 32u * 1024u * 1024u;   // one message, not a stream
inline constexpr std::uint8_t kMsgHello = 1;
inline constexpr std::uint8_t kMsgChain = 2;   // a whole chain, as the export format writes it
inline constexpr std::uint8_t kMsgBlocks = 3;  // one block, as the export format writes it
inline constexpr std::uint8_t kMsgWant = 4;    // "give me your block at this height" (decimal payload)
inline constexpr std::uint8_t kMsgNoBlock = 5; // "I do not have that block"
inline constexpr std::uint8_t kMsgAuth = 6;    // an identity proof bound to this connection
inline constexpr std::uint8_t kMsgAccept = 7;  // the accepter confirming it will talk to this peer
inline constexpr int kTimeoutMs = 3000;

// WSAStartup on Windows, nothing elsewhere. Call once before any socket work.
bool net_startup(std::string* error = nullptr);
void net_cleanup();

class Socket {
public:
    Socket() = default;
    explicit Socket(socket_t handle) noexcept : handle_(handle) {}
    ~Socket() { close(); }
    Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalidSocket; }
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool valid() const noexcept { return handle_ != kInvalidSocket; }
    socket_t raw() const noexcept { return handle_; }
    void close() noexcept;

private:
    socket_t handle_{kInvalidSocket};
};

struct Listener {
    Socket socket;
    std::uint16_t port{0};   // the real port, which matters when 0 was requested
};

// port 0 asks the OS for a free port; the chosen one comes back in Listener::port.
bool listen_loopback(std::uint16_t port, Listener& out, std::string* error = nullptr);
bool connect_loopback(std::uint16_t port, Socket& out, std::string* error = nullptr);
// Raw bytes, for framing tests and for the transport itself.
bool send_all(const Socket& socket, std::string_view bytes, std::string* error = nullptr);
// A deadline of TimePoint::max() means "no deadline beyond the per-call socket timeout". Anything else is an
// absolute point in time: a peer that keeps trickling bytes is cut off there, not three seconds after each one.
using Deadline = std::chrono::steady_clock::time_point;
inline Deadline no_deadline() { return Deadline::max(); }

// With a deadline, accept_one() waits only until then and reports a timeout instead of blocking for ever.
bool accept_one(const Socket& listener, Socket& out, std::string* error = nullptr,
                Deadline deadline = no_deadline());
bool recv_exact(const Socket& socket, std::size_t count, std::string& out, std::string* error = nullptr,
                Deadline deadline = no_deadline());

bool send_message(const Socket& socket, std::uint8_t type, std::string_view payload, std::string* error = nullptr,
                  Deadline deadline = no_deadline());
bool recv_message(const Socket& socket, std::uint8_t& type, std::string& payload, std::string* error = nullptr,
                  Deadline deadline = no_deadline());

// Compatibility-only chain handshake: checks framing and exchanges hello data, but does NOT authenticate a peer.
// `send_first` must be true on exactly one of the two ends. Use exchange_authenticated_hello for key proof.
bool exchange_hello(const Socket& socket, const Hello& ours, Hello& theirs, bool send_first,
                    std::string* error = nullptr, Deadline deadline = no_deadline(),
                    std::string* their_raw_hello = nullptr);

// Stronger handshake: both peers sign their fresh hello and prove control of that identity over
// the transcript containing both exact hello frames. This authenticates keys, not peer authorization.
bool exchange_authenticated_hello(const Socket& socket, Hello& ours, const PrivKey& private_seed,
                                   Hello& theirs, bool send_first, std::string* error = nullptr,
                                   Deadline deadline = no_deadline());

}  // namespace taz
