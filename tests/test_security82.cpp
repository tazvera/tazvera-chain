// Core v8.2 security tests: genesis commitment, guardian/veto lockout rule, recovery, restart (save/load mid-recovery), fuzzing.
// Every CHECK below is a real condition (no "|| true"). Not a security audit.
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include "taz/persist.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
using namespace taz;
namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(x) do { if(!(x)){ std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while(0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() { Key k; CHECK(generate_keypair(k.sk, k.pk)); k.addr = address_from_pubkey(k.pk); return k; }
static void resign(Transaction& tx, const Key& k) { CHECK(sign(k.sk, tx.signable_bytes(), tx.signature)); }
static Transaction base(const Key& f, std::uint64_t n) { Transaction tx; tx.from = f.addr; tx.nonce = n; tx.pubkey = f.pk; return tx; }
static Transaction transfer(const Key& f, const Address& to, Amount a, std::uint64_t n) { Transaction tx = base(f, n); tx.to = to; tx.amount = a; resign(tx, f); return tx; }
static Transaction setg(const Key& f, std::vector<Address> l, std::uint64_t thr, const Address& veto, std::uint64_t n) {
    std::sort(l.begin(), l.end()); Transaction tx = base(f, n); tx.kind = TxKind::SetGuardians; tx.guardians = l; tx.guardian_threshold = thr; tx.veto = veto; resign(tx, f); return tx;
}
static Transaction vote(const Key& g, const Address& who, const Address& to, std::uint64_t n) {
    Transaction tx = base(g, n); tx.kind = TxKind::RecoveryVote; tx.to = who; tx.new_owner = to; resign(tx, g); return tx;
}
static Transaction vcancel(const Key& v, const Address& who, std::uint64_t n) { Transaction tx = base(v, n); tx.kind = TxKind::RecoveryCancel; tx.to = who; resign(tx, v); return tx; }

static std::int64_t g_ts = 100;
static bool put(Chain& c, std::vector<Transaction> txs) { return c.append_block(txs, g_ts++).ok; }
static void empty_until(Chain& c, std::uint64_t h) { while (c.height() < h) CHECK(put(c, {})); }
static std::uint64_t nx(const Chain& c, const Key& k) { return c.state().nonce(k.addr); }
static std::string hexs(const std::uint8_t* p, std::size_t n) { static const char* d = "0123456789abcdef"; std::string s; for (std::size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; } return s; }
template <class A> static std::string hexs(const A& a) { return hexs(a.data(), a.size()); }
static bool replace_all(std::string& s, const std::string& from, const std::string& to) {
    bool any = false; for (std::size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) { s.replace(p, from.size(), to); any = true; } return any;
}
static bool same_chain(const Chain& a, const Chain& b) {
    if (a.blocks().size() != b.blocks().size()) return false;
    for (std::size_t i = 0; i < a.blocks().size(); ++i) if (a.blocks()[i].hash() != b.blocks()[i].hash()) return false;
    return a.state().state_root() == b.state().state_root() && to_json(a) == to_json(b);
}
static bool conserved(const Chain& c, Amount total) { return c.state().circulating() + c.state().held_total() + c.state().burned_total() == total; }

static constexpr Amount BIG = 100000000, FUND = 2000000;
struct World {                                   // o = owner (possibly stolen), g = guardians, n = new key, x = thief's veto key
    Key o = new_key(), g[3] = {new_key(), new_key(), new_key()}, n = new_key(), x = new_key();
    Chain c; Amount total = BIG;
    World() {
        CHECK(c.init_genesis(o.addr, BIG, n.addr, 0));       // n has a ZERO genesis balance
        CHECK(put(c, {transfer(o, g[0].addr, FUND, 0), transfer(o, g[1].addr, FUND, 1), transfer(o, g[2].addr, FUND, 2), transfer(o, x.addr, FUND, 3)}));
        CHECK(put(c, {setg(o, {g[0].addr, g[1].addr, g[2].addr}, 2, Address{}, 4)}));
    }
};

// ---- A. genesis -------------------------------------------------------------------------------------------------------
static void test_genesis() {
    World w;
    const Hash32 gc = w.c.genesis_commitment();
    CHECK(w.c.blocks()[0].previous_hash == gc);                      // bound INTO block 0 (so into its hash and every later block)
    CHECK(gc != Hash32{});
    // order of the two genesis accounts does not matter; every address and balance does
    CHECK(genesis_commitment_of(w.o.addr, BIG, w.n.addr, 0) == genesis_commitment_of(w.n.addr, 0, w.o.addr, BIG));
    Key z = new_key();
    CHECK(genesis_commitment_of(w.o.addr, BIG, z.addr, 0) != gc);          // zero-balance address is covered
    CHECK(genesis_commitment_of(w.o.addr, BIG, w.n.addr, 1) != gc);        // balance is covered
    CHECK(genesis_commitment_of(w.o.addr, BIG - 1, w.n.addr, 0) != gc);
    { Chain a, b; CHECK(a.init_genesis(w.o.addr, BIG, w.n.addr, 0)); CHECK(b.init_genesis(w.o.addr, BIG, z.addr, 0));
      CHECK(a.blocks()[0].hash() != b.blocks()[0].hash()); }                // two chains differing only in a zero-balance address get different block 0
    // attack: swap the zero-balance address and fix up the commitment field; block 0 / block 1 must refuse
    const std::string text = to_json(w.c);
    const std::string oldc = hexs(gc), newc = hexs(genesis_commitment_of(w.o.addr, BIG, z.addr, 0));
    { std::string t = text; CHECK(replace_all(t, hexs(w.n.addr), hexs(z.addr)));
      Chain out; std::string err; CHECK(!load_chain_json_text(t, out, &err)); }                        // address changed only
    { std::string t = text; replace_all(t, hexs(w.n.addr), hexs(z.addr)); CHECK(replace_all(t, oldc, newc));
      Chain out; std::string err; CHECK(!load_chain_json_text(t, out, &err)); CHECK(err.find("block") != std::string::npos); } // + commitment AND block 0 previous_hash fixed
    { std::string t = text; replace_all(t, oldc, std::string(64, '0'));                                // zeroed commitment (old layout)
      Chain out; std::string err; CHECK(!load_chain_json_text(t, out, &err)); }
    // a one-block chain (genesis only) is the limit: the commitment must be compared out of band (README)
    { Chain g; CHECK(g.init_genesis(w.o.addr, BIG, w.n.addr, 0)); Chain out; std::string err; CHECK(load_chain_json_text(to_json(g), out, &err)); CHECK(same_chain(out, g)); }
    // a normal round trip still works
    { Chain out; std::string err; CHECK(load_chain_json_text(text, out, &err)); CHECK(same_chain(out, w.c)); }
}

// ---- B. guardian / veto lockout rule ------------------------------------------------------------------------------------
static void test_pending_veto_dropped_when_recovery_agreed() {
    World w; Chain& c = w.c;
    CHECK(put(c, {setg(w.o, {w.g[0].addr, w.g[1].addr, w.g[2].addr}, 2, w.x.addr, nx(c, w.o))}));      // the thief schedules his veto
    const auto swap_ready = c.state().find(w.o.addr)->pending_guardians.ready_height;
    CHECK(c.state().find(w.o.addr)->pending_guardians.active);
    empty_until(c, swap_ready - 200);                                                                    // guardians wake up late, but before the swap
    CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0])), vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
    const auto rec_ready = c.state().find(w.o.addr)->recovery.ready_height;
    CHECK(c.state().find(w.o.addr)->recovery.active && rec_ready > swap_ready);                           // the veto would have come first
    empty_until(c, swap_ready + 1);
    CHECK(!c.state().find(w.o.addr)->pending_guardians.active);                                           // discarded, not applied
    CHECK(c.state().find(w.o.addr)->guardians.veto == Address{});                                         // the thief's veto never became live
    CHECK(!put(c, {vcancel(w.x, w.o.addr, nx(c, w.x))}));                                                 // he cannot cancel or postpone
    CHECK(c.state().find(w.o.addr)->recovery.active && !c.state().find(w.o.addr)->recovery.veto_postponed);
    empty_until(c, rec_ready);
    CHECK(c.state().balance(w.n.addr) > 0 && c.state().balance(w.o.addr) == 0);                           // funds reached the new owner
    CHECK(conserved(c, w.total));
}
static void test_veto_postpones_only_once() {
    World w; Chain& c = w.c;
    CHECK(put(c, {setg(w.o, {w.g[0].addr, w.g[1].addr, w.g[2].addr}, 2, w.x.addr, nx(c, w.o))}));
    empty_until(c, c.state().find(w.o.addr)->pending_guardians.ready_height);
    CHECK(c.state().find(w.o.addr)->guardians.veto == w.x.addr);                                          // veto is live (guardians were asleep)
    CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0])), vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
    const auto r1 = c.state().find(w.o.addr)->recovery.ready_height;
    CHECK(put(c, {vcancel(w.x, w.o.addr, nx(c, w.x))}));
    const auto r2 = c.state().find(w.o.addr)->recovery.ready_height;
    CHECK(r2 == r1 + VETO_POSTPONE_BLOCKS);                                                               // exact amount
    CHECK(!put(c, {vcancel(w.x, w.o.addr, nx(c, w.x))}));                                                 // a second veto is refused
    CHECK(!put(c, {setg(w.o, {w.g[0].addr}, 1, Address{}, nx(c, w.o))}));                                 // guardians cannot be swapped during a recovery
    empty_until(c, r2 - 1); CHECK(c.state().balance(w.n.addr) == 0);                                      // not one block early
    empty_until(c, r2);     CHECK(c.state().balance(w.n.addr) > 0);
    CHECK(conserved(c, w.total));
}
static void test_stranger_cannot_veto_or_vote() {
    World w; Chain& c = w.c; Key s = new_key();
    CHECK(put(c, {transfer(w.o, s.addr, FUND, nx(c, w.o))}));
    CHECK(!put(c, {vote(s, w.o.addr, s.addr, nx(c, s))}));                                                // not a guardian
    CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0])), vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
    CHECK(!put(c, {vcancel(s, w.o.addr, nx(c, s))}));                                                     // not the veto key (none set); not the owner
    CHECK(c.state().find(w.o.addr)->recovery.active);
}

// ---- v9.3: a guardian's vote expires -----------------------------------------------------------------------------------
static void test_vote_expires() {
    // The last block in which the first vote still counts: another guardian's matching vote completes the recovery.
    { World w; Chain& c = w.c;
      CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0]))}));
      const auto cast = c.height();
      CHECK(c.state().find(w.o.addr)->votes.size() == 1 && c.state().find(w.o.addr)->votes.begin()->second.height == cast);
      empty_until(c, cast + VOTE_EXPIRY_BLOCKS - 1);
      CHECK(put(c, {vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
      CHECK(c.height() == cast + VOTE_EXPIRY_BLOCKS);
      CHECK(c.state().find(w.o.addr)->recovery.active); }
    // One block later the first vote is gone: the second vote stands alone and nothing is agreed.
    { World w; Chain& c = w.c;
      CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0]))}));
      const auto cast = c.height();
      empty_until(c, cast + VOTE_EXPIRY_BLOCKS);
      CHECK(put(c, {vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
      CHECK(c.height() == cast + VOTE_EXPIRY_BLOCKS + 1);
      const Account* a = c.state().find(w.o.addr);
      CHECK(!a->recovery.active);
      CHECK(a->votes.size() == 1 && a->votes.begin()->first == w.g[1].addr);                  // only the fresh vote is stored
      CHECK(put(c, {vote(w.g[2], w.o.addr, w.n.addr, nx(c, w.g[2]))}));                       // two fresh votes still do agree
      CHECK(c.state().find(w.o.addr)->recovery.active);
      CHECK(conserved(c, w.total)); }
    // An expired vote with nobody voting afterwards disappears from the state; the veto key then has nothing to clear.
    { World w; Chain& c = w.c;
      CHECK(put(c, {setg(w.o, {w.g[0].addr, w.g[1].addr, w.g[2].addr}, 2, Address{}, nx(c, w.o))}));   // same list: no-op change
      CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0]))}));
      empty_until(c, c.height() + VOTE_EXPIRY_BLOCKS + 1);
      CHECK(c.state().find(w.o.addr)->votes.empty()); }
}
static void test_vote_height_is_in_the_root() {
    // Same keys, same votes, same final height; only the block the vote was cast in differs. The roots must differ.
    World w; Chain a = w.c, b = w.c;
    CHECK(put(a, {})); CHECK(put(a, {vote(w.g[0], w.o.addr, w.n.addr, nx(a, w.g[0]))}));
    CHECK(put(b, {vote(w.g[0], w.o.addr, w.n.addr, nx(b, w.g[0]))})); CHECK(put(b, {}));
    CHECK(a.height() == b.height());
    CHECK(a.state().state_root() != b.state().state_root());
}

// ---- C. restart: save / load in the middle of a recovery ----------------------------------------------------------------
static void test_restart_mid_recovery() {
    World w; Chain& c = w.c;
    CHECK(put(c, {setg(w.o, {w.g[0].addr, w.g[1].addr, w.g[2].addr}, 2, w.x.addr, nx(c, w.o))}));
    empty_until(c, c.state().find(w.o.addr)->pending_guardians.ready_height);
    CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0])), vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
    CHECK(put(c, {vcancel(w.x, w.o.addr, nx(c, w.x))}));                                                  // veto_postponed = true is in state now
    fs::path d = fs::temp_directory_path() / ("taz82_" + std::to_string(std::rand())); fs::create_directories(d);
    const std::string p = (d / "chain.json").string(); std::string err;
    CHECK(save_chain_json(c, p, &err));
    Chain r; CHECK(load_chain_json(p, r, &err)); CHECK(same_chain(r, c));
    CHECK(r.state().find(w.o.addr)->recovery.veto_postponed);                                             // survives the restart
    CHECK(!put(r, {vcancel(w.x, w.o.addr, nx(r, w.x))}));                                                 // a restart cannot give the thief a second veto
    const auto ready = r.state().find(w.o.addr)->recovery.ready_height;
    Chain a = c, b = r;                                                                                   // continue both: identical roots all the way
    while (a.height() < ready) { CHECK(a.append_block({}, g_ts).ok); CHECK(b.append_block({}, g_ts).ok); ++g_ts; CHECK(a.state().state_root() == b.state().state_root()); }
    CHECK(a.state().balance(w.n.addr) > 0 && b.state().balance(w.n.addr) == a.state().balance(w.n.addr));
    // a failed save (crash before rename) leaves the old good file
    testing::persist_stop_before_rename = true; CHECK(!save_chain_json(a, p, &err)); testing::persist_stop_before_rename = false;
    { Chain l; CHECK(load_chain_json(p, l, &err)); CHECK(same_chain(l, c)); }
    fs::remove_all(d);
}

// ---- D. fuzzing (deterministic seed; run again under ASan/UBSan in CI) --------------------------------------------------
static std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
static std::uint64_t rnd(std::uint64_t n) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return (seed >> 33) % n; }
static void test_fuzz_file() {
    World w; Chain& c = w.c;
    CHECK(put(c, {setg(w.o, {w.g[0].addr, w.g[1].addr, w.g[2].addr}, 2, w.x.addr, nx(c, w.o))}));
    empty_until(c, c.state().find(w.o.addr)->pending_guardians.ready_height);
    CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0])), vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
    const std::string text = to_json(c);
    int accepted = 0, refused = 0;
    for (int i = 0; i < 6000; ++i) {
        std::string m = text; const std::size_t pos = rnd(m.size());
        switch (rnd(5)) {
            case 0: m[pos] = static_cast<char>(rnd(256)); break;
            case 1: m.erase(pos, 1 + rnd(20)); break;
            case 2: m.insert(pos, std::string(1 + rnd(8), static_cast<char>(rnd(256)))); break;
            case 3: m.resize(pos); break;
            default: { const std::size_t q = rnd(m.size()); std::swap(m[pos], m[q]); }
        }
        Chain out; std::string err;
        if (load_chain_json_text(m, out, &err)) { ++accepted; CHECK(out.state().state_root() == c.state().state_root() && out.blocks().size() == c.blocks().size() && out.blocks().back().hash() == c.blocks().back().hash()); }
        else { ++refused; CHECK(!err.empty()); }
    }
    CHECK(refused > 5000);
    std::cout << "fuzz file: " << refused << " refused, " << accepted << " accepted (same ledger only)\n";
}
static void test_fuzz_transactions() {
    World w; const Chain baseline = w.c; const Key ks[] = {w.o, w.g[0], w.g[1], w.g[2], w.n, w.x};
    int ok = 0, bad = 0;
    // Keep every fuzz case on the same short baseline. Appending all 4,000 cases to one
    // ever-growing chain made validation work grow quadratically under ASan/UBSan.
    // Independent trials preserve 4,000 randomized block-validation cases without that
    // unrelated chain-growth cost.
    for (int i = 0; i < 4000; ++i) {
        Chain trial = baseline;
        std::vector<Transaction> txs;
        for (std::uint64_t t = rnd(4); t > 0; --t) {
            const Key& f = ks[rnd(6)]; const Key& o = ks[rnd(6)];
            Transaction tx = base(f, rnd(4) == 0 ? rnd(50) : nx(baseline, f));
            tx.kind = static_cast<TxKind>(rnd(6)); tx.to = rnd(2) ? o.addr : Address{}; tx.amount = rnd(3) ? rnd(3000000) : UINT64_MAX - rnd(3);
            tx.new_owner = o.addr; tx.veto = rnd(2) ? ks[rnd(6)].addr : Address{}; tx.guardian_threshold = rnd(5);
            tx.guardians = {ks[rnd(6)].addr, ks[rnd(6)].addr}; tx.daily_limit = rnd(5000000); tx.hold_threshold = rnd(5000000); tx.hold_blocks = rnd(20);
            resign(tx, f); if (rnd(10) == 0) tx.signature[rnd(tx.signature.size())] ^= static_cast<std::uint8_t>(1 + rnd(255));  // some with a broken signature
            txs.push_back(tx);
        }
        const auto before = trial.state().state_root();
        if (trial.append_block(txs, g_ts++).ok) ++ok; else { ++bad; CHECK(trial.state().state_root() == before); } // failed block changes nothing
        CHECK(conserved(trial, w.total));
    }
    CHECK(ok > 100 && bad > 100);
    std::cout << "fuzz transactions: " << ok << " blocks accepted, " << bad << " refused, supply conserved\n";
}

// v8.3: the file's own commitment proves internal consistency only. A pin supplied by the caller is what
// distinguishes "a chain" from "the chain I meant to join".
static void test_genesis_pin() {
    Key a = new_key(), b = new_key(), d = new_key();
    Chain c; CHECK(c.init_genesis(a.addr, BIG, b.addr, 0)); CHECK(put(c, {transfer(a, b.addr, 1000, 0)}));
    const std::string text = to_json(c);
    { LoadOptions o; o.expected_genesis_commitment = c.genesis_commitment(); Chain out; CHECK(load_chain_json_text(text, out, nullptr, o)); }
    { LoadOptions o; o.expected_genesis_commitment = Hash32{1}; Chain out; CHECK(!load_chain_json_text(text, out, nullptr, o)); }
    // an internally consistent chain with a different genesis is refused by the pin, accepted without it
    Chain other; CHECK(other.init_genesis(a.addr, BIG, d.addr, 0)); CHECK(put(other, {transfer(a, d.addr, 1000, 0)}));
    const std::string other_text = to_json(other);
    CHECK(other.genesis_commitment() != c.genesis_commitment());
    { Chain out; CHECK(load_chain_json_text(other_text, out, nullptr)); }
    { LoadOptions o; o.expected_genesis_commitment = c.genesis_commitment(); Chain out; CHECK(!load_chain_json_text(other_text, out, nullptr, o)); }
}

// v8.3: pins a documented limitation, so a future change to it is a deliberate decision and not an
// accident. With guardians and NO veto key, the account's own key may cancel a recovery — and nothing
// stops it from doing so again and again. v8.2's R2 rule only limits the VETO key (postpone once); it does
// not touch this path. The mitigation is to set a veto key with the guardians (v7). If someone later makes
// the owner-cancel path one-shot as well, this test must be updated on purpose.
static void test_owner_key_can_cancel_without_veto() {
    World w; Chain& c = w.c;
    for (int round = 0; round < 3; ++round) {
        CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0])), vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
        const Account* oa = c.state().find(w.o.addr);
        CHECK(oa && oa->recovery.active);
        CHECK(oa && oa->guardians.veto == Address{});            // the World account has no veto key
        Transaction cancel = base(w.o, nx(c, w.o));              // to = zero: the account cancels its own recovery
        cancel.kind = TxKind::RecoveryCancel; resign(cancel, w.o);
        CHECK(put(c, {cancel}));
        const Account* after = c.state().find(w.o.addr);
        CHECK(after && !after->recovery.active && after->votes.empty());
        CHECK(c.state().balance(w.n.addr) == 0);                 // nothing moved
    }
    conserved(c, w.total);
}

int main() {
    test_genesis(); test_pending_veto_dropped_when_recovery_agreed(); test_veto_postpones_only_once(); test_stranger_cannot_veto_or_vote();
    test_vote_expires(); test_vote_height_is_in_the_root();
    test_restart_mid_recovery(); test_genesis_pin(); test_owner_key_can_cancel_without_veto(); test_fuzz_file(); test_fuzz_transactions();
    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "Security v8.2 tests passed. This is not a security audit.\n"; return EXIT_SUCCESS;
}
