// v9.0: the loopback transport.
//
// Single-threaded on purpose: listen -> connect -> accept works because the kernel completes the handshake and
// queues the connection before accept() is called. No threads, no sleeps, nothing that can flake in CI.
//
// What this proves: two sockets on 127.0.0.1 exchange the hello, a peer on a different chain is refused, a peer
// announcing a huge message is refused before anything is allocated, garbage is refused, and a closed port is an
// error rather than a crash.
#include "taz/net.hpp"
#include <chrono>
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

// A single-threaded exchange, spelled out in the order it must happen: the sender sends first, the receiver
// reads, then the reply comes back. exchange_hello() is written for two live processes, where one side blocks
// in recv while the other sends; calling it on both ends of one process would simply time out.
static bool pump(Socket& first, const Hello& first_hello, Hello& first_got,
                 Socket& second, const Hello& second_hello, Hello& second_got, std::string& err) {
    if (!send_message(first, kMsgHello, encode_hello(first_hello), &err)) return false;
    std::uint8_t type = 0;
    std::string payload;
    if (!recv_message(second, type, payload, &err)) return false;
    if (type != kMsgHello) { err = "the peer did not send a hello"; return false; }
    if (!decode_hello(payload, second_got, &err)) return false;

    if (!send_message(second, kMsgHello, encode_hello(second_hello), &err)) return false;
    if (!recv_message(first, type, payload, &err)) return false;
    if (type != kMsgHello) { err = "the peer did not send a hello"; return false; }
    return decode_hello(payload, first_got, &err);
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

    const Chain chain = make_chain(1000);          // one chain, so both sides really are on the same one
    const Hello server_hello = hello_for(chain);
    const Hello client_hello = hello_for(chain);
    Hello got_server_side, got_client_side;
    CHECK(pump(client, client_hello, got_client_side, server, server_hello, got_server_side, err));
    if (!err.empty()) std::cerr << "  " << err << "\n";
    CHECK(got_server_side.chain_id == client_hello.chain_id);
    CHECK(got_client_side.genesis_commitment == server_hello.genesis_commitment);
    CHECK(check_peer(server_hello, got_server_side).ok);   // same chain: accepted
    CHECK(check_peer(server_hello, got_client_side).ok);
}

static void n02_wrong_chain_refused() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) return;

    const Hello mine = hello_for(make_chain(1000));
    const Hello theirs = hello_for(make_chain(9999));       // same chain ID, different genesis
    Hello got_server_side, got_client_side;
    CHECK(pump(client, theirs, got_client_side, server, mine, got_server_side, err));
    const HandshakeDecision decision = check_peer(mine, got_server_side);
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

// v9.0: a peer that hangs up must not kill this process. Without MSG_NOSIGNAL the
// second send raises SIGPIPE and the whole test binary dies (exit 141), which is why
// this test would not merely fail — it would disappear.
static void n06_send_to_closed_peer_is_an_error() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) { std::cerr << "  " << err << "\n"; return; }

    client.close();                                   // the peer goes away
    bool failed = false;
    for (int i = 0; i < 4 && !failed; ++i)            // the first send may still fit in the socket buffer
        failed = !send_message(server, kMsgHello, std::string(4096, 'x'), &err);
    CHECK(failed);
    CHECK(!err.empty());
}

// v9.0: the deadline must cut off a peer that trickles bytes, not wait three seconds after each one.
// The client announces a body and then goes quiet; the receive has to give up at the deadline, not at the
// per-call socket timeout.
static void n07_deadline_cuts_off_a_slow_peer() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) { std::cerr << "  " << err << "\n"; return; }

    const std::string header = std::string(3, '\0') + std::string(1, '\x64');   // length = 100, big-endian
    CHECK(send_all(client, header, &err));                                       // and a type byte
    CHECK(send_all(client, std::string(1, '\x02'), &err));                      // ... then silence

    const auto started = std::chrono::steady_clock::now();
    std::uint8_t type = 0;
    std::string payload;
    const bool got = recv_message(server, type, payload, &err, started + std::chrono::milliseconds(400));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    CHECK(!got);
    CHECK(!err.empty());
    CHECK(took.count() < 2000);                 // 400 ms deadline, not the 3 s socket timeout
}

int main() {
    std::string err;
    if (!net_startup(&err)) { std::cerr << "net_startup failed: " << err << "\n"; return 1; }
    n01_handshake_ok();
    n02_wrong_chain_refused();
    n03_oversize_refused();
    n04_garbage_refused();
    n05_closed_port_is_an_error();
    n06_send_to_closed_peer_is_an_error();
    n07_deadline_cuts_off_a_slow_peer();
    net_cleanup();
    if (failures == 0) { std::cout << "net tests: all passed\n"; return 0; }
    std::cerr << "net tests: " << failures << " failure(s)\n";
    return 1;
}
