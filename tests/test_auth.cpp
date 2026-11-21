// v9.1: the identity proof that belongs to one connection.
//
// What is checked here: the transcript is canonical and unambiguous, a signature verifies only on the exact
// transcript it was made for, a replayed or reflected message fails, and the nonces really are fresh 32 bytes.
#include "taz/auth.hpp"
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

using namespace taz;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() {
    Key k;
    if (!generate_keypair(k.sk, k.pk)) std::exit(2);
    k.addr = address_from_pubkey(k.pk);
    return k;
}

static Chain make_chain(Amount first_balance) {
    Key a = new_key(), b = new_key();
    Chain c;
    if (!c.init_genesis(a.addr, first_balance, b.addr, 0)) std::exit(2);
    return c;
}

// one side of a connection: its hello and the bytes it sent
struct Side { Hello hello; std::string bytes; };
static Side side_for(const Chain& chain) {
    Side s;
    s.hello = hello_for(chain);
    s.bytes = encode_hello(s.hello);
    return s;
}

static void a01_transcript_is_deterministic() {
    const Chain chain = make_chain(1000);
    const Side a = side_for(chain), b = side_for(chain);
    const std::string first = auth_transcript(a.hello, a.bytes, b.hello, b.bytes);
    const std::string again = auth_transcript(a.hello, a.bytes, b.hello, b.bytes);
    CHECK(!first.empty());
    CHECK(first == again);                       // both sides must build exactly the same bytes
    CHECK(first.size() > kAuthDomain.size());
}

static void a02_length_prefixes_remove_ambiguity() {
    // The classic concatenation trap: "ab"+"c" and "a"+"bc" are the same bytes without lengths.
    const Chain chain = make_chain(1000);
    Side a = side_for(chain), b = side_for(chain);
    const std::string t1 = auth_transcript(a.hello, "ab", b.hello, "c");
    const std::string t2 = auth_transcript(a.hello, "a", b.hello, "bc");
    CHECK(!t1.empty());
    CHECK(!t2.empty());
    CHECK(t1 != t2);
}

static void a03_signature_round_trip() {
    const Chain chain = make_chain(1000);
    const Side a = side_for(chain), b = side_for(chain);
    const std::string transcript = auth_transcript(a.hello, a.bytes, b.hello, b.bytes);
    CHECK(!transcript.empty());

    Key k = new_key();
    AuthMessage message;
    CHECK(sign_auth(message, k.sk, transcript));
    CHECK(message.pubkey == k.pk);
    CHECK(auth_ok(message, transcript));

    // survives the wire
    AuthMessage back;
    std::string why;
    CHECK(decode_auth(encode_auth(message), back, &why));
    CHECK(back.pubkey == message.pubkey);
    CHECK(auth_ok(back, transcript));
}

static void a04_any_change_breaks_it() {
    const Chain chain = make_chain(1000);
    Side a = side_for(chain), b = side_for(chain);
    const std::string transcript = auth_transcript(a.hello, a.bytes, b.hello, b.bytes);
    Key k = new_key();
    AuthMessage message;
    CHECK(sign_auth(message, k.sk, transcript));

    // one byte of one hello
    std::string changed = a.bytes;
    changed[changed.size() - 2] = (changed[changed.size() - 2] == 'a') ? 'b' : 'a';
    CHECK(!auth_ok(message, auth_transcript(a.hello, changed, b.hello, b.bytes)));

    // note: the transcript covers the hello BYTES, not the object, so a changed height only matters through
    // those bytes — which the check above already exercises.

    // a different chain commitment
    Hello other_chain = a.hello;
    other_chain.genesis_commitment[0] ^= 0xff;
    CHECK(!auth_ok(message, auth_transcript(other_chain, a.bytes, b.hello, b.bytes)));
}

static void a05_replay_and_reflection() {
    const Chain chain = make_chain(1000);
    Side a = side_for(chain), b = side_for(chain);
    Key k = new_key();

    AuthMessage message;
    CHECK(sign_auth(message, k.sk, auth_transcript(a.hello, a.bytes, b.hello, b.bytes)));

    // replay: same hellos, new nonce on our side -> a different transcript, so the old signature is useless
    Side fresh = side_for(chain);
    CHECK(!auth_ok(message, auth_transcript(a.hello, a.bytes, fresh.hello, fresh.bytes)));

    // reflection: the same two messages in the other order are a different transcript
    CHECK(!auth_ok(message, auth_transcript(b.hello, b.bytes, a.hello, a.bytes)));
}

static void a06_nonces_are_fresh_and_32_bytes() {
    const Chain chain = make_chain(1000);
    Side one = side_for(chain), two = side_for(chain), three = side_for(chain);

    auto not_zero = [](const Hash32& n) {
        for (std::uint8_t b : n)
            if (b != 0) return true;
        return false;
    };
    CHECK(one.hello.nonce.size() == 32);
    CHECK(not_zero(one.hello.nonce));
    CHECK(not_zero(two.hello.nonce));
    CHECK(one.hello.nonce != two.hello.nonce);      // a fresh nonce per connection
    CHECK(two.hello.nonce != three.hello.nonce);
    CHECK(one.hello.nonce != three.hello.nonce);

    // and the nonce is part of the hello that travels
    CHECK(one.bytes.find(to_hex(one.hello.nonce)) != std::string::npos);
    Hello back;
    std::string why;
    CHECK(decode_hello(one.bytes, back, &why));
    CHECK(back.nonce == one.hello.nonce);
}

static void a07_auth_message_is_strict() {
    Key k = new_key();
    AuthMessage message;
    CHECK(sign_auth(message, k.sk, "some transcript"));
    const std::string good = encode_auth(message);

    AuthMessage back;
    std::string why;
    CHECK(decode_auth(good, back, &why));
    CHECK(!decode_auth("", back, &why));
    CHECK(!decode_auth("pubkey=" + to_hex(k.pk) + "\n", back, &why));            // signature missing
    CHECK(!decode_auth("signature=" + to_hex(message.signature) + "\n", back, &why));  // pubkey missing
    CHECK(!decode_auth(good + "extra=1\n", back, &why));                          // unknown field
    CHECK(!decode_auth(good + good, back, &why));                                 // duplicated
    CHECK(!decode_auth(std::string(600, 'x'), back, &why));                       // too large
    CHECK(!auth_ok(message, ""));                                                 // empty transcript
}

int main() {
    CHECK(kAuthDomain.size() == 15);   // the domain length is part of what is signed
    a01_transcript_is_deterministic();
    a02_length_prefixes_remove_ambiguity();
    a03_signature_round_trip();
    a04_any_change_breaks_it();
    a05_replay_and_reflection();
    a06_nonces_are_fresh_and_32_bytes();
    a07_auth_message_is_strict();
    if (failures == 0) { std::cout << "auth tests: all passed\n"; return 0; }
    std::cerr << "auth tests: " << failures << " failure(s)\n";
    return 1;
}
