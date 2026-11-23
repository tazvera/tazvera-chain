// F3: transactions and block signatures are bound to ONE network (protocol 4).
//
// Before protocol 4 a transaction signed `protocol`, `chain_id`, from, to, amount ... but nothing that tells one TAZVERA
// network from another: CHAIN_ID is the same constant everywhere. A transaction signed for network A was therefore
// valid on network B whenever B had the same address with the same nonce. Chain files are public, so every signed
// transaction on an old network could be replayed on a new one that reused the address.
//
// Protocol 4 puts the network ID (the genesis commitment, which covers chain ID, protocol, both genesis accounts and
// the producer list) into the bytes that are signed, and a chain refuses a transaction whose network ID is not its own.
// A block signature signs the network ID as well, so it says in its own bytes which network it belongs to.
//
//   n01  the same signed transaction: accepted on its own network, refused on another with the same addresses
//   n02  a transaction that names another network is refused even with a valid signature for that other network
//   n03  the network ID changes with every genesis input and equals the genesis commitment
//   n04  the signature covers the network ID (change it and the signature fails)
//   n05  downgrade/replay between protocols: a protocol-3 transaction, in every disguise, is refused
//   n06  the producer signature is bound to the network (layout pinned; other network / old layout refused)
//   n07  the network ID survives a save/load, and a changed one does not load
//   n08  a state that was never bound to a network applies nothing
//   n09  a held-over proof: the same transaction cannot be replayed inside one network either (nonce), and a
//        network-bound transaction in a block of another network leaves that chain untouched
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include "taz/persist.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

using namespace taz;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{};
    Key() { if (!generate_keypair(sk, pk)) std::exit(2); addr = address_from_pubkey(pk); } };

static Transaction transfer(const Key& from, const Address& to, Amount amount, std::uint64_t nonce, const Hash32& network) {
    Transaction tx; tx.from = from.addr; tx.to = to; tx.amount = amount; tx.nonce = nonce; tx.pubkey = from.pk;
    tx.network_id = network;
    if (!sign(from.sk, tx.signable_bytes(), tx.signature)) std::exit(2);
    return tx;
}
static Address addr_n(std::uint8_t n) { Address a{}; a[0] = 0xee; a[19] = n; return a; }

static Chain chain_with(const Address& first, Amount first_balance, const Address& second, Amount second_balance) {
    Chain c;
    if (!c.init_genesis(first, first_balance, second, second_balance)) std::exit(2);
    return c;
}

// The v1/v2 signing layouts of protocols 1..3, written out here on purpose: the code under test no longer has them.
static void put_u32(std::vector<std::uint8_t>& o, std::uint32_t v) { for (int i = 3; i >= 0; --i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
static void put_u64(std::vector<std::uint8_t>& o, std::uint64_t v) { for (int i = 7; i >= 0; --i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
static void put_str(std::vector<std::uint8_t>& o, const std::string& s) { put_u32(o, static_cast<std::uint32_t>(s.size())); o.insert(o.end(), s.begin(), s.end()); }
static std::vector<std::uint8_t> old_transfer_layout(const Transaction& tx, std::uint32_t protocol) {
    std::vector<std::uint8_t> o;
    const std::string d = "TAZVERA-TX-SIGNATURE-V1";
    o.insert(o.end(), d.begin(), d.end());
    put_u32(o, protocol); put_str(o, tx.chain_id);
    o.insert(o.end(), tx.from.begin(), tx.from.end()); o.insert(o.end(), tx.to.begin(), tx.to.end());
    put_u64(o, tx.amount); put_u64(o, tx.fee); put_u64(o, tx.nonce); put_u64(o, tx.expiry_height);
    o.insert(o.end(), tx.pubkey.begin(), tx.pubkey.end());
    return o;
}

static void n01_same_transaction_on_two_networks() {
    const Key alice; const Address bob = addr_n(1), carol = addr_n(2);
    Chain a = chain_with(alice.addr, 1'000'000, bob, 0);          // network A
    Chain b = chain_with(alice.addr, 5'000'000, carol, 0);        // network B: same address, different genesis
    CHECK(a.network_id() != b.network_id());
    const Transaction tx = transfer(alice, bob, 400'000, 0, a.network_id());   // Alice signs for A only
    CHECK(a.append_block({tx}, 10).ok);
    const auto before = b.state().state_root();
    const auto r = b.append_block({tx}, 10);                       // the very same bytes, replayed on B
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(b.height() == 0);                                        // nothing was written
    CHECK(b.state().state_root() == before);
    CHECK(b.state().balance(bob) == 0);
}

static void n02_other_network_with_valid_signature() {
    const Key alice; const Address bob = addr_n(1), carol = addr_n(2);
    Chain a = chain_with(alice.addr, 1'000'000, bob, 0);
    Chain b = chain_with(alice.addr, 5'000'000, carol, 0);
    // A transaction that is perfectly signed FOR B is accepted on B and refused on A: the check is symmetric.
    const Transaction for_b = transfer(alice, carol, 10, 0, b.network_id());
    CHECK(!a.append_block({for_b}, 10).ok);
    CHECK(b.append_block({for_b}, 10).ok);
    // The all-zero network ID (what a transaction written by protocol 3 code would carry) is refused everywhere.
    const Transaction zero = transfer(alice, bob, 10, 0, Hash32{});
    CHECK(!a.append_block({zero}, 11).ok);
    // One flipped bit in the network ID, signature recomputed: still not this network.
    Hash32 off = a.network_id(); off[31] ^= 1;
    CHECK(!a.append_block({transfer(alice, bob, 10, 0, off)}, 11).ok);
    CHECK(a.append_block({transfer(alice, bob, 10, 0, a.network_id())}, 11).ok);
}

static void n03_network_id_follows_the_genesis() {
    const Key k1, k2, k3;
    Chain base = chain_with(k1.addr, 100, k2.addr, 0);
    CHECK(base.network_id() == base.genesis_commitment());
    CHECK(base.network_id() != Hash32{});
    CHECK(chain_with(k1.addr, 101, k2.addr, 0).network_id() != base.network_id());          // balance
    CHECK(chain_with(k1.addr, 100, k3.addr, 0).network_id() != base.network_id());          // address
    CHECK(chain_with(k2.addr, 0, k1.addr, 100).network_id() == base.network_id());          // order is canonical
    Chain with_producers;
    CHECK(with_producers.init_genesis(k1.addr, 100, k2.addr, 0, {k3.addr}));
    CHECK(with_producers.network_id() != base.network_id());                                // producers
    CHECK(with_producers.network_id() == with_producers.genesis_commitment());
    Chain late = chain_with(k1.addr, 100, k2.addr, 0);                                      // set_producers rewrites block 0
    CHECK(late.set_producers({k3.addr}).ok);
    CHECK(late.network_id() == late.genesis_commitment());
    CHECK(late.network_id() == with_producers.network_id());
}

static void n04_signature_covers_the_network_id() {
    const Key alice; const Address bob = addr_n(1);
    const Chain a = chain_with(alice.addr, 1000000, bob, 0);
    Transaction tx = transfer(alice, bob, 5, 0, a.network_id());
    CHECK(verify(tx.pubkey, tx.signable_bytes(), tx.signature));
    tx.network_id[0] ^= 0x80;                                       // same signature, different network
    CHECK(!verify(tx.pubkey, tx.signable_bytes(), tx.signature));
    // and the txid, which goes into the block hash, differs too
    Transaction t2 = transfer(alice, bob, 5, 0, a.network_id());
    Transaction t3 = transfer(alice, bob, 5, 0, Hash32{});
    CHECK(t2.txid() != t3.txid());
}

static void n05_downgrade_and_replay_between_protocols() {
    CHECK(PROTOCOL_VERSION >= 4);
    const Key alice; const Address bob = addr_n(1);
    Chain a = chain_with(alice.addr, 1'000'000, bob, 0);
    // A transaction as protocol 3 would have signed it: old layout, no network ID.
    Transaction legacy; legacy.from = alice.addr; legacy.to = bob; legacy.amount = 5; legacy.nonce = 0; legacy.pubkey = alice.pk;
    legacy.protocol_version = 3;
    CHECK(sign(alice.sk, old_transfer_layout(legacy, 3), legacy.signature));

    // 1. as it is: refused by the protocol-version rule (the stated policy: older signatures are not accepted).
    CHECK(!a.append_block({legacy}, 10).ok);
    CHECK(validate_tx_shape(legacy).has_value());                  // the shape check itself names the problem
    // 2. upgraded to the current protocol number, signature untouched: the bytes that are signed differ now.
    Transaction up = legacy; up.protocol_version = PROTOCOL_VERSION;
    CHECK(!verify(up.pubkey, up.signable_bytes(), up.signature));
    CHECK(!a.append_block({up}, 10).ok);
    // 3. upgraded and given this network's ID: still not signed over it.
    up.network_id = a.network_id();
    CHECK(!verify(up.pubkey, up.signable_bytes(), up.signature));
    CHECK(!a.append_block({up}, 10).ok);
    // 4. the old layout signed with the NEW protocol number (a downgrade of the layout, not of the number).
    Transaction mix = legacy; mix.protocol_version = PROTOCOL_VERSION; mix.network_id = a.network_id();
    CHECK(sign(alice.sk, old_transfer_layout(mix, PROTOCOL_VERSION), mix.signature));
    CHECK(!a.append_block({mix}, 10).ok);
    // 5. a current transaction rewritten down to protocol 3: the signature no longer matches and the version is refused.
    Transaction cur = transfer(alice, bob, 5, 0, a.network_id());
    Transaction down = cur; down.protocol_version = 3;
    CHECK(!a.append_block({down}, 10).ok);
    CHECK(a.height() == 0);
    // 6. and the honest current transaction is accepted on the same chain
    CHECK(a.append_block({cur}, 10).ok);
    CHECK(a.height() == 1);
}

// The producer signature layout of protocol 4, written out here: domain || chain_id || 0 || network_id || height || hash.
static std::vector<std::uint8_t> block_message(const std::string& domain, const Hash32* network, const Block& b) {
    std::vector<std::uint8_t> m;
    m.insert(m.end(), domain.begin(), domain.end());
    m.insert(m.end(), b.chain_id.begin(), b.chain_id.end()); m.push_back(0);
    if (network) m.insert(m.end(), network->begin(), network->end());
    put_u64(m, b.height);
    const Hash32 h = b.hash(); m.insert(m.end(), h.begin(), h.end());
    return m;
}

static void n06_producer_signature_is_network_bound() {
    const Key producer, funder; const Address other = addr_n(3);
    Chain ch;
    CHECK(ch.init_genesis(funder.addr, 1000, other, 0, {producer.addr}));
    // Build block 1 unsigned on a copy, then sign it by hand in each layout.
    Chain probe = ch;
    CHECK(probe.append_block_signed({}, 10, producer.sk, producer.pk).ok);
    Block good = probe.blocks().back();
    // the signature the chain itself made equals the documented layout
    CHECK(verify(good.producer_pubkey, block_message("TAZVERA-BLOCK-SIG-V3", &ch.network_id(), good), good.producer_signature));
    CHECK(ch.accept_block(good).ok);

    // the same block, signed for ANOTHER network, or in the protocol-3 layout, is refused on a fresh copy
    const Chain fresh = [&] { Chain c; c.init_genesis(funder.addr, 1000, other, 0, {producer.addr}); return c; }();
    Hash32 elsewhere = fresh.network_id(); elsewhere[0] ^= 1;
    Block wrong_net = good;
    CHECK(sign(producer.sk, block_message("TAZVERA-BLOCK-SIG-V3", &elsewhere, wrong_net), wrong_net.producer_signature));
    { Chain c = fresh; CHECK(!c.accept_block(wrong_net).ok); CHECK(c.height() == 0); }
    Block old_layout = good;
    CHECK(sign(producer.sk, block_message("TAZVERA-BLOCK-SIG-V2", nullptr, old_layout), old_layout.producer_signature));
    { Chain c = fresh; CHECK(!c.accept_block(old_layout).ok); CHECK(c.height() == 0); }
    { Chain c = fresh; CHECK(c.accept_block(good).ok); CHECK(c.height() == 1); }

    // a second network with the same producer key and the same funder, different amounts: block 1 of the first
    // network cannot be used on it (hash chain) and a signature re-made for it is a different signature
    Chain other_net; CHECK(other_net.init_genesis(funder.addr, 2000, other, 0, {producer.addr}));
    CHECK(!other_net.accept_block(good).ok);
}

static void n07_network_id_in_the_file() {
    const Key alice; const Address bob = addr_n(1);
    Chain a = chain_with(alice.addr, 1'000'000, bob, 0);
    CHECK(a.append_block({transfer(alice, bob, 7, 0, a.network_id())}, 10).ok);
    const std::string text = to_json(a);
    CHECK(text.find("\"network_id\"") != std::string::npos);
    Chain back; std::string err;
    CHECK(load_chain_json_text(text, back, &err));
    CHECK(back.state().state_root() == a.state().state_root());
    CHECK(back.network_id() == a.network_id());
    // change the network ID inside the transaction: the file must not load
    const std::string needle = "\"network_id\": \"" + to_hex(a.network_id()) + "\"";
    const std::size_t at = text.find(needle);
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        std::string tampered = text;
        Hash32 other = a.network_id(); other[0] ^= 1;
        tampered.replace(at, needle.size(), "\"network_id\": \"" + to_hex(other) + "\"");
        Chain out; std::string why;
        CHECK(!load_chain_json_text(tampered, out, &why));
        CHECK(!why.empty());
    }
    // a protocol-3 file (the number written by the previous release) is refused with a reason
    std::string v3 = text;
    const std::string pv = "\"protocol_version\": " + std::to_string(PROTOCOL_VERSION);
    const std::size_t p = v3.find(pv);
    CHECK(p != std::string::npos);
    if (p != std::string::npos) {
        v3.replace(p, pv.size(), "\"protocol_version\": 3");
        Chain out; std::string why;
        CHECK(!load_chain_json_text(v3, out, &why));
        CHECK(!why.empty());
    }
}

static void n08_unbound_state_applies_nothing() {
    const Key alice; const Address bob = addr_n(1);
    State s; s.set_balance(alice.addr, 1'000'000);
    CHECK(s.network_id() == Hash32{});
    CHECK(!s.apply(transfer(alice, bob, 5, 0, Hash32{}), 1).ok);       // even with a matching all-zero ID
    Hash32 some{}; some[5] = 9;
    CHECK(!s.apply(transfer(alice, bob, 5, 0, some), 1).ok);
    s.bind_network(some);
    CHECK(s.network_id() == some);
    CHECK(s.apply(transfer(alice, bob, 5, 0, some), 1).ok);
    CHECK(!s.apply(transfer(alice, bob, 5, 1, Hash32{}), 1).ok);
}

static void n09_failed_cross_network_block_leaves_no_trace() {
    const Key alice; const Address bob = addr_n(1), carol = addr_n(2);
    Chain a = chain_with(alice.addr, 1'000'000, bob, 0);
    Chain b = chain_with(alice.addr, 5'000'000, carol, 0);
    const Transaction good_a = transfer(alice, bob, 1, 0, a.network_id());
    // a block of A containing a tx for A and one that was signed for B: all-or-nothing
    const Transaction for_b = transfer(alice, bob, 1, 1, b.network_id());
    const auto root = a.state().state_root();
    CHECK(!a.append_block({good_a, for_b}, 10).ok);
    CHECK(a.height() == 0 && a.state().state_root() == root);
    CHECK(a.append_block({good_a}, 10).ok);
    CHECK(!a.append_block({good_a}, 11).ok);                          // plain replay inside the network: bad nonce
}

int main() {
    n01_same_transaction_on_two_networks();
    n02_other_network_with_valid_signature();
    n03_network_id_follows_the_genesis();
    n04_signature_covers_the_network_id();
    n05_downgrade_and_replay_between_protocols();
    n06_producer_signature_is_network_bound();
    n07_network_id_in_the_file();
    n08_unbound_state_applies_nothing();
    n09_failed_cross_network_block_leaves_no_trace();
    if (failures == 0) { std::cout << "network binding tests: all passed (9 cases)\n"; return 0; }
    std::cerr << "network binding tests: " << failures << " failure(s)\n";
    return 1;
}
