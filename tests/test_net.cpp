// v9.0: the loopback transport.
//
// Single-threaded on purpose: listen -> connect -> accept works because the kernel completes the handshake and
// queues the connection before accept() is called. No threads, no sleeps, nothing that can flake in CI.
//
// What this proves: two sockets on 127.0.0.1 exchange the hello, a peer on a different chain is refused, a peer
// announcing a huge message is refused before anything is allocated, garbage is refused, and a closed port is an
// error rather than a crash.
#include "taz/auth.hpp"
#include "taz/net.hpp"
#include <chrono>
#include <atomic>
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <cstdlib>
#include <thread>
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
    CHECK(check_peer(client_hello, got_client_side).ok);   // the client judges what it received (v9.2: checking the server's own hello against itself is a reflection, and is refused)
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

    // announce 64 MiB (twice the 32 MiB limit of v9.1; the limit was 64 KiB before, when this said 10 MB) and send
    // nothing else: the receiver must refuse without waiting or allocating
    std::string header;
    header.push_back(static_cast<char>(0x04));
    header.push_back(static_cast<char>(0));
    header.push_back(static_cast<char>(0));
    header.push_back(static_cast<char>(0));
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

// v9.2: a peer that sends a VALID message and then dribbles its payload must still be cut off at the absolute
// deadline. The valid header matters: with garbage the parser rejects the message as soon as the type byte
// arrives, which is a different behaviour and proves nothing about the deadline.
static void n06_slow_peer_is_cut_at_the_deadline() {
    Listener listener;
    Socket server, client;
    std::string err;
    CHECK(listen_loopback(0, listener, &err));
    CHECK(connect_loopback(listener.port, client, &err));

    // A real frame: length 100 (big-endian), type 0x02, and then the payload one byte at a time.
    const std::string header = std::string(3, '\0') + std::string(1, '\x64');
    CHECK(send_all(client, header, &err));
    CHECK(send_all(client, std::string(1, '\x02'), &err));

    // One byte a second is comfortably faster than the socket's three-second timeout, so every read succeeds and
    // the only thing that can stop this is the deadline itself.
    std::thread dribbler([&] {
        for (int i = 0; i < 100; ++i) {
            if (!send_all(client, std::string(1, 'x'), nullptr)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    });

    CHECK(accept_one(listener.socket, server, &err));
    std::uint8_t type = 0;
    std::string payload;
    const auto started = std::chrono::steady_clock::now();
    const bool got = recv_message(server, type, payload, &err, started + std::chrono::milliseconds(10000));
    const auto took =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    CHECK(!got);                                 // it must not be allowed to finish
    // Milliseconds, not seconds: rounding to seconds hides exactly the difference this test is looking for.
    CHECK(took.count() >= 9000);                 // without a deadline this would run the full hundred seconds
    CHECK(took.count() <= 25000);                // nor hang past the deadline by much
    std::cout << "  a dribbling peer was cut off after " << took.count() << " ms\n";
    dribbler.join();
}

// v9.3: the authenticated handshake proves control of both peer keys and binds the proof to fresh hellos.
static void n08_authenticated_handshake_ok() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) { std::cerr << "  " << err << "\n"; return; }

    const Chain chain = make_chain(1000);
    PrivKey server_key{}, client_key{}; PubKey server_pub{}, client_pub{};
    CHECK(generate_keypair(server_key, server_pub));
    CHECK(generate_keypair(client_key, client_pub));

    Hello server_hello = hello_for(chain), client_hello = hello_for(chain);
    Hello server_got, client_got;
    std::string server_err, client_err;
    bool server_ok = false, client_ok = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::thread server_thread([&] {
        server_ok = exchange_authenticated_hello(server, server_hello, server_key, server_got,
                                                 false, &server_err, deadline);
    });
    client_ok = exchange_authenticated_hello(client, client_hello, client_key, client_got,
                                             true, &client_err, deadline);
    server_thread.join();

    CHECK(server_ok);
    CHECK(client_ok);
    if (!server_err.empty()) std::cerr << "  server auth: " << server_err << "\n";
    if (!client_err.empty()) std::cerr << "  client auth: " << client_err << "\n";
    CHECK(hello_signature_ok(server_hello));
    CHECK(hello_signature_ok(client_hello));
    CHECK(server_got.pubkey == client_pub);
    CHECK(client_got.pubkey == server_pub);
    CHECK(check_peer(server_hello, server_got).ok);
    CHECK(check_peer(client_hello, client_got).ok);
}

// A caller may accidentally reuse a previously populated Hello. The authenticated transport
// must replace that value with a fresh nonce before signing, rather than trusting non-zero bytes.
static void n11_authenticated_handshake_refreshes_reused_nonce() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) return;

    const Chain chain = make_chain(1000);
    PrivKey server_key{}, client_key{}; PubKey server_pub{}, client_pub{};
    CHECK(generate_keypair(server_key, server_pub));
    CHECK(generate_keypair(client_key, client_pub));

    Hello server_hello = hello_for(chain), client_hello = hello_for(chain);
    Hash32 stale_nonce{};
    stale_nonce.fill(0xA5);
    server_hello.nonce = stale_nonce;
    client_hello.nonce = stale_nonce;

    Hello server_got, client_got;
    std::string server_err, client_err;
    bool server_ok = false, client_ok = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::thread server_thread([&] {
        server_ok = exchange_authenticated_hello(server, server_hello, server_key, server_got,
                                                 false, &server_err, deadline);
    });
    client_ok = exchange_authenticated_hello(client, client_hello, client_key, client_got,
                                             true, &client_err, deadline);
    server_thread.join();

    CHECK(server_ok);
    CHECK(client_ok);
    CHECK(server_hello.nonce != stale_nonce);
    CHECK(client_hello.nonce != stale_nonce);
    CHECK(server_hello.nonce != client_hello.nonce);
}

// An unsigned hello may identify a chain, but it must never pass the authenticated handshake.
static void n09_unsigned_peer_refused() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) return;

    const Chain chain = make_chain(1000);
    PrivKey local_key{}; PubKey local_pub{};
    CHECK(generate_keypair(local_key, local_pub));
    Hello local = hello_for(chain);
    Hello anonymous = hello_for(chain);
    CHECK(send_message(client, kMsgHello, encode_hello(anonymous), &err));

    Hello got;
    CHECK(!exchange_authenticated_hello(server, local, local_key, got, false, &err));
    CHECK(err.find("peer did not prove an identity") != std::string::npos);
}

// A peer that advertises a key but sends a broken hello signature must not pass strict authentication.
static void n12_invalid_peer_hello_signature_refused() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) return;

    const Chain chain = make_chain(1000);
    PrivKey local_key{}, peer_key{}; PubKey local_pub{}, peer_pub{};
    CHECK(generate_keypair(local_key, local_pub));
    CHECK(generate_keypair(peer_key, peer_pub));

    Hello local = hello_for(chain), peer = hello_for(chain), got;
    CHECK(sign_hello(peer, peer_key));
    peer.signature[0] ^= 0x01;  // corrupt the signed hello after signing
    CHECK(!hello_signature_ok(peer));
    CHECK(send_message(client, kMsgHello, encode_hello(peer), &err));

    CHECK(!exchange_authenticated_hello(server, local, local_key, got, false, &err));
    CHECK(err.find("identity signature is invalid") != std::string::npos);
}

// The proof key must be the same key that signed the peer's hello.
static void n13_auth_proof_key_must_match_hello_key() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) return;

    const Chain chain = make_chain(1000);
    PrivKey local_key{}, hello_key{}, proof_key{};
    PubKey local_pub{}, hello_pub{}, proof_pub{};
    CHECK(generate_keypair(local_key, local_pub));
    CHECK(generate_keypair(hello_key, hello_pub));
    CHECK(generate_keypair(proof_key, proof_pub));
    CHECK(local_pub != hello_pub);
    CHECK(hello_pub != proof_pub);

    Hello local = hello_for(chain), peer = hello_for(chain), server_got;
    CHECK(sign_hello(peer, hello_key));
    const std::string peer_raw = encode_hello(peer);
    bool server_ok = true;
    std::string server_err;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::thread server_thread([&] {
        server_ok = exchange_authenticated_hello(server, local, local_key, server_got,
                                                 false, &server_err, deadline);
    });

    CHECK(send_message(client, kMsgHello, peer_raw, &err, deadline));
    std::uint8_t type = 0;
    std::string server_raw;
    CHECK(recv_message(client, type, server_raw, &err, deadline));
    CHECK(type == kMsgHello);
    Hello server_hello;
    CHECK(decode_hello(server_raw, server_hello, &err));
    const std::string transcript = auth_transcript(peer, peer_raw, server_hello, server_raw);
    AuthMessage wrong_key_proof;
    CHECK(sign_auth(wrong_key_proof, proof_key, transcript));
    CHECK(send_message(client, kMsgAuth, encode_auth(wrong_key_proof), &err, deadline));

    server_thread.join();
    CHECK(!server_ok);
    CHECK(server_err.find("authentication key does not match") != std::string::npos);
}

// Omitting the deadline from the public authenticated-handshake API must still impose an absolute limit.
// The peer sends a valid frame header, then trickles an incomplete auth body faster than the 3-second
// per-call socket timeout. Without the API's finite default deadline this can hold the handshake indefinitely.
static void n14_authenticated_handshake_has_default_deadline() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) return;

    const Chain chain = make_chain(1000);
    PrivKey server_key{}, peer_key{}; PubKey server_pub{}, peer_pub{};
    CHECK(generate_keypair(server_key, server_pub));
    CHECK(generate_keypair(peer_key, peer_pub));

    Hello local = hello_for(chain), peer = hello_for(chain), got;
    CHECK(sign_hello(peer, peer_key));
    CHECK(send_message(client, kMsgHello, encode_hello(peer), &err));

    std::string server_err;
    bool server_ok = true;
    std::atomic<bool> finished{false};
    const auto started = std::chrono::steady_clock::now();
    std::thread server_thread([&] {
        // Deliberately omit the deadline argument: the API default must be safe.
        server_ok = exchange_authenticated_hello(server, local, server_key, got, false, &server_err);
        finished.store(true);
    });

    std::thread dribbler([&] {
        // 4-byte length (200), auth message type, then 200 bytes. One byte every 100 ms
        // keeps each individual recv alive but takes longer than the 10-second absolute deadline.
        std::string prefix;
        prefix.push_back('\0'); prefix.push_back('\0'); prefix.push_back('\0');
        prefix.push_back(static_cast<char>(200)); prefix.push_back(static_cast<char>(kMsgAuth));
        for (char byte : prefix) {
            if (finished.load()) return;
            if (!send_all(client, std::string(1, byte), nullptr)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        for (int i = 0; i < 200 && !finished.load(); ++i) {
            if (!send_all(client, std::string(1, 'x'), nullptr)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    server_thread.join();
    finished.store(true);
    dribbler.join();
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    CHECK(!server_ok);
    CHECK(took.count() >= 8000);
    CHECK(took.count() < 14000);
    std::cout << "  default authenticated-handshake deadline stopped trickle after "
              << took.count() << " ms\n";
}

// A zero-filled seed is not an identity and must be rejected before any hello is sent.
static void n10_zero_local_identity_refused() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) return;

    const Chain chain = make_chain(1000);
    Hello local = hello_for(chain), got;
    PrivKey empty_key{};
    CHECK(!exchange_authenticated_hello(server, local, empty_key, got, false, &err));
    CHECK(err.find("identity key is empty") != std::string::npos);
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

// A peer that never reads must not extend a large send by allowing repeated partial writes to make progress.
static void n07_send_deadline_cuts_off_a_slow_receiver() {
    Listener listener; Socket server, client; std::string err;
    CHECK(connected_pair(listener, server, client, err));
    if (!server.valid() || !client.valid()) return;

    // The receiver drains chunks slowly. Each successful partial write must NOT restart the 500 ms
    // total deadline; a simple "no reader" test would only exercise the per-call socket timeout.
    std::thread slow_reader([&] {
        std::string chunk;
        for (;;) {
            if (!recv_exact(client, 512u * 1024u, chunk, nullptr)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    const std::string payload(kMaxPayloadBytes, 'x'); // maximum legal frame payload; far above socket buffers
    const auto started = std::chrono::steady_clock::now();
    const bool sent = send_message(server, kMsgBlocks, payload, &err,
                                   started + std::chrono::milliseconds(500));
    const auto took =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    server.close(); // unblock the reader before joining it

    CHECK(!sent);
    CHECK(!err.empty());
    CHECK(took.count() >= 300);
    CHECK(took.count() < 2500);
    slow_reader.join();
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
    n06_slow_peer_is_cut_at_the_deadline();
    n06_send_to_closed_peer_is_an_error();
    n07_send_deadline_cuts_off_a_slow_receiver();
    n07_deadline_cuts_off_a_slow_peer();
    n08_authenticated_handshake_ok();
    n11_authenticated_handshake_refreshes_reused_nonce();
    n09_unsigned_peer_refused();
    n12_invalid_peer_hello_signature_refused();
    n13_auth_proof_key_must_match_hello_key();
    n14_authenticated_handshake_has_default_deadline();
    n10_zero_local_identity_refused();
    net_cleanup();
    if (failures == 0) { std::cout << "net tests: all passed\n"; return 0; }
    std::cerr << "net tests: " << failures << " failure(s)\n";
    return 1;
}
