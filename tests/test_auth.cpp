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

    // a different chain commitment, on either side (which side is "first" depends on the random nonces)
    Hello other_a = a.hello;
    other_a.genesis_commitment[0] ^= 0xff;
    CHECK(!auth_ok(message, auth_transcript(other_a, a.bytes, b.hello, b.bytes)));
    Hello other_b = b.hello;
    other_b.genesis_commitment[0] ^= 0xff;
    CHECK(!auth_ok(message, auth_transcript(a.hello, a.bytes, other_b, b.bytes)));
    // a different protocol version or chain ID, on either side
    Hello proto_a = a.hello; proto_a.protocol_version += 1;
    CHECK(!auth_ok(message, auth_transcript(proto_a, a.bytes, b.hello, b.bytes)));
    Hello proto_b = b.hello; proto_b.protocol_version += 1;
    CHECK(!auth_ok(message, auth_transcript(a.hello, a.bytes, proto_b, b.bytes)));
    Hello id_a = a.hello; id_a.chain_id += "x";
    CHECK(!auth_ok(message, auth_transcript(id_a, a.bytes, b.hello, b.bytes)));
    Hello id_b = b.hello; id_b.chain_id += "x";
    CHECK(!auth_ok(message, auth_transcript(a.hello, a.bytes, id_b, b.bytes)));
}

// Replay: the same hellos, but one nonce is fresh. The transcript differs, so the recorded proof is useless.
static void a05_replay_fails() {
    const Chain chain = make_chain(1000);
    Side a = side_for(chain), b = side_for(chain);
    Key k = new_key();
    AuthMessage message;
    CHECK(sign_auth(message, k.sk, auth_transcript(a.hello, a.bytes, b.hello, b.bytes)));

    Side fresh = side_for(chain);
    CHECK(fresh.hello.nonce != b.hello.nonce);
    CHECK(!auth_ok(message, auth_transcript(a.hello, a.bytes, fresh.hello, fresh.bytes)));
}

// Reflection: a peer bounces OUR OWN hello (and our own proof) back at us. The transcript cannot tell the two sides
// apart (it is ordered by nonce, so it is the same whichever way round it is built), so the defence is the nonce
// rule: a peer whose nonce equals ours is refused. See a13. Here: a proof made for the real pair (a, b) is useless
// for the reflected pair (a, a), because the transcripts differ.
static void a06_reflection_fails() {
    const Chain chain = make_chain(1000);
    Side a = side_for(chain), b = side_for(chain);
    Key k = new_key();
    AuthMessage message;
    const std::string real = auth_transcript(a.hello, a.bytes, b.hello, b.bytes);
    CHECK(sign_auth(message, k.sk, real));
    const std::string bounced = auth_transcript(a.hello, a.bytes, a.hello, a.bytes);
    CHECK(bounced != real);
    CHECK(!auth_ok(message, bounced));
}

// Mutual: both sides sign the same transcript with their own key, and each verifies the other's proof. Two
// different keys, two different messages, one transcript.
static void a07_mutual_verification() {
    const Chain chain = make_chain(1000);
    Side a = side_for(chain), b = side_for(chain);
    const std::string transcript = auth_transcript(a.hello, a.bytes, b.hello, b.bytes);
    CHECK(!transcript.empty());

    Key ka = new_key(), kb = new_key();
    AuthMessage from_a, from_b;
    CHECK(sign_auth(from_a, ka.sk, transcript));
    CHECK(sign_auth(from_b, kb.sk, transcript));

    CHECK(from_a.pubkey != from_b.pubkey);
    CHECK(from_a.signature != from_b.signature);
    CHECK(auth_ok(from_a, transcript));       // B checks A
    CHECK(auth_ok(from_b, transcript));       // A checks B

    // a proof is bound to its key: claiming the other side's identity with your own signature fails
    AuthMessage impostor;
    CHECK(sign_auth(impostor, kb.sk, transcript));
    impostor.pubkey = ka.pk;                  // say "I am A", but sign with B's key
    CHECK(!auth_ok(impostor, transcript));
}

static void a08_nonces_are_fresh_and_32_bytes() {
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

static void a09_auth_message_is_strict() {
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

// The order the caller passes must not matter: the transcript sorts by nonce. Without this, a caller that passed
// the arguments the same way on both sides would open a reflection hole.
static void a10_argument_order_does_not_matter() {
    const Chain chain = make_chain(1000);
    Side a = side_for(chain), b = side_for(chain);
    const std::string one_way = auth_transcript(a.hello, a.bytes, b.hello, b.bytes);
    const std::string other_way = auth_transcript(b.hello, b.bytes, a.hello, a.bytes);
    CHECK(!one_way.empty());
    CHECK(one_way == other_way);

    Key k = new_key();
    AuthMessage message;
    CHECK(sign_auth(message, k.sk, one_way));
    CHECK(auth_ok(message, other_way));      // a proof made from one ordering verifies against the other
}

// An all-zero key is not an identity. Ed25519 accepts an all-zero seed as a valid key, so this has to be refused
// explicitly or an uninitialised PrivKey{} would happily sign.
static void a11_all_zero_keys_are_refused() {
    const Chain chain = make_chain(1000);
    Side a = side_for(chain), b = side_for(chain);
    const std::string transcript = auth_transcript(a.hello, a.bytes, b.hello, b.bytes);

    const PrivKey zero_seed{};
    AuthMessage message;
    CHECK(!sign_auth(message, zero_seed, transcript));

    AuthMessage zero_key;
    CHECK(!auth_ok(zero_key, transcript));   // all-zero pubkey

    Key k = new_key();
    AuthMessage good;
    CHECK(sign_auth(good, k.sk, transcript));
    CHECK(auth_ok(good, transcript));
}

static void a12_zero_nonce_is_refused() {
    const Chain chain = make_chain(1000);
    const Hello ours = hello_for(chain);
    Hello theirs = hello_for(chain);
    CHECK(hello_has_nonce(ours));
    CHECK(check_peer(ours, theirs).ok);          // a normal peer is fine
    theirs.nonce = Hash32{};
    const HandshakeDecision d = check_peer(ours, theirs);
    CHECK(!d.ok);
    CHECK(d.reason.find("nonce") != std::string::npos);
}

// The order-independent transcript has one consequence: our own proof, bounced back, would verify against the
// transcript we build (it is the same bytes). The only thing that stops that is refusing a peer whose nonce is ours.
static void a13_reflected_hello_is_refused() {
    const Chain chain = make_chain(1000);
    const Hello ours = hello_for(chain);
    Hello bounced = ours;                         // exactly what a reflecting peer would send
    const HandshakeDecision d = check_peer(ours, bounced);
    CHECK(!d.ok);
    CHECK(d.reason.find("nonce") != std::string::npos);
    const Hello other = hello_for(chain);         // a genuine peer has its own fresh nonce
    CHECK(check_peer(ours, other).ok);
}

int main() {
    CHECK(kAuthDomain.size() == 15);   // the domain length is part of what is signed
    a01_transcript_is_deterministic();
    a02_length_prefixes_remove_ambiguity();
    a03_signature_round_trip();
    a04_any_change_breaks_it();
    a05_replay_fails();
    a06_reflection_fails();
    a07_mutual_verification();
    a08_nonces_are_fresh_and_32_bytes();
    a09_auth_message_is_strict();
    a10_argument_order_does_not_matter();
    a11_all_zero_keys_are_refused();
    a12_zero_nonce_is_refused();
    a13_reflected_hello_is_refused();
    if (failures == 0) { std::cout << "auth tests: all passed\n"; return 0; }
    std::cerr << "auth tests: " << failures << " failure(s)\n";
    return 1;
}
