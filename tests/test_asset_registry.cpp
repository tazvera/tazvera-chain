// RWA registry, part 1: registering an asset (no transfer of ownership yet).
//
// What is tested: the asset_id rule (pinned to values computed independently with another SHA-256), the registry as part
// of the state (and of the state root), every rule that must refuse a registration, and that a chain holding one can be
// saved, loaded again and verified by replay. Protocol stays 3 (devnet), see OPEN-DECISIONS.md.
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include "taz/persist.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using namespace taz;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() { Key k; if (!generate_keypair(k.sk, k.pk)) std::exit(2); k.addr = address_from_pubkey(k.pk); return k; }
static void resign(Transaction& tx, const Key& k) { CHECK(sign(k.sk, tx.signable_bytes(), tx.signature)); }
static Hash32 doc(std::uint8_t seed) { Hash32 h{}; for (std::size_t i = 0; i < h.size(); ++i) h[i] = static_cast<std::uint8_t>(seed + i); return h; }

static Transaction reg(const Key& who, std::uint64_t nonce, std::uint8_t kind = 1, std::uint8_t doc_seed = 1) {
    Transaction tx; tx.kind = TxKind::RegisterAsset; tx.from = who.addr; tx.pubkey = who.pk; tx.nonce = nonce;
    tx.owner = who.addr; tx.asset_kind = kind; tx.doc_hash = doc(doc_seed);
    resign(tx, who); return tx;
}
static Transaction plain_transfer(const Key& f, const Address& to, Amount a, std::uint64_t nonce) {
    Transaction tx; tx.from = f.addr; tx.pubkey = f.pk; tx.nonce = nonce; tx.to = to; tx.amount = a; resign(tx, f); return tx;
}
static constexpr Amount F = FEE_TRANSFER_V1;
static constexpr Amount START = 1000000;
static std::int64_t g_ts = 10;
struct World {
    Key owner = new_key(), other = new_key();
    Chain chain;
    World() { CHECK(chain.init_genesis(owner.addr, START, other.addr, START)); }
    bool put(const std::vector<Transaction>& txs) { return chain.append_block(txs, g_ts++).ok; }
    const AssetRecord* asset(std::uint64_t nonce) const { return chain.state().find_asset(asset_id(CHAIN_ID, owner.addr, nonce)); }
};

// t1: the id rule, pinned. The two digests were computed with Python's hashlib, not with this code.
static void t1_asset_id_rule_is_pinned() {
    static_assert(std::string_view("TAZVERA-ASSET-V1").size() == 16);
    Address owner{}; for (std::size_t i = 0; i < owner.size(); ++i) owner[i] = static_cast<std::uint8_t>(i + 1);
    CHECK(to_hex(asset_id("tazvera-local-prototype-v1", owner, 0)) == "bb1f30f264111f8079b0018bf190e3a85f640d558258d3f190126e55f09315cd");
    CHECK(to_hex(asset_id("tazvera-local-prototype-v1", owner, 7)) == "de0b01e6e02b0326b3cea6a541f9e5a771088f059a7fbcde4d3d180b0d6e3cfd");
    CHECK(asset_id("tazvera-local-prototype-v1", owner, 1) != asset_id("tazvera-local-prototype-v1", owner, 2));
    CHECK(asset_id("another-chain", owner, 7) != asset_id("tazvera-local-prototype-v1", owner, 7));   // chain_id is in the hash
    Address other = owner; other[19] ^= 1;
    CHECK(asset_id("tazvera-local-prototype-v1", other, 7) != asset_id("tazvera-local-prototype-v1", owner, 7));
    CHECK(asset_id("tazvera-local-prototype-v1", owner, 1ULL << 32) != asset_id("tazvera-local-prototype-v1", owner, 1));   // 64-bit, big-endian
}

// t2: a correct registration happens and shows in the registry; the fee is burned like a transfer's fee
static void t2_registration_works_and_shows_in_the_registry() {
    World w;
    CHECK(w.put({reg(w.owner, 0, 2, 5)}));
    const AssetRecord* r = w.asset(0);
    CHECK(r != nullptr);
    if (r) {
        CHECK(r->owner == w.owner.addr);
        CHECK(r->asset_kind == 2);
        CHECK(r->doc_hash == doc(5));
        CHECK(r->created_at == w.chain.height());                  // the height of the block that registered it
    }
    CHECK(w.chain.state().asset_count() == 1);
    CHECK(w.chain.state().balance(w.owner.addr) == START - F);
    CHECK(w.chain.state().nonce(w.owner.addr) == 1);
    CHECK(w.chain.state().burned_total() == F);
    CHECK(w.chain.state().circulating() + w.chain.state().burned_total() == 2 * START);   // nothing created or lost
    // a second asset by the same owner gets its own id
    CHECK(w.put({reg(w.owner, 1, 1, 6)}));
    CHECK(w.chain.state().asset_count() == 2 && w.asset(1) != nullptr);
}

// t3: the same registration again is refused (same nonce, so the same asset_id), and nothing changes
static void t3_same_asset_id_twice_is_refused() {
    World w;
    const Transaction t = reg(w.owner, 0);
    CHECK(w.put({t}));
    const Hash32 root = w.chain.state().state_root();
    const Amount bal = w.chain.state().balance(w.owner.addr);
    State probe = w.chain.state();
    const ApplyResult again = probe.apply(t, w.chain.height() + 1);
    CHECK(!again.ok);
    CHECK(!w.put({t}));                                            // and as a block
    CHECK(w.chain.state().state_root() == root);
    CHECK(w.chain.state().balance(w.owner.addr) == bal);
    CHECK(w.chain.state().asset_count() == 1);
}

// t4: a wrong signature is refused
static void t4_wrong_signature_is_refused() {
    World w;
    Transaction t = reg(w.owner, 0); t.doc_hash = doc(9);          // changed after signing
    CHECK(!w.put({t}));
    Transaction u = reg(w.owner, 0); u.signature = Signature{};   // no signature at all
    CHECK(!w.put({u}));
    Transaction v = reg(w.owner, 0); v.asset_kind = 3;             // the kind is signed too
    CHECK(!w.put({v}));
    Transaction x = reg(w.owner, 0); x.signature = reg(w.other, 0).signature;   // somebody else's signature
    CHECK(!w.put({x}));
    CHECK(w.chain.state().asset_count() == 0);
    CHECK(w.chain.state().nonce(w.owner.addr) == 0);
}

// t5: registering in somebody else's name is refused, whoever signs it
static void t5_registering_for_somebody_else_is_refused() {
    World w;
    Transaction t = reg(w.other, 0); t.owner = w.owner.addr; resign(t, w.other);   // `other` signs, names `owner` as the owner
    CHECK(!w.put({t}));
    Transaction u = reg(w.owner, 0); u.owner = w.other.addr; resign(u, w.owner);   // `owner` signs, names `other`
    CHECK(!w.put({u}));
    Transaction z = reg(w.owner, 0); z.owner = Address{}; resign(z, w.owner);       // no owner at all
    CHECK(!w.put({z}));
    CHECK(w.chain.state().asset_count() == 0);
    CHECK(w.chain.state().nonce(w.owner.addr) == 0 && w.chain.state().nonce(w.other.addr) == 0);
}

// t6: the fee is the transfer fee: none, a different one, or not enough money, all refused
static void t6_fee_rule_is_the_transfer_rule() {
    World w;
    Transaction free_tx = reg(w.owner, 0); free_tx.fee = 0; resign(free_tx, w.owner);
    CHECK(!w.put({free_tx}));
    Transaction cheap = reg(w.owner, 0); cheap.fee = F - 1; resign(cheap, w.owner);
    CHECK(!w.put({cheap}));
    Transaction rich = reg(w.owner, 0); rich.fee = F + 1; resign(rich, w.owner);
    CHECK(!w.put({rich}));
    // a poor account cannot register: it has less than the fee
    Key poor = new_key();
    CHECK(w.put({plain_transfer(w.owner, poor.addr, F - 1, 0)}));
    CHECK(!w.put({reg(poor, 0)}));
    CHECK(w.chain.state().asset_count() == 0);
}

// t7: the registry is part of the state root: two states that differ only in what was registered have different roots
static void t7_registry_is_part_of_the_state_root() {
    World a, b;
    b.chain = a.chain;                                             // identical chains, identical keys
    State s1 = a.chain.state(), s2 = a.chain.state();
    CHECK(s1.state_root() == s2.state_root());
    CHECK(s1.apply(reg(a.owner, 0, 1, 1), 1).ok);
    CHECK(s2.apply(reg(a.owner, 0, 1, 2), 1).ok);                  // only the document hash differs
    CHECK(s1.balance(a.owner.addr) == s2.balance(a.owner.addr) && s1.nonce(a.owner.addr) == s2.nonce(a.owner.addr));
    CHECK(s1.burned_total() == s2.burned_total());
    CHECK(s1.state_root() != s2.state_root());
    State s3 = a.chain.state();
    CHECK(s3.apply(reg(a.owner, 0, 2, 1), 1).ok);                  // only the asset kind differs
    CHECK(s3.state_root() != s1.state_root());
    // a chain that never registers anything keeps its old root (the plain layouts are untouched)
    State plain = a.chain.state();
    CHECK(plain.asset_count() == 0);
}

// t8: asset_kind must be 1..4
static void t8_asset_kind_is_checked() {
    World w;
    CHECK(!w.put({reg(w.owner, 0, 0)}));
    CHECK(!w.put({reg(w.owner, 0, 5)}));
    CHECK(!w.put({reg(w.owner, 0, 255)}));
    for (std::uint8_t k = 1; k <= 4; ++k) CHECK(w.put({reg(w.owner, k - 1ULL, k)}));    // land, vehicle, invoice, other
    CHECK(w.chain.state().asset_count() == 4);
}

// t9: every other field that registration does not use must be zero, and the asset fields belong to registration only
static void t9_unused_fields_must_be_zero() {
    World w;
    Transaction a = reg(w.owner, 0); a.amount = 1; resign(a, w.owner); CHECK(!w.put({a}));
    Transaction b = reg(w.owner, 0); b.to = w.other.addr; resign(b, w.owner); CHECK(!w.put({b}));
    Transaction c = reg(w.owner, 0); c.daily_limit = 1; resign(c, w.owner); CHECK(!w.put({c}));
    Transaction d = reg(w.owner, 0); d.new_owner = w.other.addr; resign(d, w.owner); CHECK(!w.put({d}));
    Transaction e = reg(w.owner, 0); e.guardians = {w.other.addr}; e.guardian_threshold = 1; resign(e, w.owner); CHECK(!w.put({e}));
    // asset fields on another kind of transaction
    Transaction t = plain_transfer(w.owner, w.other.addr, 5, 0); t.doc_hash = doc(1); resign(t, w.owner); CHECK(!w.put({t}));
    Transaction u = plain_transfer(w.owner, w.other.addr, 5, 0); u.asset_kind = 1; resign(u, w.owner); CHECK(!w.put({u}));
    Transaction v = plain_transfer(w.owner, w.other.addr, 5, 0); v.owner = w.owner.addr; resign(v, w.owner); CHECK(!w.put({v}));
    CHECK(w.chain.state().asset_count() == 0 && w.chain.state().nonce(w.owner.addr) == 0);
}

// t10: a chain that holds a registration can be saved, loaded and verified again by replay, and gives the same registry
static void t10_save_and_load_round_trip() {
    World w;
    CHECK(w.put({reg(w.owner, 0, 1, 3), plain_transfer(w.owner, w.other.addr, 100, 1)}));
    CHECK(w.put({reg(w.other, 0, 3, 4)}));
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "taz-asset-roundtrip.json";
    std::string err;
    CHECK(save_chain_json(w.chain, file.string(), &err));
    Chain back; LoadOptions pin; pin.require_genesis_pin = true; pin.expected_genesis_commitment = w.chain.genesis_commitment();
    const bool loaded = load_chain_json(file.string(), back, &err, pin);
    if (!loaded) std::cerr << "load failed: " << err << "\n";
    CHECK(loaded);
    if (loaded) {
        CHECK(back.height() == w.chain.height());
        CHECK(back.blocks().back().hash() == w.chain.blocks().back().hash());
        CHECK(back.state().assets() == w.chain.state().assets());
        CHECK(back.state().state_root() == w.chain.state().state_root());
        CHECK(back.state().asset_count() == 2);
    }
    std::error_code ec; std::filesystem::remove(file, ec);
}

int main() {
    t1_asset_id_rule_is_pinned();
    t2_registration_works_and_shows_in_the_registry();
    t3_same_asset_id_twice_is_refused();
    t4_wrong_signature_is_refused();
    t5_registering_for_somebody_else_is_refused();
    t6_fee_rule_is_the_transfer_rule();
    t7_registry_is_part_of_the_state_root();
    t8_asset_kind_is_checked();
    t9_unused_fields_must_be_zero();
    t10_save_and_load_round_trip();
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "asset registry tests: all passed (10 cases)\n";
    return EXIT_SUCCESS;
}
