#include "taz/net.hpp"
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

bool accept_one(const Socket& listener, Socket& out, std::string* error) {
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
        const int chunk = ::send(socket.raw(), bytes.data() + sent,
                                 static_cast<int>(bytes.size() - sent), 0);
        if (chunk <= 0) {
            if (error) *error = "the connection closed while sending";
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }
    return true;
}

bool recv_exact(const Socket& socket, std::size_t count, std::string& out, std::string* error) {
    out.clear();
    out.reserve(count);
    std::vector<char> buffer(1024);
    while (out.size() < count) {
        const std::size_t want = std::min(buffer.size(), count - out.size());
        const int got = ::recv(socket.raw(), buffer.data(), static_cast<int>(want), 0);
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

bool send_message(const Socket& socket, std::uint8_t type, std::string_view payload, std::string* error) {
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

bool recv_message(const Socket& socket, std::uint8_t& type, std::string& payload, std::string* error) {
    std::string header;
    if (!recv_exact(socket, 4, header, error)) return false;
    const std::uint32_t length = get_u32(header);
    if (length > kMaxPayloadBytes) {                 // refuse before allocating anything
        if (error) *error = "the peer announced a message larger than this transport allows";
        return false;
    }
    std::string type_byte;
    if (!recv_exact(socket, 1, type_byte, error)) return false;
    type = static_cast<std::uint8_t>(type_byte[0]);
    if (length == 0) { payload.clear(); return true; }
    return recv_exact(socket, length, payload, error);
}

bool exchange_hello(const Socket& socket, const Hello& ours, Hello& theirs, bool send_first, std::string* error) {
    std::uint8_t type = 0;
    std::string payload;
    auto send_ours = [&]() { return send_message(socket, kMsgHello, encode_hello(ours), error); };
    auto recv_theirs = [&]() {
        if (!recv_message(socket, type, payload, error)) return false;
        if (type != kMsgHello) {
            if (error) *error = "the peer did not start with a hello";
            return false;
        }
        std::string why;
        if (!decode_hello(payload, theirs, &why)) {
            if (error) *error = "the peer sent a hello this node cannot read: " + why;
            return false;
        }
        return true;
    };
    if (send_first) return send_ours() && recv_theirs();
    return recv_theirs() && send_ours();
}

}  // namespace taz
