// v9.0: the handshake message and the decision that goes with it.
//
// No sockets here. This is the part that decides whether two nodes are on the same chain, and it is written so
// it can be tested directly: what a peer sends, what we refuse, and what a peer can make us allocate.
#include "taz/hello.hpp"
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

// a message survives a round trip
static void h01_round_trip() {
    Chain c = make_chain(1000);
    const Hello ours = hello_for(c);
    Hello back;
    std::string why;
    CHECK(decode_hello(encode_hello(ours), back, &why));
    CHECK(back.protocol_version == ours.protocol_version);
    CHECK(back.chain_id == ours.chain_id);
    CHECK(back.genesis_commitment == ours.genesis_commitment);
    CHECK(back.best_height == ours.best_height);
}

// the same chain is accepted, and nothing else is
static void h02_decision() {
    Chain a = make_chain(1000);
    Chain b = make_chain(2000);            // same chain ID, same protocol, different genesis
    const Hello ours = hello_for(a);
    CHECK(check_peer(ours, hello_for(a)).ok);

    const HandshakeDecision wrong_genesis = check_peer(ours, hello_for(b));
    CHECK(!wrong_genesis.ok);
    CHECK(wrong_genesis.reason.find("genesis") != std::string::npos);

    Hello other_chain = ours;
    other_chain.chain_id = "someone-elses-chain";
    CHECK(!check_peer(ours, other_chain).ok);

    Hello other_protocol = ours;
    other_protocol.protocol_version = ours.protocol_version + 1;
    const HandshakeDecision wrong_proto = check_peer(ours, other_protocol);
    CHECK(!wrong_proto.ok);
    CHECK(wrong_proto.reason.find("protocol") != std::string::npos);
}

// a message that is not ours, or is malformed, is refused rather than half-read
static void h03_malformed() {
    const std::string good = encode_hello(hello_for(make_chain(1000)));
    Hello out; std::string why;

    CHECK(!decode_hello("", out, &why));
    CHECK(!decode_hello("HTTP/1.1 200 OK\r\n\r\n", out, &why));
    CHECK(!decode_hello("TAZHELLO/2\nproto=2\nchain=x\ngenesis=" + std::string(64, 'a') + "\nheight=0\n", out, &why));

    // a missing field
    std::string missing = good;
    const std::size_t at = missing.find("height=");
    CHECK(at != std::string::npos);
    missing.erase(at, missing.find('\n', at) - at + 1);
    CHECK(!decode_hello(missing, out, &why));

    // an unknown field
    CHECK(!decode_hello(good + "extra=1\n", out, &why));

    // the same field twice
    CHECK(!decode_hello(good + "height=5\n", out, &why));

    // genesis that is not hex, and one that is the wrong length
    std::string bad_hex = good;
    const std::size_t g = bad_hex.find("genesis=");
    bad_hex.replace(g + 8, 64, std::string(64, 'z'));
    CHECK(!decode_hello(bad_hex, out, &why));

    // a height that would overflow
    std::string big = good;
    const std::size_t h = big.find("height=");
    big.replace(h + 7, big.find('\n', h) - h - 7, "99999999999999999999999999");
    CHECK(!decode_hello(big, out, &why));
}

// a peer cannot make us allocate without bound
static void h04_size_limits() {
    Hello out; std::string why;
    std::string huge = "TAZHELLO/1\n";
    while (huge.size() < kMaxHelloBytes + 64) huge += "chain=" + std::string(40, 'x') + "\n";
    CHECK(!decode_hello(huge, out, &why));

    std::string long_line = "TAZHELLO/1\nchain=" + std::string(kMaxHelloLine + 10, 'x') + "\n";
    CHECK(!decode_hello(long_line, out, &why));
}

int main() {
    h01_round_trip();
    h02_decision();
    h03_malformed();
    h04_size_limits();
    if (failures == 0) { std::cout << "hello tests: all passed\n"; return 0; }
    std::cerr << "hello tests: " << failures << " failure(s)\n";
    return 1;
}
