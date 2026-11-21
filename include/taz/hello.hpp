#pragma once
#include "taz/types.hpp"
#include <string>
#include <string_view>

namespace taz {

// v9.0: the first thing two nodes say to each other, and the only part of a network that decides whether a peer
// is even on the same chain.
//
// There is no socket here and no threading: this is the message and the decision, written so it can be tested
// without a network. A transport that carries it comes next, and it will have nothing to decide — the decision
// lives here, in one place, and it is the same decision for every transport.
//
// The rule is simple and it fails closed: a peer is accepted only if it names the same chain ID, speaks the same
// protocol version and reports the same genesis commitment. Anything else is a different chain, however
// consistent its own blocks look, and the connection is not used.
struct Hello {
    std::uint32_t protocol_version{PROTOCOL_VERSION};
    std::string chain_id{CHAIN_ID};
    Hash32 genesis_commitment{};
    std::uint64_t best_height{0};   // the peer's claim about itself; nothing is trusted until blocks arrive
    Hash32 state_root{};            // ditto: a hint, so two nodes that already agree do not exchange a chain
    Hash32 tip_hash{};              // the hash of the newest block: this is what covers the history
    // v9.0: optional proof of who is speaking. All zero means anonymous, which is allowed and said out loud —
    // a signature says "this key spoke", never "this peer can be trusted".
    // v9.1: a fresh 32-byte nonce per connection. It is what makes an identity proof belong to THIS connection:
    // without it a recorded handshake could be replayed by anyone.
    Hash32 nonce{};
    PubKey pubkey{};
    Signature signature{};
};

// Canonical, line based, strict:
//
//   TAZHELLO/1
//   proto=2
//   chain=tazvera-local-prototype-v1
//   genesis=<64 hex>
//   height=<decimal>
//   root=<64 hex>
//   tip=<64 hex>
//   nonce=<64 hex>
//   pubkey=<64 hex>      (optional, only with signature)
//   signature=<128 hex>  (optional, only with pubkey)
//
// decode_hello() rejects: a wrong header, an unknown or duplicated key, a missing key, a value that is not the
// expected shape, more than kMaxHelloBytes, or any line longer than kMaxHelloLine. A peer cannot make the node
// allocate an unbounded amount of memory by sending a big number.
inline constexpr std::size_t kMaxHelloBytes = 768;   // grew when the identity fields were added
inline constexpr std::size_t kMaxHelloLine = 160;   // a signature line is 10 + 128 characters

std::string encode_hello(const Hello& hello);
bool decode_hello(std::string_view text, Hello& out, std::string* why = nullptr);

struct HandshakeDecision {
    bool ok{false};
    std::string reason;   // empty when ok
};

// Compares what we are with what the peer says it is. `ours` is built from the local chain, never from anything
// the peer sent.
HandshakeDecision check_peer(const Hello& ours, const Hello& theirs);

// Convenience: our own Hello for a chain we already loaded.
Hello hello_for(const class Chain& chain);

// The bytes a node signs: the hello as text, carrying its public key and without the signature. The encoding is
// fixed order and fixed spelling, so the verifier rebuilds exactly what the signer signed.
std::string hello_signable_bytes(const Hello& hello);
bool sign_hello(Hello& hello, const PrivKey& private_seed);
bool hello_has_identity(const Hello& hello);
// True only when a signature is present and verifies. An absent signature is not a pass.
bool hello_signature_ok(const Hello& hello);
Address hello_address(const Hello& hello);
// A hello whose nonce is all zero came from a failed random read and must not be used on a connection.
bool hello_has_nonce(const Hello& hello);

}  // namespace taz
