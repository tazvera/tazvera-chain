#include "taz/api.hpp"
#include "taz/export.hpp"
#include "taz/net.hpp"
#include "taz/types.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace taz {
namespace {

constexpr std::size_t kMaxRequestLine = 8u * 1024u;
constexpr unsigned kMaxConnections = 8;
constexpr auto kRequestTimeout = std::chrono::seconds(10);

bool hex_value(char c, std::uint8_t& out) {
    if (c >= '0' && c <= '9') out = static_cast<std::uint8_t>(c - '0');
    else if (c >= 'a' && c <= 'f') out = static_cast<std::uint8_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') out = static_cast<std::uint8_t>(c - 'A' + 10);
    else return false;
    return true;
}

template <std::size_t N>
bool parse_hex(std::string_view text, std::array<std::uint8_t, N>& out) {
    if (text.size() != N * 2) return false;
    for (std::size_t i = 0; i < N; ++i) {
        std::uint8_t hi = 0, lo = 0;
        if (!hex_value(text[i * 2], hi) || !hex_value(text[i * 2 + 1], lo)) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::string json_string(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(hex[(c >> 4) & 0x0f]);
                out.push_back(hex[c & 0x0f]);
            } else out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
    return out;
}

struct Response {
    int status{200};
    std::string reason{"OK"};
    std::string body;
};

Response error_response(int status, std::string_view reason, std::string_view message) {
    return {status, std::string(reason), "{\"error\":" + json_string(message) + "}"};
}

std::string asset_json(const Hash32& id, const AssetRecord& asset) {
    return "{\"asset_id\":" + json_string(to_hex(id)) +
           ",\"asset_kind\":" + std::to_string(asset.asset_kind) +
           ",\"doc_hash\":" + json_string(to_hex(asset.doc_hash)) +
           ",\"created_at\":" + json_string(std::to_string(asset.created_at)) + "}";
}

Response route_get(const Chain& chain, std::string_view path) {
    if (path == "/status") {
        return {200, "OK",
            "{\"chain_id\":" + json_string(CHAIN_ID) +
            ",\"protocol_version\":" + std::to_string(PROTOCOL_VERSION) +
            ",\"height\": " + json_string(std::to_string(chain.height())) +
            ",\"genesis\":" + json_string(to_hex(chain.genesis_commitment())) + "}"};
    }

    constexpr std::string_view balance_prefix = "/balance/";
    if (path.starts_with(balance_prefix)) {
        Address address{};
        if (!parse_hex(path.substr(balance_prefix.size()), address))
            return error_response(400, "Bad Request", "address must be exactly 40 hexadecimal characters");
        return {200, "OK",
            "{\"balance\":" + json_string(std::to_string(chain.state().balance(address))) +
            ",\"nonce\":" + json_string(std::to_string(chain.state().nonce(address))) + "}"};
    }

    constexpr std::string_view assets_prefix = "/assets/";
    if (path.starts_with(assets_prefix)) {
        Address address{};
        if (!parse_hex(path.substr(assets_prefix.size()), address))
            return error_response(400, "Bad Request", "address must be exactly 40 hexadecimal characters");
        std::string body = "[";
        bool first = true;
        for (const auto& [id, asset] : chain.state().assets()) {
            if (asset.owner != address) continue;
            if (!first) body.push_back(',');
            first = false;
            body += asset_json(id, asset);
        }
        body.push_back(']');
        return {200, "OK", std::move(body)};
    }

    constexpr std::string_view asset_prefix = "/asset/";
    if (path.starts_with(asset_prefix)) {
        Hash32 id{};
        if (!parse_hex(path.substr(asset_prefix.size()), id))
            return error_response(400, "Bad Request", "asset id must be exactly 64 hexadecimal characters");
        const AssetRecord* asset = chain.state().find_asset(id);
        if (!asset) return error_response(404, "Not Found", "asset was not found");
        return {200, "OK", asset_json(id, *asset)};
    }

    constexpr std::string_view block_prefix = "/block/";
    if (path.starts_with(block_prefix)) {
        const std::string_view digits = path.substr(block_prefix.size());
        std::uint64_t height = 0;
        if (digits.empty()) return error_response(400, "Bad Request", "block height must be a decimal integer");
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), height);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
            return error_response(400, "Bad Request", "block height must be a decimal integer");
        if (height >= chain.blocks().size() || chain.blocks()[static_cast<std::size_t>(height)].height != height)
            return error_response(404, "Not Found", "block was not found");
        return {200, "OK", block_to_json(chain.blocks()[static_cast<std::size_t>(height)])};
    }

    return error_response(404, "Not Found", "endpoint was not found");
}

bool read_request_line(const Socket& socket, std::string& line, Deadline deadline) {
    line.clear();
    for (;;) {
        std::string byte;
        if (!recv_exact(socket, 1, byte, nullptr, deadline)) return false;
        line.push_back(byte[0]);
        if (line.size() > kMaxRequestLine) return false;
        if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n') {
            line.resize(line.size() - 2);
            return true;
        }
    }
}



struct RequestHeaders {
    std::string host;
    std::string origin;
    bool host_seen{false};
    bool host_duplicate{false};
    bool origin_seen{false};
    bool origin_duplicate{false};
};

bool ascii_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool read_request_headers(const Socket& socket, Deadline deadline, RequestHeaders& headers) {
    // Consume and validate all headers before replying; never trust a browser-supplied Host value.
    constexpr std::size_t kMaxHeaderBytes = 8u * 1024u;
    std::size_t consumed = 0;
    std::string line;
    while (consumed <= kMaxHeaderBytes) {
        line.clear();
        for (;;) {
            std::string byte;
            if (!recv_exact(socket, 1, byte, nullptr, deadline)) return false;
            line.push_back(byte[0]);
            ++consumed;
            if (consumed > kMaxHeaderBytes) return false;
            if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n') {
                line.resize(line.size() - 2);
                break;
            }
        }
        if (line.empty()) return true;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return false;
        const std::string_view name(line.data(), colon);
        std::size_t value_start = colon + 1;
        while (value_start < line.size() && (line[value_start] == ' ' || line[value_start] == '\t')) ++value_start;
        std::size_t value_end = line.size();
        while (value_end > value_start && (line[value_end - 1] == ' ' || line[value_end - 1] == '\t')) --value_end;
        const std::string value = line.substr(value_start, value_end - value_start);
        if (ascii_iequals(name, "host")) {
            if (headers.host_seen) headers.host_duplicate = true;
            headers.host_seen = true;
            headers.host = value;
        } else if (ascii_iequals(name, "origin")) {
            if (headers.origin_seen) headers.origin_duplicate = true;
            headers.origin_seen = true;
            headers.origin = value;
        }
    }
    return false;
}

bool valid_loopback_host(std::string_view host, std::uint16_t port) {
    const std::string suffix = ":" + std::to_string(port);
    return host == "127.0.0.1" + suffix || host == "localhost" + suffix;
}

bool valid_origin_value(std::string_view origin) {
    const bool https = origin.starts_with("https://");
    const bool http = origin.starts_with("http://");
    const std::size_t scheme = https ? 8u : (http ? 7u : 0u);
    if (scheme == 0 || origin.size() == scheme) return false;
    for (std::size_t i = scheme; i < origin.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(origin[i]);
        if (c <= 0x20 || c == 0x7f || c == '/' || c == '?' || c == '#' ||
            c == '@' || c == '*' || c == '\\' || c == ',') return false;
    }
    return true;
}

bool origin_is_allowed(std::string_view origin, const std::vector<std::string>& allowed_origins) {
    if (!valid_origin_value(origin)) return false;
    return std::find(allowed_origins.begin(), allowed_origins.end(), origin) != allowed_origins.end();
}

bool drain_oversized_request_line(const Socket& socket, std::string_view prefix, Deadline deadline) {
    char previous = prefix.size() >= 2 ? prefix[prefix.size() - 2] : '\0';
    char last = prefix.empty() ? '\0' : prefix.back();
    while (!(previous == '\r' && last == '\n')) {
        std::string byte;
        if (!recv_exact(socket, 1, byte, nullptr, deadline)) return false;
        previous = last;
        last = byte[0];
    }
    RequestHeaders ignored;
    return read_request_headers(socket, deadline, ignored);
}

void send_response(const Socket& socket, const Response& response, std::string_view cors_origin = {},
                   bool options = false) {
    std::string wire = "HTTP/1.1 " + std::to_string(response.status) + " " + response.reason + "\r\n";
    if (!response.body.empty()) wire += "Content-Type: application/json\r\n";
    wire += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    wire += "Connection: close\r\n";
    if (!cors_origin.empty()) {
        wire += "Access-Control-Allow-Origin: " + std::string(cors_origin) + "\r\n";
        wire += "Vary: Origin\r\n";
    }
    if (options) {
        wire += "Allow: GET\r\n";
        wire += "Access-Control-Allow-Methods: GET\r\n";
    }
    wire += "\r\n";
    wire += response.body;
    (void)send_all(socket, wire, nullptr);
}

void serve_one(const Chain& chain, const Socket& socket, std::uint16_t port,
                const std::vector<std::string>& allowed_origins) {
    const Deadline deadline = std::chrono::steady_clock::now() + kRequestTimeout;
    std::string line;
    if (!read_request_line(socket, line, deadline)) {
        // An overlong request line is a client error; a silent or slow peer is simply closed at its deadline.
        if (line.size() > kMaxRequestLine && drain_oversized_request_line(socket, line, deadline))
            send_response(socket, error_response(400, "Bad Request", "request line exceeds 8 KiB"));
        return;
    }

    RequestHeaders headers;
    if (!read_request_headers(socket, deadline, headers)) {
        send_response(socket, error_response(400, "Bad Request", "request headers are malformed, incomplete or exceed 8 KiB"));
        return;
    }
    if (!headers.host_seen || headers.host_duplicate || !valid_loopback_host(headers.host, port)) {
        send_response(socket, error_response(400, "Bad Request", "Host must be 127.0.0.1 or localhost with this API port"));
        return;
    }
    if (headers.origin_duplicate) {
        send_response(socket, error_response(400, "Bad Request", "multiple Origin headers are not allowed"));
        return;
    }
    const std::string_view cors_origin = origin_is_allowed(headers.origin, allowed_origins)
        ? std::string_view(headers.origin) : std::string_view{};

    const std::size_t first_space = line.find(' ');
    const std::size_t second_space = first_space == std::string::npos
        ? std::string::npos : line.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos ||
        line.find(' ', second_space + 1) != std::string::npos ||
        (line.substr(second_space + 1) != "HTTP/1.0" && line.substr(second_space + 1) != "HTTP/1.1")) {
        send_response(socket, error_response(400, "Bad Request", "malformed HTTP request line"));
        return;
    }

    const std::string_view method(line.data(), first_space);
    const std::string_view path(line.data() + first_space + 1, second_space - first_space - 1);
    if (path.empty() || path.front() != '/' || path.find_first_of("?#") != std::string_view::npos) {
        send_response(socket, error_response(400, "Bad Request", "request path is invalid"), cors_origin);
        return;
    }
    if (method == "OPTIONS") {
        send_response(socket, Response{204, "No Content", {}}, cors_origin, true);
        return;
    }
    if (method != "GET") {
        send_response(socket, error_response(405, "Method Not Allowed", "Only GET is allowed; this API is read-only"), cors_origin);
        return;
    }
    send_response(socket, route_get(chain, path), cors_origin);
}

}  // namespace

struct ApiServer::Impl {
    const Chain* chain{nullptr};
    std::vector<std::string> allowed_origins;
    Listener listener;
    std::atomic<bool> stopping{false};
    std::atomic<unsigned> active{0};
    std::thread accept_thread;
    mutable std::mutex mutex;
    std::condition_variable idle;
    bool started{false};
    bool network_started{false};

    void accept_loop() {
        while (!stopping.load(std::memory_order_acquire)) {
            Socket peer;
            std::string error;
            if (!accept_one(listener.socket, peer, &error,
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(100))) {
                continue;
            }
            const unsigned previous = active.fetch_add(1, std::memory_order_acq_rel);
            if (previous >= kMaxConnections) {
                active.fetch_sub(1, std::memory_order_acq_rel);
                peer.close();
                continue;
            }
            try {
                std::thread([this, peer = std::move(peer)]() mutable {
                    try {
                        serve_one(*chain, peer, listener.port, allowed_origins);
                    } catch (...) {
                        // A malformed request or allocation failure must not terminate the node process.
                    }
                    peer.close();
                    {
                        std::lock_guard lock(mutex);
                        active.fetch_sub(1, std::memory_order_acq_rel);
                    }
                    idle.notify_all();
                }).detach();
            } catch (...) {
                peer.close();
                active.fetch_sub(1, std::memory_order_acq_rel);
                idle.notify_all();
            }
        }
    }
};

ApiServer::ApiServer() : impl_(std::make_unique<Impl>()) {}
ApiServer::~ApiServer() { stop(); }
ApiServer::ApiServer(ApiServer&& other) noexcept : impl_(std::move(other.impl_)) {}
ApiServer& ApiServer::operator=(ApiServer&& other) noexcept {
    if (this != &other) {
        stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

bool ApiServer::start(const Chain& chain, std::uint16_t port, std::string* error) {
    return start(chain, port, std::vector<std::string>{}, error);
}

bool ApiServer::start(const Chain& chain, std::uint16_t port,
                      const std::vector<std::string>& allowed_origins, std::string* error) {
    if (!impl_ || impl_->started) {
        if (error) *error = "API server is already started";
        return false;
    }
    for (const std::string& origin : allowed_origins) {
        if (!valid_origin_value(origin)) {
            if (error) *error = "CORS origins must be exact http:// or https:// origins; '*' is forbidden";
            return false;
        }
    }
    if (!net_startup(error)) return false;
    impl_->network_started = true;
    if (!listen_loopback(port, impl_->listener, error, 16)) {
        net_cleanup();
        impl_->network_started = false;
        return false;
    }
    impl_->chain = &chain;
    impl_->allowed_origins = allowed_origins;
    impl_->stopping.store(false, std::memory_order_release);
    impl_->started = true;
    try {
        impl_->accept_thread = std::thread([state = impl_.get()] { state->accept_loop(); });
    } catch (...) {
        impl_->listener.socket.close();
        impl_->started = false;
        net_cleanup();
        impl_->network_started = false;
        if (error) *error = "could not start the API accept thread";
        return false;
    }
    return true;
}

void ApiServer::wait() {
    if (impl_ && impl_->accept_thread.joinable()) impl_->accept_thread.join();
}

void ApiServer::stop() {
    if (!impl_ || !impl_->started) return;
    impl_->stopping.store(true, std::memory_order_release);
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    {
        std::unique_lock lock(impl_->mutex);
        impl_->idle.wait(lock, [this] { return impl_->active.load(std::memory_order_acquire) == 0; });
    }
    impl_->listener.socket.close();
    impl_->started = false;
    if (impl_->network_started) {
        net_cleanup();
        impl_->network_started = false;
    }
}

bool ApiServer::running() const noexcept {
    return impl_ && impl_->started && !impl_->stopping.load(std::memory_order_acquire);
}

std::uint16_t ApiServer::port() const noexcept {
    return impl_ ? impl_->listener.port : 0;
}

}  // namespace taz
