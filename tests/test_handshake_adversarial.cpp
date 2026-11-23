// Adversarial tests for exchange_authenticated_hello() (PR #68 hardening).
//
// The honest paths are covered in test_net.cpp (n08-n13). This file plays the attacker: a raw socket that speaks the
// wire protocol by hand and does the wrong thing at exactly one point, while the real function runs on the other
// end in a thread. Every test expects a refusal AND a reason, so a test cannot pass because something else broke.
//
//   r01  replay: a valid proof of peer P from ANOTHER connection is refused on this one
//   r02  reflection: the server's own hello echoed back is refused
//   r03  reflection: the server's own auth proof echoed back is refused
//   r04  malformed auth payloads (empty, garbage, truncated, short hex, unknown field, duplicate field, oversize)
//   r05  a message of the wrong type where the auth proof belongs
//   r06  a peer that says nothing is cut at the deadline
//   r07  a peer that finishes the hello and then goes silent is cut at the deadline
//   r08  a proof signed over the right key but the wrong transcript (tampered peer hello) is refused
#include "taz/auth.hpp"
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/net.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace taz;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

static Chain make_chain() {
    PrivKey sk{}; PubKey pk{}; PrivKey sk2{}; PubKey pk2{};
    if (!generate_keypair(sk, pk) || !generate_keypair(sk2, pk2)) std::exit(2);
    Chain c;
    if (!c.init_genesis(address_from_pubkey(pk), 1000, address_from_pubkey(pk2), 0)) std::exit(2);
    return c;
}

static bool connected_pair(Listener& listener, Socket& server, Socket& client, std::string& err) {
    if (!listen_loopback(0, listener, &err)) return false;
    if (!connect_loopback(listener.port, client, &err)) return false;
    return accept_one(listener.socket, server, &err);
}

static Deadline in_seconds(int s) { return std::chrono::steady_clock::now() + std::chrono::seconds(s); }

// The real function, on its own thread, as the side that reads first (it answers after the attacker speaks).
struct Victim {
    Socket sock;
    Hello ours;
    PrivKey key{};
    PubKey pub{};
    Hello got;
    bool ok = true;
    std::string err;
    std::thread th;
    std::chrono::steady_clock::duration took{};

    void start(const Chain& chain, bool send_first, Deadline deadline) {
        ours = hello_for(chain);
        if (!generate_keypair(key, pub)) std::exit(2);
        th = std::thread([this, send_first, deadline] {
            const auto t0 = std::chrono::steady_clock::now();
            ok = exchange_authenticated_hello(sock, ours, key, got, send_first, &err, deadline);
            took = std::chrono::steady_clock::now() - t0;
        });
    }
    void finish() { if (th.joinable()) th.join(); }
};

// A signed attacker hello (the attacker holds a real key; only the behaviour afterwards is wrong).
struct Attacker {
    Socket sock;
    PrivKey key{};
    PubKey pub{};
    Hello hello;
    std::string raw;          // the exact hello bytes sent
    Hello victim_hello;
    std::string victim_raw;   // the exact hello bytes received

    bool init(const Chain& chain) {
        if (!generate_keypair(key, pub)) return false;
        hello = hello_for(chain);
        return sign_hello(hello, key);
    }
    bool send_hello(std::string* err) {
        raw = encode_hello(hello);
        return send_message(sock, kMsgHello, raw, err, in_seconds(5));
    }
    bool read_hello(std::string* err) {
        std::uint8_t type = 0;
        if (!recv_message(sock, type, victim_raw, err, in_seconds(5))) return false;
        if (type != kMsgHello) { if (err) *err = "not a hello"; return false; }
        return decode_hello(victim_raw, victim_hello, err);
    }
    std::string transcript() const { return auth_transcript(hello, raw, victim_hello, victim_raw); }
    // A correct proof for THIS connection
    AuthMessage proof() const {
        AuthMessage m;
        if (!sign_auth(m, key, transcript())) std::exit(2);
        return m;
    }
};

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// Common set-up: a connected pair, the victim running, the attacker having exchanged hellos.
struct Scene {
    Listener listener;
    Socket server_side;
    Chain chain = make_chain();
    Victim victim;
    Attacker atk;
    std::string err;
    bool ready = false;

    // exchange_hello: attacker (connector) speaks first, victim answers.
    bool open(bool do_hello, Deadline deadline) {
        if (!connected_pair(listener, victim.sock, atk.sock, err)) return false;
        if (!atk.init(chain)) return false;
        victim.start(chain, false, deadline);
        if (do_hello) {
            if (!atk.send_hello(&err)) return false;
            if (!atk.read_hello(&err)) return false;
        }
        ready = true;
        return true;
    }
};

// r01: Peer P proved itself on connection 1. An attacker who recorded that proof cannot use it on connection 2,
// because the transcript (both hellos, both nonces) is different. The hello on connection 2 is signed by P's key,
// so only the transcript binding can stop this.
static void r01_replayed_proof_is_refused() {
    // Connection 1 (synthetic): P with nonce1 talking to some other node.
    Chain chain1 = make_chain();
    PrivKey p_key{}; PubKey p_pub{}; PrivKey o_key{}; PubKey o_pub{};
    CHECK(generate_keypair(p_key, p_pub));
    CHECK(generate_keypair(o_key, o_pub));
    Hello p1 = hello_for(chain1), o1 = hello_for(chain1);
    CHECK(sign_hello(p1, p_key));
    CHECK(sign_hello(o1, o_key));
    AuthMessage recorded;
    CHECK(sign_auth(recorded, p_key, auth_transcript(p1, encode_hello(p1), o1, encode_hello(o1))));
    CHECK(auth_ok(recorded, auth_transcript(p1, encode_hello(p1), o1, encode_hello(o1))));   // genuinely valid there

    // Connection 2: the real victim; the attacker presents P's hello key and replays the recorded proof.
    Scene s;
    s.chain = chain1;                       // same chain, so only the identity layer can refuse it
    CHECK(s.open(false, in_seconds(5)));
    if (!s.ready) { s.victim.finish(); return; }
    s.atk.key = p_key; s.atk.pub = p_pub;
    s.atk.hello = hello_for(s.chain);
    CHECK(sign_hello(s.atk.hello, p_key));
    CHECK(s.atk.send_hello(&s.err));
    CHECK(s.atk.read_hello(&s.err));
    CHECK(send_message(s.atk.sock, kMsgAuth, encode_auth(recorded), &s.err, in_seconds(5)));
    s.victim.finish();
    CHECK(!s.victim.ok);
    CHECK(contains(s.victim.err, "connection-bound identity proof is invalid"));
}

// r02: the attacker plays the server's own hello back to it. The nonce is the server's own, so check_peer refuses.
static void r02_reflected_hello_is_refused() {
    Scene s;
    CHECK(connected_pair(s.listener, s.victim.sock, s.atk.sock, s.err));
    if (!s.victim.sock.valid() || !s.atk.sock.valid()) return;
    s.victim.start(s.chain, true, in_seconds(5));            // the victim speaks first this time
    std::uint8_t type = 0;
    std::string victim_raw;
    CHECK(recv_message(s.atk.sock, type, victim_raw, &s.err, in_seconds(5)));
    CHECK(type == kMsgHello);
    CHECK(send_message(s.atk.sock, kMsgHello, victim_raw, &s.err, in_seconds(5)));   // echoed, byte for byte
    s.victim.finish();
    CHECK(!s.victim.ok);
    CHECK(contains(s.victim.err, "peer hello refused"));
}

// r03: a correct hello, then the server's own auth message echoed back as the "peer's" proof. Its key is the
// server's key, not the hello key the attacker advertised, so the key check refuses it.
static void r03_reflected_auth_is_refused() {
    Scene s;
    CHECK(s.open(true, in_seconds(5)));
    if (!s.ready) { s.victim.finish(); return; }
    std::uint8_t type = 0;
    std::string victim_auth;
    CHECK(recv_message(s.atk.sock, type, victim_auth, &s.err, in_seconds(5)));   // the victim's own proof
    CHECK(type == kMsgAuth);
    CHECK(send_message(s.atk.sock, kMsgAuth, victim_auth, &s.err, in_seconds(5)));
    s.victim.finish();
    CHECK(!s.victim.ok);
    CHECK(contains(s.victim.err, "authentication key does not match"));
}

// r04: payloads that are not an auth message. Each gets a fresh connection; each must be refused with the
// decoder's complaint, never accepted and never a crash.
static void r04_malformed_auth_payloads() {
    const std::string good_pub(64, 'a'), good_sig(128, 'b');
    const std::string cases[] = {
        "",                                                         // empty
        "garbage",                                                  // no newline
        "garbage\n",                                                // not key=value
        "pubkey=" + good_pub + "\n",                                // signature missing
        "signature=" + good_sig + "\n",                             // pubkey missing
        "pubkey=" + good_pub.substr(0, 62) + "\nsignature=" + good_sig + "\n",   // short pubkey
        "pubkey=" + good_pub + "\nsignature=" + good_sig.substr(0, 126) + "\n",  // short signature
        "pubkey=" + std::string(64, 'z') + "\nsignature=" + good_sig + "\n",     // not hex
        "pubkey=" + good_pub + "\nsignature=" + good_sig + "\nextra=1\n",        // unknown field
        "pubkey=" + good_pub + "\npubkey=" + good_pub + "\nsignature=" + good_sig + "\n",   // duplicate
        std::string(4096, 'a') + "\n",                              // oversize
    };
    int index = 0;
    for (const std::string& payload : cases) {
        Scene s;
        CHECK(s.open(true, in_seconds(5)));
        if (!s.ready) { s.victim.finish(); continue; }
        std::uint8_t type = 0;
        std::string ignored;
        CHECK(recv_message(s.atk.sock, type, ignored, &s.err, in_seconds(5)));   // the victim's own proof first
        CHECK(send_message(s.atk.sock, kMsgAuth, payload, &s.err, in_seconds(5)));
        s.victim.finish();
        const bool refused = !s.victim.ok && contains(s.victim.err, "invalid authentication proof");
        CHECK(refused);
        if (!refused) std::cerr << "  malformed case #" << index << ": ok=" << s.victim.ok
                                << " err=\"" << s.victim.err << "\"\n";
        ++index;
    }
}

// r05: the proof's place taken by another message type.
static void r05_wrong_message_type_instead_of_auth() {
    const std::uint8_t types[] = {kMsgHello, kMsgAccept};
    for (std::uint8_t t : types) {
        Scene s;
        CHECK(s.open(true, in_seconds(5)));
        if (!s.ready) { s.victim.finish(); continue; }
        std::uint8_t type = 0;
        std::string ignored;
        CHECK(recv_message(s.atk.sock, type, ignored, &s.err, in_seconds(5)));
        CHECK(send_message(s.atk.sock, t, t == kMsgHello ? s.atk.raw : std::string(), &s.err, in_seconds(5)));
        s.victim.finish();
        CHECK(!s.victim.ok);
        CHECK(contains(s.victim.err, "did not send an authentication proof"));
    }
}

// r06: connects and says nothing. The victim must give up at its deadline, not wait for ever.
static void r06_silent_peer_hits_the_deadline() {
    Scene s;
    CHECK(s.open(false, in_seconds(1)));
    if (!s.ready) { s.victim.finish(); return; }
    s.victim.finish();                       // the attacker keeps the socket open and sends nothing
    CHECK(!s.victim.ok);
    CHECK(!s.victim.err.empty());
    CHECK(s.victim.took < std::chrono::seconds(4));
    CHECK(s.victim.took >= std::chrono::milliseconds(500));      // it did wait; it did not fail for another reason
}

// r07: a complete hello exchange, then silence where the proof belongs.
static void r07_peer_silent_after_hello_hits_the_deadline() {
    Scene s;
    CHECK(s.open(true, in_seconds(1)));
    if (!s.ready) { s.victim.finish(); return; }
    s.victim.finish();                       // the attacker never sends its proof
    CHECK(!s.victim.ok);
    CHECK(!s.victim.err.empty());
    CHECK(s.victim.took < std::chrono::seconds(4));
    CHECK(s.victim.took >= std::chrono::milliseconds(500));
}

// r08: the attacker's proof is signed by the right key, but over a transcript built from a different hello than
// the one that travelled (it "improves" its own hello after the fact). Binding to the exact bytes catches it.
static void r08_proof_over_a_tampered_transcript_is_refused() {
    Scene s;
    CHECK(s.open(true, in_seconds(5)));
    if (!s.ready) { s.victim.finish(); return; }
    Hello altered = s.atk.hello;
    altered.best_height += 1;                              // not what was sent
    CHECK(sign_hello(altered, s.atk.key));
    AuthMessage forged;
    CHECK(sign_auth(forged, s.atk.key, auth_transcript(altered, encode_hello(altered), s.atk.victim_hello, s.atk.victim_raw)));
    std::uint8_t type = 0;
    std::string ignored;
    CHECK(recv_message(s.atk.sock, type, ignored, &s.err, in_seconds(5)));
    CHECK(send_message(s.atk.sock, kMsgAuth, encode_auth(forged), &s.err, in_seconds(5)));
    s.victim.finish();
    CHECK(!s.victim.ok);
    CHECK(contains(s.victim.err, "connection-bound identity proof is invalid"));
}

int main() {
    std::string err;
    if (!net_startup(&err)) { std::cerr << "net_startup: " << err << "\n"; return 2; }
    r01_replayed_proof_is_refused();
    r02_reflected_hello_is_refused();
    r03_reflected_auth_is_refused();
    r04_malformed_auth_payloads();
    r05_wrong_message_type_instead_of_auth();
    r06_silent_peer_hits_the_deadline();
    r07_peer_silent_after_hello_hits_the_deadline();
    r08_proof_over_a_tampered_transcript_is_refused();
    net_cleanup();
    if (failures == 0) { std::cout << "handshake adversarial tests: all passed\n"; return 0; }
    std::cerr << "handshake adversarial tests: " << failures << " failure(s)\n";
    return 1;
}
