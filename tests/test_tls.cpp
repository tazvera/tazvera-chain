// v9.3: TLS 1.3 foundation. These tests claim something narrow and checkable, nothing more:
//   - both sides make their own certificate in memory, and nothing is written to disk;
//   - two nodes hand-shake and exchange bytes over TLS 1.3;
//   - a plaintext client cannot get in;
//   - a handshake that stalls is cut off by the deadline instead of hanging the node;
//   - each side can read the other's certificate fingerprint, which is what the allowlist will pin.
#include "taz/net.hpp"
#include "taz/tls.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace taz;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

static bool all_zero(const Hash32& h) {
    for (std::uint8_t b : h) if (b != 0) return false;
    return true;
}

static void t01_both_sides_make_a_certificate() {
    std::string err;
    auto server = TlsContext::make_server(&err);
    CHECK(server != nullptr);
    if (!server) { std::cerr << "  make_server: " << err << "\n"; return; }
    CHECK(!all_zero(server->certificate_fingerprint()));

    auto client = TlsContext::make_client(&err);
    CHECK(client != nullptr);
    if (!client) { std::cerr << "  make_client: " << err << "\n"; return; }
    // The client owns a certificate too, so the server has something to pin.
    CHECK(!all_zero(client->certificate_fingerprint()));
    CHECK(client->certificate_fingerprint() != server->certificate_fingerprint());

    // A second server gets a different certificate: nothing is shared, nothing is cached on disk.
    auto other = TlsContext::make_server(&err);
    CHECK(other != nullptr);
    if (other) CHECK(other->certificate_fingerprint() != server->certificate_fingerprint());
}

static void t02_two_nodes_exchange_bytes_and_fingerprints() {
    std::string err;
    Listener listener;
    CHECK(listen_loopback(0, listener, &err));
    auto server_ctx = TlsContext::make_server(&err);
    auto client_ctx = TlsContext::make_client(&err);
    CHECK(server_ctx != nullptr);
    CHECK(client_ctx != nullptr);
    if (!server_ctx || !client_ctx) return;

    const Deadline deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    Hash32 server_saw_client{};
    std::string server_seen;
    std::string server_err;          // the server thread writes only here
    bool server_ok = false;
    std::thread server([&] {
        Socket raw;
        if (!accept_one(listener.socket, raw, &server_err, deadline)) return;
        auto conn = server_ctx->accept(raw, &server_err, deadline);
        if (!conn) { std::cerr << "  server accept: " << server_err << "\n"; return; }
        server_saw_client = conn->peer_certificate_fingerprint();
        if (!conn->recv_exact(5, server_seen, &server_err, deadline)) return;
        server_ok = conn->send_all("world", &server_err, deadline);
    });

    Hash32 client_saw_server{};
    Socket raw;
    if (!connect_loopback(listener.port, raw, &err)) {
        std::cerr << "  connect: " << err << "\n";
    } else {
        auto conn = client_ctx->connect(raw, &err, deadline);
        CHECK(conn != nullptr);
        if (!conn) {
            std::cerr << "  client connect: " << err << "\n";
        } else {
            client_saw_server = conn->peer_certificate_fingerprint();
            CHECK(conn->send_all("hello", &err, deadline));
            std::string reply;
            CHECK(conn->recv_exact(5, reply, &err, deadline));
            CHECK(reply == "world");
        }
    }
    server.join();

    CHECK(server_ok);
    CHECK(server_seen == "hello");
    // Each side sees exactly the other's certificate. This is the value the allowlist will pin.
    CHECK(client_saw_server == server_ctx->certificate_fingerprint());
    CHECK(server_saw_client == client_ctx->certificate_fingerprint());
}

static void t03_plaintext_client_is_refused() {
    std::string err;
    Listener listener;
    CHECK(listen_loopback(0, listener, &err));
    auto server_ctx = TlsContext::make_server(&err);
    CHECK(server_ctx != nullptr);
    if (!server_ctx) return;

    bool refused = false;
    std::string server_err;          // the server thread writes only here
    const Deadline deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::thread server([&] {
        Socket raw;
        if (!accept_one(listener.socket, raw, &server_err, deadline)) return;
        auto conn = server_ctx->accept(raw, &server_err, deadline);
        // A client that sends plain bytes must not end up with a working encrypted session.
        refused = (conn == nullptr);
    });

    Socket raw;
    if (connect_loopback(listener.port, raw, &err)) {
        send_all(raw, "GET / HTTP/1.0\r\n\r\n", &err);   // not a TLS record
        raw.close();                                      // go away, so the server is not left waiting
    }
    server.join();
    CHECK(refused);
}

static void t04_a_stalled_handshake_is_cut_off() {
    std::string err;
    Listener listener;
    CHECK(listen_loopback(0, listener, &err));
    auto server_ctx = TlsContext::make_server(&err);
    CHECK(server_ctx != nullptr);
    if (!server_ctx) return;

    bool refused = false;
    std::string why;
    std::string server_err;          // the server thread writes only here
    std::thread server([&] {
        Socket raw;
        const Deadline accept_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        if (!accept_one(listener.socket, raw, &server_err, accept_deadline)) return;
        // This peer connects and then says nothing. One second is all it gets.
        const Deadline deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        auto conn = server_ctx->accept(raw, &server_err, deadline);
        refused = (conn == nullptr);
        why = server_err;
    });

    const auto started = std::chrono::steady_clock::now();
    Socket raw;
    if (connect_loopback(listener.port, raw, &err)) {
        std::this_thread::sleep_for(std::chrono::seconds(4));   // connect, then stay silent
        raw.close();
    }
    server.join();
    const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started).count();

    CHECK(refused);
    CHECK(why.find("deadline") != std::string::npos);
    // The point of the deadline: the node gives up on its own, without waiting for the peer to leave.
    CHECK(waited < 4);
}

int main() {
    t01_both_sides_make_a_certificate();
    t02_two_nodes_exchange_bytes_and_fingerprints();
    t03_plaintext_client_is_refused();
    t04_a_stalled_handshake_is_cut_off();
    if (failures == 0) { std::cout << "tls tests: all passed\n"; return 0; }
    std::cerr << "tls tests: " << failures << " failure(s)\n";
    return 1;
}
