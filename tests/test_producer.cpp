// v9: producer signatures. A block says who made it, and the claim is checked against the producer set the
// chain was started with. The field is NOT part of the block hash, so every chain written before v9 keeps its
// exact hashes and state roots — that compatibility case is a test here, not a hope.
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include "taz/persist.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
using namespace taz;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() {
    Key k;
    if (!generate_keypair(k.sk, k.pk)) { std::cerr << "keygen failed\n"; std::exit(2); }
    k.addr = address_from_pubkey(k.pk);
    return k;
}
static void resign(Transaction& tx, const Key& k) { if (!sign(k.sk, tx.signable_bytes(), tx.signature)) std::exit(2); }
static Transaction transfer(const Key& f, const Address& to, Amount a, std::uint64_t n) {
    Transaction tx; tx.from = f.addr; tx.nonce = n; tx.pubkey = f.pk; tx.to = to; tx.amount = a;
    resign(tx, f); return tx;
}
static std::int64_t ts = 100;

// the producer set must be sane, and can only be set before the chain has blocks
static void t01_set_producers_rules() {
    Key a = new_key(), b = new_key();
    Chain c; CHECK(c.init_genesis(a.addr, 1000, b.addr, 0));
    CHECK(!c.set_producers({}).ok);
    CHECK(!c.set_producers({a.addr, Address{}}).ok);
    CHECK(!c.set_producers({a.addr, a.addr}).ok);
    CHECK(c.set_producers({a.addr, b.addr}).ok);
    CHECK(!c.set_producers({a.addr}).ok);                 // already set
    CHECK(c.producers().size() == 2);
}

// round robin: height 1 -> producer 0, height 2 -> producer 1, height 3 -> producer 0
static void t02_round_robin() {
    Key o = new_key(), g = new_key(), p1 = new_key(), p2 = new_key();
    Chain c; CHECK(c.init_genesis(o.addr, 1000000, g.addr, 0));
    CHECK(c.set_producers({p1.addr, p2.addr}).ok);
    CHECK(c.append_block_signed({transfer(o, g.addr, 10, 0)}, ts++, p1.sk, p1.pk).ok);
    CHECK(c.append_block_signed({transfer(o, g.addr, 10, 1)}, ts++, p2.sk, p2.pk).ok);
    CHECK(c.append_block_signed({}, ts++, p1.sk, p1.pk).ok);
    CHECK(c.blocks()[1].producer == p1.addr);
    CHECK(c.blocks()[2].producer == p2.addr);
    CHECK(c.blocks()[3].producer == p1.addr);
    CHECK(c.blocks()[1].producer_pubkey == p1.pk);
}

// the wrong key for that height is refused before anything is committed
static void t03_wrong_key_for_height() {
    Key o = new_key(), g = new_key(), p1 = new_key(), p2 = new_key();
    Chain c; CHECK(c.init_genesis(o.addr, 1000000, g.addr, 0));
    CHECK(c.set_producers({p1.addr, p2.addr}).ok);
    const ChainResult r = c.append_block_signed({}, ts++, p2.sk, p2.pk);   // height 1 needs p1
    CHECK(!r.ok);
    CHECK(c.height() == 0);                                                // nothing committed
}

// a chain without a producer set must refuse a block that claims one
static void t04_claim_without_set() {
    Key o = new_key(), g = new_key(), p = new_key();
    Chain a; CHECK(a.init_genesis(o.addr, 1000, g.addr, 0));
    CHECK(a.append_block({}, ts++).ok);
    Block b = a.blocks()[1];
    b.producer = p.addr;
    b.producer_pubkey = p.pk;
    Chain target; CHECK(target.init_genesis(o.addr, 1000, g.addr, 0));
    const ChainResult r = target.accept_block(b);
    CHECK(!r.ok);
}

// a signature that does not match the block hash is refused
static void t05_tampered_signature() {
    Key o = new_key(), g = new_key(), p1 = new_key();
    Chain a; CHECK(a.init_genesis(o.addr, 1000000, g.addr, 0));
    CHECK(a.set_producers({p1.addr}).ok);
    CHECK(a.append_block_signed({transfer(o, g.addr, 10, 0)}, ts++, p1.sk, p1.pk).ok);
    Block b = a.blocks()[1];
    b.producer_signature[0] = static_cast<std::uint8_t>(b.producer_signature[0] ^ 0xffU);
    Chain target; CHECK(target.init_genesis(o.addr, 1000000, g.addr, 0));
    CHECK(target.set_producers({p1.addr}).ok);
    const ChainResult r = target.accept_block(b);
    CHECK(!r.ok);
}

// a producer chain survives export and reload, and the producer set is rebuilt from the blocks
static void t06_export_round_trip() {
    Key o = new_key(), g = new_key(), p1 = new_key(), p2 = new_key();
    Chain c; CHECK(c.init_genesis(o.addr, 1000000, g.addr, 0));
    CHECK(c.set_producers({p1.addr, p2.addr}).ok);
    CHECK(c.append_block_signed({transfer(o, g.addr, 10, 0)}, ts++, p1.sk, p1.pk).ok);
    CHECK(c.append_block_signed({transfer(o, g.addr, 20, 1)}, ts++, p2.sk, p2.pk).ok);
    const std::string text = to_json(c);
    CHECK(text.find("\"producer\"") != std::string::npos);
    CHECK(text.find("tazvera-chain-export-v5") != std::string::npos);
    Chain out; std::string err;
    CHECK(load_chain_json_text(text, out, &err));
    CHECK(out.height() == c.height());
    CHECK(out.producers().size() == 2);
    CHECK(out.blocks()[1].producer == p1.addr);
    CHECK(out.blocks()[2].producer == p2.addr);
    if (!out.blocks().empty()) CHECK(to_json(out) == text);   // byte identical re-export (guard: no crash on failure)
}

// a chain written before v9 must be untouched: no producer keys, same label, same hashes
static void t07_v8_chain_unchanged() {
    Key o = new_key(), g = new_key();
    Chain c; CHECK(c.init_genesis(o.addr, 1000000, g.addr, 0));
    CHECK(c.append_block({transfer(o, g.addr, 10, 0)}, ts++).ok);
    const std::string text = to_json(c);
    CHECK(text.find("\"producer\"") == std::string::npos);
    CHECK(text.find("tazvera-chain-export-v5") == std::string::npos);
    CHECK(c.blocks()[1].producer == Address{});
    Chain out; std::string err;
    CHECK(load_chain_json_text(text, out, &err));
    CHECK(out.producers().empty());
    CHECK(to_json(out) == text);
}


// v9 P1/P2: the producer list is committed in the genesis manifest, so a restart keeps it — and the order,
// which is the schedule — without inferring anything from the blocks.
static void t08_restart_keeps_producers() {
    Key o = new_key(), g = new_key(), p1 = new_key(), p2 = new_key();
    Chain c; CHECK(c.init_genesis(o.addr, 1000000, g.addr, 0, {p1.addr, p2.addr}));
    CHECK(c.producers().size() == 2);
    CHECK(c.append_block_signed({transfer(o, g.addr, 10, 0)}, ts++, p1.sk, p1.pk).ok);
    CHECK(c.append_block_signed({transfer(o, g.addr, 10, 1)}, ts++, p2.sk, p2.pk).ok);
    const std::string text = to_json(c);
    CHECK(text.find("genesis_producers") != std::string::npos);
    Chain out; std::string err;
    CHECK(load_chain_json_text(text, out, &err));
    if (out.producers().size() != 2) { CHECK(false); return; }
    CHECK(out.producers()[0] == p1.addr);              // same list
    CHECK(out.producers()[1] == p2.addr);              // same order
    CHECK(out.genesis_commitment() == c.genesis_commitment());
    // height 3 belongs to p1: the round robin survives the restart
    CHECK(!out.append_block_signed({}, ts++, p2.sk, p2.pk).ok);
    CHECK(out.append_block_signed({}, ts++, p1.sk, p1.pk).ok);
}

// the commitment must actually commit the list: same balances, different producers, different commitment
static void t09_commitment_commits_producers() {
    Key o = new_key(), g = new_key(), p1 = new_key(), p2 = new_key();
    Chain plain; CHECK(plain.init_genesis(o.addr, 1000, g.addr, 0));
    Chain with;  CHECK(with.init_genesis(o.addr, 1000, g.addr, 0, {p1.addr}));
    Chain two;   CHECK(two.init_genesis(o.addr, 1000, g.addr, 0, {p1.addr, p2.addr}));
    CHECK(plain.genesis_commitment() != with.genesis_commitment());
    CHECK(with.genesis_commitment() != two.genesis_commitment());
    // and the same list in a different order is a different chain
    Chain swapped; CHECK(swapped.init_genesis(o.addr, 1000, g.addr, 0, {p2.addr, p1.addr}));
    CHECK(swapped.genesis_commitment() != two.genesis_commitment());
    // block 0 carries that commitment, so every later block inherits it through previous_hash
    CHECK(with.blocks()[0].previous_hash == with.genesis_commitment());
}

// editing the declared producer list in a file must be refused
static void t10_tampered_producer_list() {
    Key o = new_key(), g = new_key(), p1 = new_key(), p2 = new_key();
    Chain c; CHECK(c.init_genesis(o.addr, 1000000, g.addr, 0, {p1.addr, p2.addr}));
    CHECK(c.append_block_signed({transfer(o, g.addr, 10, 0)}, ts++, p1.sk, p1.pk).ok);
    std::string text = to_json(c);
    const std::string p2hex = to_hex(p2.addr);
    const std::string p1hex = to_hex(p1.addr);
    const std::size_t at = text.find(p2hex);            // the second producer in the manifest
    CHECK(at != std::string::npos);
    if (at == std::string::npos) return;
    text.replace(at, p2hex.size(), p1hex);              // claim the chain had one producer twice
    Chain out; std::string err;
    CHECK(!load_chain_json_text(text, out, &err));
}


// v9 P3: the message names the chain and the height, so one block's signature cannot be moved to another
// block made by the same producer.
static void t11_signature_bound_to_height() {
    Key o = new_key(), g = new_key(), p1 = new_key();
    Chain c; CHECK(c.init_genesis(o.addr, 1000000, g.addr, 0, {p1.addr}));
    CHECK(c.append_block_signed({transfer(o, g.addr, 10, 0)}, ts++, p1.sk, p1.pk).ok);
    CHECK(c.append_block_signed({transfer(o, g.addr, 20, 1)}, ts++, p1.sk, p1.pk).ok);
    Block b = c.blocks()[2];
    b.producer_signature = c.blocks()[1].producer_signature;      // same producer, different height
    Chain target; CHECK(target.init_genesis(o.addr, 1000000, g.addr, 0, {p1.addr}));
    CHECK(target.append_block_signed({transfer(o, g.addr, 10, 0)}, ts++, p1.sk, p1.pk).ok);
    CHECK(!target.accept_block(b).ok);
}

int main() {
    t01_set_producers_rules();
    t02_round_robin();
    t03_wrong_key_for_height();
    t04_claim_without_set();
    t05_tampered_signature();
    t06_export_round_trip();
    t07_v8_chain_unchanged();
    t08_restart_keeps_producers();
    t09_commitment_commits_producers();
    t10_tampered_producer_list();
    t11_signature_bound_to_height();
    if (failures == 0) { std::cout << "producer tests: all passed\n"; return 0; }
    std::cerr << "producer tests: " << failures << " failure(s)\n";
    return 1;
}
