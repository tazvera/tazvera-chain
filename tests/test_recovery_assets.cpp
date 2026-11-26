// RWA registry, part 1.5: a recovery carries the assets as well as the balance.
//
// When an agreed recovery comes due, EVERY asset of the old address gets the new address as its owner, in the same step
// as the balance. Nothing is removed from the registry, nobody else's asset is touched. If the new address cannot hold
// the funds, nothing moves at all (balance and assets stay together). See OPEN-DECISIONS.md.
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/persist.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>

using namespace taz;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() { Key k; if (!generate_keypair(k.sk, k.pk)) std::exit(2); k.addr = address_from_pubkey(k.pk); return k; }
static void resign(Transaction& tx, const Key& k) { CHECK(sign(k.sk, tx.signable_bytes(), tx.signature)); }
static Transaction base(const Key& f, std::uint64_t nonce) { Transaction tx; tx.from = f.addr; tx.pubkey = f.pk; tx.nonce = nonce; return tx; }
static Transaction transfer(const Key& f, const Address& to, Amount a, std::uint64_t n) { Transaction tx = base(f, n); tx.to = to; tx.amount = a; resign(tx, f); return tx; }
static Transaction setg(const Key& f, std::vector<Address> list, std::uint64_t thr, std::uint64_t n) {
    std::sort(list.begin(), list.end()); Transaction tx = base(f, n); tx.kind = TxKind::SetGuardians; tx.guardians = std::move(list); tx.guardian_threshold = thr; resign(tx, f); return tx;
}
static Transaction vote(const Key& g, const Address& who, const Address& new_owner, std::uint64_t n) {
    Transaction tx = base(g, n); tx.kind = TxKind::RecoveryVote; tx.to = who; tx.new_owner = new_owner; resign(tx, g); return tx;
}
static Hash32 doc(std::uint8_t seed) { Hash32 h{}; for (std::size_t i = 0; i < h.size(); ++i) h[i] = static_cast<std::uint8_t>(seed + i); return h; }
static Transaction reg(const Key& who, std::uint64_t nonce, std::uint8_t kind, std::uint8_t seed) {
    Transaction tx = base(who, nonce); tx.kind = TxKind::RegisterAsset; tx.owner = who.addr; tx.asset_kind = kind; tx.doc_hash = doc(seed); resign(tx, who); return tx;
}
static constexpr Amount F = FEE_TRANSFER_V1;
static constexpr Amount BIG = 100000000;

// How many assets does this address own?
static std::size_t owned_by(const State& s, const Address& a) {
    std::size_t n = 0; for (const auto& [id, rec] : s.assets()) if (rec.owner == a) ++n; return n;
}

// A State where o (key lost) owns three assets of different kinds, x (a stranger) owns two, guardians g0,g1 agreed on n.
struct Scene {
    Key o = new_key(), g0 = new_key(), g1 = new_key(), n = new_key(), x = new_key();
    State s;
    Hash32 oid[3]{}, xid[2]{};
    std::uint64_t ready{0};
    Scene(Amount o_balance, Amount n_balance) {
        s.set_balance(o.addr, o_balance); s.set_balance(g0.addr, 100000); s.set_balance(g1.addr, 100000); s.set_balance(x.addr, 100000);
        if (n_balance) s.set_balance(n.addr, n_balance);
        CHECK(s.apply(setg(o, {g0.addr, g1.addr}, 2, 0), 1).ok);
        for (std::uint64_t i = 0; i < 3; ++i) {
            CHECK(s.apply(reg(o, 1 + i, static_cast<std::uint8_t>(1 + i), static_cast<std::uint8_t>(10 + i)), 1).ok);
            oid[i] = asset_id(CHAIN_ID, o.addr, 1 + i);
        }
        for (std::uint64_t i = 0; i < 2; ++i) {
            CHECK(s.apply(reg(x, i, 4, static_cast<std::uint8_t>(50 + i)), 1).ok);
            xid[i] = asset_id(CHAIN_ID, x.addr, i);
        }
        CHECK(s.apply(vote(g0, o.addr, n.addr, 0), 2).ok);
        CHECK(s.apply(vote(g1, o.addr, n.addr, 0), 3).ok);
        ready = 3 + RECOVERY_DELAY_BLOCKS;
    }
};

// t1: three assets, one recovery: all three go to the new address, the registry keeps its size, others are untouched
static void t1_recovery_moves_all_assets() {
    Scene w(BIG, 0);
    CHECK(w.s.asset_count() == 5 && owned_by(w.s, w.o.addr) == 3 && owned_by(w.s, w.x.addr) == 2);
    const auto before = w.s.assets();
    const Hash32 root_before = w.s.state_root();
    CHECK(w.s.begin_block(w.ready - 1).recoveries.empty());          // not due yet: nothing moves
    CHECK(w.s.assets() == before);
    const auto ev = w.s.begin_block(w.ready);
    CHECK(ev.recoveries.size() == 1 && ev.recoveries[0].moved);
    for (const Hash32& id : w.oid) {
        const AssetRecord* r = w.s.find_asset(id);
        CHECK(r && r->owner == w.n.addr);                            // the new owner
        const AssetRecord& was = before.at(id);
        CHECK(r && r->asset_kind == was.asset_kind && r->doc_hash == was.doc_hash && r->created_at == was.created_at);   // only the owner changed
    }
    CHECK(w.s.asset_count() == 5);                                    // nothing removed, nothing added
    CHECK(owned_by(w.s, w.o.addr) == 0);                              // none left under the old address
    CHECK(owned_by(w.s, w.n.addr) == 3);
    for (const Hash32& id : w.xid) CHECK(w.s.find_asset(id) && *w.s.find_asset(id) == before.at(id));   // somebody else's: exactly as before
    CHECK(w.s.state_root() != root_before);
    CHECK(ev.recoveries[0].asset_count == 3);                         // only the old address's assets are counted
}

// t2: an account without assets: the event says 0 and the registry is untouched
static void t2_account_without_assets() {
    Key o = new_key(), g0 = new_key(), g1 = new_key(), n = new_key(), x = new_key();
    State s; s.set_balance(o.addr, BIG); s.set_balance(g0.addr, 100000); s.set_balance(g1.addr, 100000); s.set_balance(x.addr, 100000);
    CHECK(s.apply(setg(o, {g0.addr, g1.addr}, 2, 0), 1).ok);
    CHECK(s.apply(reg(x, 0, 1, 7), 1).ok);
    CHECK(s.apply(vote(g0, o.addr, n.addr, 0), 2).ok); CHECK(s.apply(vote(g1, o.addr, n.addr, 0), 3).ok);
    const auto before = s.assets();
    const auto ev = s.begin_block(3 + RECOVERY_DELAY_BLOCKS);
    CHECK(ev.recoveries.size() == 1 && ev.recoveries[0].moved && ev.recoveries[0].asset_count == 0);
    CHECK(s.assets() == before);
}

// t3: the new address cannot hold the funds: nothing moves, so the assets stay with the old address too (one step, not half)
static void t3_nothing_moves_when_the_balance_cannot() {
    constexpr Amount MAXA = std::numeric_limits<Amount>::max();
    Scene w(BIG, MAXA - 5);
    const auto before = w.s.assets();
    const auto ev = w.s.begin_block(w.ready);
    CHECK(ev.recoveries.size() == 1 && !ev.recoveries[0].moved);
    CHECK(ev.recoveries[0].asset_count == 0);
    CHECK(w.s.assets() == before);
    CHECK(owned_by(w.s, w.o.addr) == 3 && owned_by(w.s, w.n.addr) == 0);
}

// t4: an account that holds assets but no money is still carried over (the balance is 0, the assets are the point)
static void t4_assets_move_even_when_the_balance_is_zero() {
    Scene w(4 * F, 0);                                                // exactly the fees of setg + three registrations
    CHECK(w.s.balance(w.o.addr) == 0);
    const auto ev = w.s.begin_block(w.ready);
    CHECK(ev.recoveries.size() == 1 && ev.recoveries[0].moved && ev.recoveries[0].amount == 0 && ev.recoveries[0].asset_count == 3);
    CHECK(owned_by(w.s, w.o.addr) == 0 && owned_by(w.s, w.n.addr) == 3 && w.s.asset_count() == 5);
}

// t5: through a real chain, then saved and loaded again: same registry, same state root, same tip
static void t5_chain_save_load_after_recovery() {
    Key o = new_key(), g0 = new_key(), g1 = new_key(), n = new_key(), x = new_key();
    Chain c; CHECK(c.init_genesis(o.addr, BIG, g0.addr, 5000000));
    std::int64_t ts = 10;
    auto put = [&](std::vector<Transaction> txs) { return c.append_block(txs, ts++).ok; };
    CHECK(put({transfer(o, g1.addr, 2000000, 0), transfer(o, x.addr, 2000000, 1)}));
    CHECK(put({setg(o, {g0.addr, g1.addr}, 2, 2), reg(o, 3, 1, 21), reg(o, 4, 2, 22), reg(o, 5, 3, 23), reg(x, 0, 4, 24)}));
    CHECK(c.state().asset_count() == 4);
    CHECK(put({vote(g0, o.addr, n.addr, 0), vote(g1, o.addr, n.addr, 0)}));
    const std::uint64_t ready = c.state().find(o.addr)->recovery.ready_height;
    while (c.height() < ready - 1) CHECK(put({}));
    CHECK(owned_by(c.state(), o.addr) == 3);                          // still the old owner one block before
    CHECK(put({}));                                                    // the block at which it comes due
    CHECK(c.height() == ready);
    CHECK(owned_by(c.state(), o.addr) == 0 && owned_by(c.state(), n.addr) == 3 && owned_by(c.state(), x.addr) == 1);
    CHECK(c.state().asset_count() == 4);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "taz-recovery-assets.json";
    std::string err;
    CHECK(save_chain_json(c, file.string(), &err));
    Chain back; LoadOptions pin; pin.require_genesis_pin = true; pin.expected_genesis_commitment = c.genesis_commitment();
    const bool loaded = load_chain_json(file.string(), back, &err, pin);
    if (!loaded) std::cerr << "load failed: " << err << "\n";
    CHECK(loaded);
    if (loaded) {
        CHECK(back.height() == c.height());
        CHECK(back.blocks().back().hash() == c.blocks().back().hash());
        CHECK(back.state().assets() == c.state().assets());
        CHECK(owned_by(back.state(), n.addr) == 3 && owned_by(back.state(), o.addr) == 0);
        CHECK(back.state().state_root() == c.state().state_root());
    }
    std::error_code ec; std::filesystem::remove(file, ec);
}

int main() {
    t1_recovery_moves_all_assets();
    t2_account_without_assets();
    t3_nothing_moves_when_the_balance_cannot();
    t4_assets_move_even_when_the_balance_is_zero();
    t5_chain_save_load_after_recovery();
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "recovery assets tests: all passed (5 cases)\n";
    return EXIT_SUCCESS;
}
