// RWA registry, part 2: transferring the ownership of an asset (TransferAsset).
//
// The transaction names the asset in `ref` (its asset_id), the new owner in `to` and the current owner in `owner`; it is
// signed by the current owner's key. Only the owner changes. While a recovery is pending for the owner's address the asset
// cannot be moved (a thief with the stolen key would try exactly that). Protocol stays 3 (devnet), see OPEN-DECISIONS.md.
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/persist.hpp"
#include <algorithm>
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
static Transaction base(const Key& f, std::uint64_t nonce) { Transaction tx; tx.from = f.addr; tx.pubkey = f.pk; tx.nonce = nonce; return tx; }
static Transaction transfer(const Key& f, const Address& to, Amount a, std::uint64_t n) { Transaction tx = base(f, n); tx.to = to; tx.amount = a; resign(tx, f); return tx; }
static Hash32 doc(std::uint8_t seed) { Hash32 h{}; for (std::size_t i = 0; i < h.size(); ++i) h[i] = static_cast<std::uint8_t>(seed + i); return h; }
static Transaction reg(const Key& who, std::uint64_t nonce, std::uint8_t kind = 1, std::uint8_t seed = 1) {
    Transaction tx = base(who, nonce); tx.kind = TxKind::RegisterAsset; tx.owner = who.addr; tx.asset_kind = kind; tx.doc_hash = doc(seed); resign(tx, who); return tx;
}
// give `asset` to `to`; `from` signs and is named as the current owner
static Transaction give(const Key& from, const Hash32& asset, const Address& to, std::uint64_t nonce) {
    Transaction tx = base(from, nonce); tx.kind = TxKind::TransferAsset; tx.ref = asset; tx.to = to; tx.owner = from.addr; resign(tx, from); return tx;
}
static Transaction setg(const Key& f, std::vector<Address> list, std::uint64_t thr, std::uint64_t n) {
    std::sort(list.begin(), list.end()); Transaction tx = base(f, n); tx.kind = TxKind::SetGuardians; tx.guardians = std::move(list); tx.guardian_threshold = thr; resign(tx, f); return tx;
}
static Transaction vote(const Key& g, const Address& who, const Address& new_owner, std::uint64_t n) {
    Transaction tx = base(g, n); tx.kind = TxKind::RecoveryVote; tx.to = who; tx.new_owner = new_owner; resign(tx, g); return tx;
}
static Transaction rcancel(const Key& f, std::uint64_t n) { Transaction tx = base(f, n); tx.kind = TxKind::RecoveryCancel; resign(tx, f); return tx; }
static constexpr Amount F = FEE_TRANSFER_V1;
static constexpr Amount START = 1000000;

static Amount total(const State& s) { return s.circulating() + s.held_total() + s.burned_total(); }

// A State with a (the owner), b, c, and one asset of a registered at nonce 0.
struct Scene {
    Key a = new_key(), b = new_key(), c = new_key();
    State s;
    Hash32 id{};
    Scene() {
        s.set_balance(a.addr, START); s.set_balance(b.addr, START); s.set_balance(c.addr, START);
        CHECK(s.apply(reg(a, 0, 2, 9), 1).ok);
        id = asset_id(CHAIN_ID, a.addr, 0);
    }
    Hash32 other_root() const { return s.state_root(); }
};

// t1: the owner gives the asset away: the owner changes, the fee is burned like a transfer's, nothing else moves
static void t1_owner_gives_the_asset_away() {
    Scene w;
    const Amount t0 = total(w.s), bal = w.s.balance(w.a.addr), burned = w.s.burned_total();
    const auto res = w.s.apply(give(w.a, w.id, w.b.addr, 1), 2);
    CHECK(res.ok);
    const AssetRecord* r = w.s.find_asset(w.id);
    CHECK(r && r->owner == w.b.addr);
    CHECK(w.s.balance(w.a.addr) == bal - F && w.s.nonce(w.a.addr) == 2 && w.s.burned_total() == burned + F);
    CHECK(w.s.balance(w.b.addr) == START);                            // the receiver pays nothing and receives no money
    CHECK(total(w.s) == t0);                                          // supply conserved (the fee moves to "burned")
    CHECK(w.s.asset_count() == 1);                                    // nothing added, nothing removed
    // the new owner can pass it on, the old one cannot any more
    CHECK(!w.s.apply(give(w.a, w.id, w.c.addr, 2), 3).ok);
    CHECK(w.s.apply(give(w.b, w.id, w.c.addr, 0), 3).ok);
    CHECK(w.s.find_asset(w.id)->owner == w.c.addr);
}

// t2: somebody who is not the owner cannot move it, whatever he writes into the "owner" field
static void t2_a_stranger_cannot_transfer() {
    Scene w;
    const auto before = w.s.assets(); const Hash32 root = w.s.state_root();
    {   // names the real owner, signs with his own key: the signer is not the owner
        Transaction tx = base(w.b, 0); tx.kind = TxKind::TransferAsset; tx.ref = w.id; tx.to = w.c.addr; tx.owner = w.a.addr; resign(tx, w.b);
        const auto r = w.s.apply(tx, 2); CHECK(!r.ok && r.error == "only the owner may transfer this asset");
    }
    {   // names himself as the owner: the registry says otherwise
        const auto r = w.s.apply(give(w.b, w.id, w.c.addr, 0), 2); CHECK(!r.ok && r.error == "only the owner may transfer this asset");
    }
    CHECK(w.s.assets() == before && w.s.state_root() == root && w.s.nonce(w.b.addr) == 0 && w.s.balance(w.b.addr) == START);
}

// t3: a wrong signature is refused
static void t3_wrong_signature_is_refused() {
    Scene w;
    const Hash32 root = w.s.state_root();
    { Transaction tx = give(w.a, w.id, w.b.addr, 1); tx.to = w.c.addr; CHECK(!w.s.apply(tx, 2).ok); }            // `to` changed after signing
    { Transaction tx = give(w.a, w.id, w.b.addr, 1); tx.ref[0] ^= 1; CHECK(!w.s.apply(tx, 2).ok); }              // asset id changed after signing
    { Transaction tx = give(w.a, w.id, w.b.addr, 1); tx.signature = Signature{}; CHECK(!w.s.apply(tx, 2).ok); }  // no signature
    { Transaction tx = give(w.a, w.id, w.b.addr, 1); resign(tx, w.b); CHECK(!w.s.apply(tx, 2).ok); }             // somebody else's signature
    CHECK(w.s.find_asset(w.id)->owner == w.a.addr && w.s.state_root() == root);
}

// t4: an asset that does not exist
static void t4_unknown_asset_is_refused() {
    Scene w;
    Hash32 nope = w.id; nope[31] ^= 1;
    const auto r = w.s.apply(give(w.a, nope, w.b.addr, 1), 2);
    CHECK(!r.ok && r.error == "asset was not found");
    CHECK(!w.s.apply(give(w.a, Hash32{}, w.b.addr, 1), 2).ok);
    CHECK(w.s.nonce(w.a.addr) == 1 && w.s.find_asset(w.id)->owner == w.a.addr);   // the failed attempts cost nothing and changed nothing
}

// t5: the same asset cannot be sold twice in one block (no double spending); through a real chain
static void t5_no_double_sale_in_one_block() {
    Key a = new_key(), b = new_key(), c = new_key();
    Chain ch; CHECK(ch.init_genesis(a.addr, START, b.addr, START));
    std::int64_t ts = 10;
    CHECK(ch.append_block({reg(a, 0, 1, 3)}, ts++).ok);
    const Hash32 id = asset_id(CHAIN_ID, a.addr, 0);
    const Hash32 root = ch.state().state_root(); const std::uint64_t h = ch.height();
    // two different buyers, two nonces, one block
    CHECK(!ch.append_block({give(a, id, b.addr, 1), give(a, id, c.addr, 2)}, ts++).ok);
    CHECK(ch.height() == h && ch.state().state_root() == root && ch.state().find_asset(id)->owner == a.addr);   // the whole block was refused
    // the same transaction twice
    const Transaction t = give(a, id, b.addr, 1);
    CHECK(!ch.append_block({t, t}, ts++).ok);
    CHECK(ch.height() == h && ch.state().find_asset(id)->owner == a.addr);
    // one of them alone is fine; afterwards the old owner's second attempt is refused
    CHECK(ch.append_block({give(a, id, b.addr, 1)}, ts++).ok);
    CHECK(ch.state().find_asset(id)->owner == b.addr);
    CHECK(!ch.append_block({give(a, id, c.addr, 2)}, ts++).ok);
    CHECK(ch.state().find_asset(id)->owner == b.addr);
}

// t6: while a recovery is pending for the owner's address the asset cannot be moved
static void t6_blocked_while_a_recovery_is_pending() {
    Key o = new_key(), g0 = new_key(), g1 = new_key(), n = new_key(), x = new_key(), y = new_key();
    State s; for (const Key* k : {&o, &g0, &g1, &x, &y}) s.set_balance(k->addr, START);
    CHECK(s.apply(setg(o, {g0.addr, g1.addr}, 2, 0), 1).ok);
    CHECK(s.apply(reg(o, 1), 1).ok); CHECK(s.apply(reg(x, 0), 1).ok);
    const Hash32 oid = asset_id(CHAIN_ID, o.addr, 1), xid = asset_id(CHAIN_ID, x.addr, 0);
    CHECK(s.apply(give(o, oid, y.addr, 2), 2).ok);                    // before any vote: allowed (y gives it back at once)
    CHECK(s.apply(give(y, oid, o.addr, 0), 2).ok);
    CHECK(s.apply(vote(g0, o.addr, n.addr, 0), 2).ok);
    CHECK(s.apply(vote(g1, o.addr, n.addr, 0), 3).ok);
    CHECK(s.find(o.addr)->recovery.active);
    const Hash32 root = s.state_root();
    const auto r = s.apply(give(o, oid, y.addr, 3), 4);
    CHECK(!r.ok && r.error == "asset transfer is blocked while a recovery is pending");
    CHECK(s.find_asset(oid)->owner == o.addr && s.state_root() == root);
    CHECK(s.apply(give(x, xid, y.addr, 1), 4).ok);                    // somebody else's address is not affected
    CHECK(s.apply(rcancel(o, 3), 5).ok);                              // the owner stops the recovery ...
    CHECK(s.apply(give(o, oid, y.addr, 4), 6).ok);                    // ... and can move the asset again
}

// t6b: when the recovery runs, the assets go to the new address and the old key can no longer move them
static void t6b_after_the_recovery_the_old_key_has_nothing() {
    Key o = new_key(), g0 = new_key(), g1 = new_key(), n = new_key(), y = new_key();
    State s; for (const Key* k : {&o, &g0, &g1}) s.set_balance(k->addr, START);
    CHECK(s.apply(setg(o, {g0.addr, g1.addr}, 2, 0), 1).ok);
    CHECK(s.apply(reg(o, 1), 1).ok);
    const Hash32 id = asset_id(CHAIN_ID, o.addr, 1);
    CHECK(s.apply(vote(g0, o.addr, n.addr, 0), 2).ok); CHECK(s.apply(vote(g1, o.addr, n.addr, 0), 3).ok);
    const std::uint64_t ready = s.find(o.addr)->recovery.ready_height;
    s.begin_block(ready);
    CHECK(s.find_asset(id)->owner == n.addr);
    const auto r = s.apply(give(o, id, y.addr, 2), ready);
    CHECK(!r.ok && r.error == "only the owner may transfer this asset");
}

// t7: only the owner changes
static void t7_only_the_owner_changes() {
    Scene w;
    const AssetRecord was = *w.s.find_asset(w.id);
    const Hash32 root = w.s.state_root();
    CHECK(w.s.apply(give(w.a, w.id, w.b.addr, 1), 9).ok);             // a later height than the registration
    const AssetRecord* r = w.s.find_asset(w.id);
    CHECK(r && r->asset_kind == was.asset_kind && r->doc_hash == was.doc_hash && r->created_at == was.created_at && r->created_at == 1);
    CHECK(r && r->owner == w.b.addr && r->owner != was.owner);
    CHECK(w.s.assets().begin()->first == w.id);                       // the key (asset_id) is the same
    CHECK(w.s.state_root() != root);                                   // the registry is part of the state root
}

// t8: shape rules
static void t8_shape_rules() {
    Scene w;
    auto bad = [&](Transaction tx) { return !w.s.apply(tx, 2).ok; };
    auto good = [&] { return give(w.a, w.id, w.b.addr, 1); };
    { Transaction t = good(); t.to = Address{}; resign(t, w.a); CHECK(bad(t)); }                      // no receiver
    { Transaction t = good(); t.to = w.a.addr; resign(t, w.a); CHECK(bad(t)); }                       // to himself
    { Transaction t = good(); t.fee = 0; resign(t, w.a); CHECK(bad(t)); }                             // fee rule
    { Transaction t = good(); t.fee = F + 1; resign(t, w.a); CHECK(bad(t)); }
    { Transaction t = good(); t.amount = 1; resign(t, w.a); CHECK(bad(t)); }                          // money moves with a transfer, not here
    { Transaction t = good(); t.asset_kind = 1; resign(t, w.a); CHECK(bad(t)); }                      // registration-only fields
    { Transaction t = good(); t.doc_hash = doc(1); resign(t, w.a); CHECK(bad(t)); }
    { Transaction t = good(); t.new_owner = w.c.addr; resign(t, w.a); CHECK(bad(t)); }
    { Transaction t = good(); t.hold_blocks = 1; resign(t, w.a); CHECK(bad(t)); }
    { Transaction t = good(); t.owner = w.b.addr; resign(t, w.a); CHECK(bad(t)); }                    // owner must be the signer
    { Transaction t = transfer(w.a, w.b.addr, 5, 1); t.owner = w.a.addr; resign(t, w.a); CHECK(bad(t)); }   // `owner` still belongs to assets only
    { Transaction t = good(); t.ref = Hash32{}; resign(t, w.a); CHECK(bad(t)); }                      // no asset named
    {   // the sender cannot pay the fee
        State s2; s2.set_balance(w.a.addr, F); CHECK(s2.apply(reg(w.a, 0, 2, 9), 1).ok);   // the registration uses up the money
        const auto r = s2.apply(give(w.a, w.id, w.b.addr, 1), 2); CHECK(!r.ok && s2.find_asset(w.id)->owner == w.a.addr);
    }
    CHECK(w.s.find_asset(w.id)->owner == w.a.addr && w.s.nonce(w.a.addr) == 1);   // none of the above changed anything
    CHECK(w.s.apply(good(), 2).ok);                                                // control: the correct form is accepted
}

// t9: a chain with a transferred asset can be saved, loaded and replayed again
static void t9_save_and_load() {
    Key a = new_key(), b = new_key(), c = new_key();
    Chain ch; CHECK(ch.init_genesis(a.addr, START, b.addr, START));
    std::int64_t ts = 10;
    CHECK(ch.append_block({reg(a, 0, 3, 4), reg(a, 1, 1, 5)}, ts++).ok);
    const Hash32 id0 = asset_id(CHAIN_ID, a.addr, 0), id1 = asset_id(CHAIN_ID, a.addr, 1);
    CHECK(ch.append_block({give(a, id0, b.addr, 2)}, ts++).ok);
    CHECK(ch.append_block({give(b, id0, c.addr, 0)}, ts++).ok);
    CHECK(ch.state().find_asset(id0)->owner == c.addr && ch.state().find_asset(id1)->owner == a.addr);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "taz-asset-transfer.json";
    std::string err;
    CHECK(save_chain_json(ch, file.string(), &err));
    Chain back; LoadOptions pin; pin.require_genesis_pin = true; pin.expected_genesis_commitment = ch.genesis_commitment();
    const bool loaded = load_chain_json(file.string(), back, &err, pin);
    if (!loaded) std::cerr << "load failed: " << err << "\n";
    CHECK(loaded);
    if (loaded) {
        CHECK(back.height() == ch.height());
        CHECK(back.blocks().back().hash() == ch.blocks().back().hash());
        CHECK(back.state().assets() == ch.state().assets());
        CHECK(back.state().find_asset(id0)->owner == c.addr && back.state().find_asset(id1)->owner == a.addr);
        CHECK(back.state().state_root() == ch.state().state_root());
    }
    std::error_code ec; std::filesystem::remove(file, ec);
}

int main() {
    t1_owner_gives_the_asset_away();
    t2_a_stranger_cannot_transfer();
    t3_wrong_signature_is_refused();
    t4_unknown_asset_is_refused();
    t5_no_double_sale_in_one_block();
    t6_blocked_while_a_recovery_is_pending();
    t6b_after_the_recovery_the_old_key_has_nothing();
    t7_only_the_owner_changes();
    t8_shape_rules();
    t9_save_and_load();
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "asset transfer tests: all passed (10 cases)\n";
    return EXIT_SUCCESS;
}
