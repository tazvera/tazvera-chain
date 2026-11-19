// v9.0: the loopback transport.
//
// Single-threaded on purpose: listen -> connect -> accept works because the kernel completes the handshake and
// queues the connection before accept() is called. No threads, no sleeps, nothing that can flake in CI.
//
// What this proves: two sockets on 127.0.0.1 exchange the hello, a peer on a different chain is refused, a peer
// announcing a huge message is refused before anything is allocated, garbage is refused, and a closed port is an
// error rather than a crash.
#include "taz/net.hpp"
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

using namespace taz;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

static Chain make_chain(Amount first_balance) {
    PrivKey sk{}; PubKey pk{}; PrivKey sk2{}; PubKey pk2{};
    if (!generate_keypair(sk, pk) || !generate_keypair(sk2, pk2)) std::exit(2);
    Chain c;
    if (!c.init_genesis(address_from_pubkey(pk), first_balance, address_from_pubkey(pk2), 0)) std::exit(2);
    return c;
}

// a connected pair of sockets on loopback
static bool connected_pair(Listener& listener, Socket& server, Socket& client, std::string& err) {
    if (!listen_loopback(0, listener, &err)) return false;
    if (!connect_loopback(listener.port, client, &err)) return false;
    return accept_one(listener.socket, server, &err);
}

static void n01_handshake_ok() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) { std::cerr << "  " << err << "\n"; return; }

    const Hello server_hello = hello_for(make_chain(1000));
    const Hello client_hello = hello_for(make_chain(1000));
    Hello got_server_side, got_client_side;
    CHECK(exchange_hello(server, server_hello, got_server_side, false, &err));
    CHECK(exchange_hello(client, client_hello, got_client_side, true, &err));
    CHECK(got_server_side.chain_id == client_hello.chain_id);
    CHECK(got_client_side.genesis_commitment == server_hello.genesis_commitment);
    CHECK(check_peer(server_hello, got_server_side).ok);   // same chain: accepted
}

static void n02_wrong_chain_refused() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) return;

    const Hello mine = hello_for(make_chain(1000));
    const Hello theirs = hello_for(make_chain(9999));       // same chain ID, different genesis
    Hello got;
    CHECK(exchange_hello(server, mine, got, false, &err));
    CHECK(exchange_hello(client, theirs, got, true, &err));
    const HandshakeDecision decision = check_peer(mine, got);
    CHECK(!decision.ok);
    CHECK(decision.reason.find("genesis") != std::string::npos);
}

static void n03_oversize_refused() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) return;

    // announce 10 MB and send nothing else: the receiver must refuse without waiting or allocating
    std::string header;
    header.push_back(static_cast<char>(0));
    header.push_back(static_cast<char>(0x98));
    header.push_back(static_cast<char>(0x96));
    header.push_back(static_cast<char>(0x80));
    CHECK(send_all(client, header, &err));
    std::uint8_t type = 0; std::string payload;
    CHECK(!recv_message(server, type, payload, &err));
    CHECK(err.find("larger") != std::string::npos);
}

static void n04_garbage_refused() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) return;

    CHECK(send_message(client, kMsgHello, "this is not a hello message\n", &err));
    Hello got;
    CHECK(!exchange_hello(server, hello_for(make_chain(1000)), got, false, &err));
    CHECK(!err.empty());
}

static void n05_closed_port_is_an_error() {
    Listener listener; std::string err;
    CHECK(listen_loopback(0, listener, &err));
    const std::uint16_t free_port = listener.port;
    listener.socket.close();                               // nothing is listening there now
    Socket client;
    CHECK(!connect_loopback(free_port, client, &err));
    CHECK(!err.empty());
    CHECK(!client.valid());
}

int main() {
    std::string err;
    if (!net_startup(&err)) { std::cerr << "net_startup failed: " << err << "\n"; return 1; }
    n01_handshake_ok();
    n02_wrong_chain_refused();
    n03_oversize_refused();
    n04_garbage_refused();
    n05_closed_port_is_an_error();
    net_cleanup();
    if (failures == 0) { std::cout << "net tests: all passed\n"; return 0; }
    std::cerr << "net tests: " << failures << " failure(s)\n";
    return 1;
}
