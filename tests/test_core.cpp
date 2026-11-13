#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace taz;
static int failures = 0;
#define CHECK(x) do { if(!(x)){ std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while(0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() {
    Key k; CHECK(generate_keypair(k.sk, k.pk)); k.addr = address_from_pubkey(k.pk); return k;
}
static void resign(Transaction& tx, const PrivKey& sk) {
    const auto bytes = tx.signable_bytes(); CHECK(sign(sk, bytes, tx.signature));
}
static Transaction make_tx(const Key& from, const Address& to, Amount amount, std::uint64_t nonce,
                           std::uint64_t expiry = 0) {
    Transaction tx; tx.from = from.addr; tx.to = to; tx.amount = amount; tx.fee = FEE_TRANSFER_V1;
    tx.nonce = nonce; tx.expiry_height = expiry; tx.pubkey = from.pk;
    resign(tx, from.sk); return tx;
}
static constexpr Amount INITIAL = 100000000;

static void test_signatures_and_basic_transfer() {
    Key alice = new_key(), bob = new_key();
    Chain chain; CHECK(chain.init_genesis(alice.addr, INITIAL, bob.addr, 0)); CHECK(chain.height() == 0);
    auto tx = make_tx(alice, bob.addr, 5000000, 0);
    CHECK(verify(alice.pk, tx.signable_bytes(), tx.signature));
    auto bad = tx; bad.signature[0] ^= 0x80; CHECK(!verify(alice.pk, bad.signable_bytes(), bad.signature));
    auto wrong_chain = tx; wrong_chain.chain_id = "some-other-chain";
    CHECK(!chain.append_block({wrong_chain}, 1).ok); CHECK(chain.state().balance(alice.addr) == INITIAL);
    auto result = chain.append_block({tx}, 1); CHECK(result.ok); CHECK(chain.height() == 1);
    CHECK(chain.state().balance(alice.addr) == INITIAL - 5000000 - FEE_TRANSFER_V1);
    CHECK(chain.state().balance(bob.addr) == 5000000);
    CHECK(chain.state().burned_total() == FEE_TRANSFER_V1);
    CHECK(chain.state().circulating() + chain.state().burned_total() == INITIAL);
    CHECK(chain.state().nonce(alice.addr) == 1);
    // replay of the same transaction
    CHECK(!chain.append_block({tx}, 2).ok); CHECK(chain.height() == 1);
    // tampering with a signed field invalidates the signature
    auto tampered = make_tx(alice, bob.addr, 10, 1); tampered.amount = 11;
    CHECK(!chain.append_block({tampered}, 2).ok);
}

static void test_rejections() {
    Key alice = new_key(), bob = new_key(), carol = new_key();
    Chain chain; CHECK(chain.init_genesis(alice.addr, INITIAL, bob.addr, 5000));
    const auto root = chain.state().state_root();
    // insufficient balance (bob has 5000, needs amount + 1000 fee)
    CHECK(!chain.append_block({make_tx(bob, carol.addr, 4500, 0)}, 1).ok);
    // wrong fee
    auto wrong_fee = make_tx(alice, bob.addr, 1, 0); wrong_fee.fee++; resign(wrong_fee, alice.sk);
    CHECK(!chain.append_block({wrong_fee}, 1).ok);
    // self transfer
    CHECK(!chain.append_block({make_tx(alice, alice.addr, 1, 0)}, 1).ok);
    // zero amount
    CHECK(!chain.append_block({make_tx(alice, bob.addr, 0, 0)}, 1).ok);
    // public key does not match the sender address (signature itself is valid)
    Transaction mismatch; mismatch.from = bob.addr; mismatch.to = carol.addr; mismatch.amount = 1;
    mismatch.nonce = 0; mismatch.pubkey = alice.pk; resign(mismatch, alice.sk);
    CHECK(!chain.append_block({mismatch}, 1).ok);
    // signed by the wrong key for the claimed public key
    Transaction forged = make_tx(alice, bob.addr, 1, 0); resign(forged, bob.sk);
    CHECK(!chain.append_block({forged}, 1).ok);
    // nonce skipped ahead
    CHECK(!chain.append_block({make_tx(alice, bob.addr, 1, 5)}, 1).ok);
    // amount + fee overflow
    CHECK(!chain.append_block({make_tx(alice, bob.addr, std::numeric_limits<Amount>::max(), 0)}, 1).ok);
    CHECK(!chain.append_block({make_tx(alice, bob.addr, std::numeric_limits<Amount>::max() - FEE_TRANSFER_V1 + 1, 0)}, 1).ok);
    // wrong protocol version
    auto wrong_ver = make_tx(alice, bob.addr, 1, 0); wrong_ver.protocol_version = 2; resign(wrong_ver, alice.sk);
    CHECK(!chain.append_block({wrong_ver}, 1).ok);
    CHECK(chain.height() == 0); CHECK(chain.state().state_root() == root);
}

static void test_expiry() {
    Key alice = new_key(), bob = new_key();
    Chain chain; CHECK(chain.init_genesis(alice.addr, INITIAL, bob.addr, 0));
    CHECK(chain.append_block({}, 1).ok); // height 1, empty block
    // expiry_height 1 is still valid at height 2? No: 2 > 1, so it must be rejected.
    CHECK(!chain.append_block({make_tx(alice, bob.addr, 1, 0, /*expiry=*/1)}, 2).ok);
    // expiry_height equal to the inclusion height is allowed
    CHECK(chain.append_block({make_tx(alice, bob.addr, 1, 0, /*expiry=*/2)}, 2).ok);
    CHECK(chain.height() == 2);
}

static void test_receiver_overflow() {
    Key alice = new_key(), bob = new_key();
    Chain chain; CHECK(chain.init_genesis(alice.addr, INITIAL, bob.addr, std::numeric_limits<Amount>::max() - 100));
    CHECK(!chain.append_block({make_tx(alice, bob.addr, 5000, 0)}, 1).ok);
    CHECK(chain.state().balance(alice.addr) == INITIAL); CHECK(chain.height() == 0);
}

static void test_atomic_block() {
    Key alice = new_key(), bob = new_key();
    Chain chain; CHECK(chain.init_genesis(alice.addr, INITIAL, bob.addr, 0));
    const auto before = chain.state().state_root();
    // first tx is valid, second is valid-looking but spends more than remains
    auto first = make_tx(alice, bob.addr, 1000000, 0);
    auto too_big = make_tx(alice, bob.addr, INITIAL, 1);
    auto r = chain.append_block({first, too_big}, 1);
    CHECK(!r.ok); CHECK(r.error.find("tx 1") != std::string::npos);
    CHECK(chain.height() == 0); CHECK(chain.state().state_root() == before);
    CHECK(chain.state().balance(bob.addr) == 0);
    // and the valid transaction alone still works afterwards
    CHECK(chain.append_block({first}, 1).ok);
    // two transactions from one sender in one block, in nonce order
    CHECK(chain.append_block({make_tx(alice, bob.addr, 10, 1), make_tx(alice, bob.addr, 20, 2)}, 2).ok);
    CHECK(chain.state().nonce(alice.addr) == 3); CHECK(chain.state().burned_total() == 3 * FEE_TRANSFER_V1);
    // duplicate nonce inside one block
    CHECK(!chain.append_block({make_tx(alice, bob.addr, 10, 3), make_tx(alice, bob.addr, 10, 3)}, 3).ok);
    CHECK(chain.state().nonce(alice.addr) == 3);
}

static void test_timestamps() {
    Key alice = new_key(), bob = new_key();
    Chain chain; CHECK(chain.init_genesis(alice.addr, INITIAL, bob.addr, 0));
    CHECK(!chain.append_block({}, 0).ok);   // not greater than genesis (0)
    CHECK(!chain.append_block({}, -5).ok);  // negative
    CHECK(chain.append_block({}, 10).ok);
    CHECK(!chain.append_block({}, 10).ok);  // equal to previous
    CHECK(!chain.append_block({}, 9).ok);   // backwards
    CHECK(!chain.append_block({}, 100, /*max=*/50).ok); // too far in the future
    CHECK(chain.append_block({}, 50, /*max=*/50).ok);   // exactly at the limit
    CHECK(chain.height() == 2);
}

static void test_block_size_limit() {
    Key alice = new_key(), bob = new_key();
    Chain chain; CHECK(chain.init_genesis(alice.addr, INITIAL, bob.addr, 0));
    std::vector<Transaction> many(MAX_BLOCK_TXS + 1, make_tx(alice, bob.addr, 1, 0));
    auto r = chain.append_block(many, 1);
    CHECK(!r.ok); CHECK(r.error.find("too many") != std::string::npos);
}

static void test_state_canonical_form() {
    Address x{}; x[0] = 7; Address y{}; y[0] = 9;
    State empty;
    State with_zero; with_zero.set_balance(x, 0);
    CHECK(with_zero.state_root() == empty.state_root()); CHECK(with_zero.account_count() == 0);
    State grown_then_emptied; grown_then_emptied.set_balance(x, 5); grown_then_emptied.set_balance(x, 0);
    CHECK(grown_then_emptied.state_root() == empty.state_root());
    State a, b; a.set_balance(x, 1); a.set_balance(y, 2); b.set_balance(y, 2); b.set_balance(x, 1);
    CHECK(a.state_root() == b.state_root()); // insertion order does not matter
    State c; c.set_balance(x, 1); c.set_balance(y, 3); CHECK(a.state_root() != c.state_root());
    // genesis with an explicit zero balance equals genesis without that account
    Key alice = new_key(), bob = new_key();
    Chain one, two; CHECK(one.init_genesis(alice.addr, 10, bob.addr, 0)); CHECK(two.init_genesis(alice.addr, 10, bob.addr, 0));
    CHECK(one.blocks().front().hash() == two.blocks().front().hash());
    CHECK(one.state().account_count() == 1);
    Chain dup; CHECK(!dup.init_genesis(alice.addr, 1, alice.addr, 1)); // same address twice
    CHECK(!one.init_genesis(alice.addr, 1, bob.addr, 1));              // genesis only once
}

static void test_block_hash_commitments() {
    Key alice = new_key(), bob = new_key();
    Chain chain; CHECK(chain.init_genesis(alice.addr, INITIAL, bob.addr, 0));
    CHECK(chain.append_block({make_tx(alice, bob.addr, 1000, 0)}, 1).ok);
    const Block base = chain.blocks().back(); const auto h = base.hash();
    auto b = base; b.timestamp += 1; CHECK(b.hash() != h);
    b = base; b.transactions.clear(); CHECK(b.hash() != h);
    b = base; b.state_root[0] ^= 1; CHECK(b.hash() != h);
    b = base; b.previous_hash[0] ^= 1; CHECK(b.hash() != h);
    b = base; b.height += 1; CHECK(b.hash() != h);
    b = base; b.chain_id = "x"; CHECK(b.hash() != h);
    b = base; b.protocol_version += 1; CHECK(b.hash() != h);
    b = base; b.transactions[0].signature[0] ^= 1; CHECK(b.hash() != h); // txid covers the signature
    CHECK(base.hash() == h); // hashing is deterministic
}

static void test_validate_and_accept_block() {
    Key alice = new_key(), bob = new_key();
    Chain producer, follower;
    CHECK(producer.init_genesis(alice.addr, INITIAL, bob.addr, 0));
    CHECK(follower.init_genesis(alice.addr, INITIAL, bob.addr, 0));
    CHECK(producer.append_block({make_tx(alice, bob.addr, 1000, 0)}, 1).ok);
    const Block good = producer.blocks().back();
    // a different node re-executes the block and reaches the same state
    CHECK(follower.validate_block(good).ok); CHECK(follower.height() == 0); // validate does not commit
    CHECK(follower.accept_block(good).ok); CHECK(follower.height() == 1);
    CHECK(follower.state().state_root() == producer.state().state_root());
    CHECK(follower.blocks().back().hash() == good.hash());

    // build a second block on the producer, then attack copies of it
    CHECK(producer.append_block({make_tx(alice, bob.addr, 2000, 1)}, 2).ok);
    const Block next = producer.blocks().back();
    const auto root_before = follower.state().state_root();
    auto reject = [&](Block bad) {
        auto r = follower.accept_block(bad);
        CHECK(!r.ok); CHECK(follower.height() == 1); CHECK(follower.state().state_root() == root_before);
    };
    { auto b = next; b.state_root[0] ^= 1; reject(b); }                // wrong claimed state root
    { auto b = next; b.previous_hash[0] ^= 1; reject(b); }             // wrong parent
    { auto b = next; b.height = 5; reject(b); }                        // wrong height
    { auto b = next; b.height = 1; reject(b); }                        // already-known height
    { auto b = next; b.timestamp = 1; reject(b); }                     // not after parent
    { auto b = next; b.chain_id = "other"; reject(b); }                // wrong chain
    { auto b = next; b.protocol_version = 9; reject(b); }              // wrong version
    { auto b = next; b.transactions[0].amount += 1; reject(b); }       // signed field changed
    { auto b = next; b.transactions[0].signature[3] ^= 1; reject(b); } // broken signature
    { auto b = next; b.transactions.clear(); reject(b); }              // txs removed, root now wrong
    { auto b = next; b.transactions.push_back(b.transactions[0]); reject(b); } // replayed inside block
    CHECK(!follower.validate_block(next, /*max_timestamp=*/1).ok);     // future-dated vs caller's limit
    CHECK(follower.accept_block(next).ok); CHECK(follower.height() == 2);
    CHECK(!follower.accept_block(next).ok); // cannot be accepted twice
    CHECK(follower.state().state_root() == producer.state().state_root());
}

static void test_wipe_and_keys() {
    PrivKey sk{}; PubKey pk{}; CHECK(generate_keypair(sk, pk));
    bool nonzero = false; for (auto v : sk) nonzero = nonzero || v != 0; CHECK(nonzero);
    secure_wipe(sk); for (auto v : sk) CHECK(v == 0);
    secure_wipe(std::span<std::uint8_t>{}); // empty span is a no-op
    // sign/verify round trip and wrong-key rejection
    Key a = new_key(), b = new_key(); const std::vector<std::uint8_t> msg{1, 2, 3};
    Signature sig{}; CHECK(sign(a.sk, msg, sig)); CHECK(verify(a.pk, msg, sig)); CHECK(!verify(b.pk, msg, sig));
    const std::vector<std::uint8_t> other{1, 2, 4}; CHECK(!verify(a.pk, other, sig));
    // SHA-256 known answer: sha256("abc")
    const std::vector<std::uint8_t> abc{'a', 'b', 'c'};
    CHECK(to_hex(sha256(abc)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

int main() {
    test_signatures_and_basic_transfer();
    test_rejections();
    test_expiry();
    test_receiver_overflow();
    test_atomic_block();
    test_timestamps();
    test_block_size_limit();
    test_state_canonical_form();
    test_block_hash_commitments();
    test_validate_and_accept_block();
    test_wipe_and_keys();
    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "All local core tests passed. This is not a security audit.\n"; return EXIT_SUCCESS;
}
