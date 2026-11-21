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

inline constexpr std::uint32_t kMaxPayloadBytes = 64 * 1024;   // one message, not a stream
inline constexpr std::uint8_t kMsgHello = 1;
inline constexpr std::uint8_t kMsgChain = 2;   // a whole chain, as the export format writes it
inline constexpr std::uint8_t kMsgBlocks = 3;  // the blocks a peer is missing, as a JSON array
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
bool accept_one(const Socket& listener, Socket& out, std::string* error = nullptr);

// Raw bytes, for framing tests and for the transport itself.
bool send_all(const Socket& socket, std::string_view bytes, std::string* error = nullptr);
// A deadline of TimePoint::max() means "no deadline beyond the per-call socket timeout". Anything else is an
// absolute point in time: a peer that keeps trickling bytes is cut off there, not three seconds after each one.
using Deadline = std::chrono::steady_clock::time_point;
inline Deadline no_deadline() { return Deadline::max(); }
bool recv_exact(const Socket& socket, std::size_t count, std::string& out, std::string* error = nullptr,
                Deadline deadline = no_deadline());

bool send_message(const Socket& socket, std::uint8_t type, std::string_view payload, std::string* error = nullptr);
bool recv_message(const Socket& socket, std::uint8_t& type, std::string& payload, std::string* error = nullptr,
                  Deadline deadline = no_deadline());

// One side of the handshake. `send_first` must be true on exactly one of the two ends.
bool exchange_hello(const Socket& socket, const Hello& ours, Hello& theirs, bool send_first, std::string* error = nullptr);

}  // namespace taz
