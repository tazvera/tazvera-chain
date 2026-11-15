#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
using namespace taz;
static int failures = 0;
#define CHECK(x) do { if(!(x)){ std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while(0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() { Key k; CHECK(generate_keypair(k.sk, k.pk)); k.addr = address_from_pubkey(k.pk); return k; }
static void resign(Transaction& tx, const Key& k) { CHECK(sign(k.sk, tx.signable_bytes(), tx.signature)); }
static Transaction base(const Key& f, std::uint64_t nonce) { Transaction tx; tx.from = f.addr; tx.nonce = nonce; tx.pubkey = f.pk; return tx; }
static Transaction transfer(const Key& f, const Address& to, Amount a, std::uint64_t n) {
    Transaction tx = base(f, n); tx.to = to; tx.amount = a; resign(tx, f); return tx;
}
static Transaction safety(const Key& f, Amount limit, Amount thr, std::uint64_t hold, std::uint64_t n) {
    Transaction tx = base(f, n); tx.kind = TxKind::SetSafety; tx.daily_limit = limit; tx.hold_threshold = thr; tx.hold_blocks = hold; resign(tx, f); return tx;
}
static Transaction cancel_held(const Key& f, const Hash32& ref, std::uint64_t n) {
    Transaction tx = base(f, n); tx.kind = TxKind::CancelHeld; tx.ref = ref; resign(tx, f); return tx;
}
static Transaction setg_raw(const Key& f, std::vector<Address> list, std::uint64_t thr, std::uint64_t n) {
    Transaction tx = base(f, n); tx.kind = TxKind::SetGuardians; tx.guardians = std::move(list); tx.guardian_threshold = thr; resign(tx, f); return tx;
}
static Transaction setg(const Key& f, std::vector<Address> list, std::uint64_t thr, std::uint64_t n) {
    std::sort(list.begin(), list.end()); return setg_raw(f, std::move(list), thr, n);
}
static Transaction vote(const Key& g, const Address& who, const Address& new_owner, std::uint64_t n) {
    Transaction tx = base(g, n); tx.kind = TxKind::RecoveryVote; tx.to = who; tx.new_owner = new_owner; resign(tx, g); return tx;
}
static Transaction rcancel(const Key& f, std::uint64_t n) {
    Transaction tx = base(f, n); tx.kind = TxKind::RecoveryCancel; resign(tx, f); return tx;
}
static std::int64_t g_ts = 10;
static bool put(Chain& c, std::vector<Transaction> txs) { return c.append_block(txs, g_ts++).ok; }
static void empty_until(Chain& c, std::uint64_t height) { while (c.height() < height) CHECK(put(c, {})); }
static std::uint64_t nx(const Chain& c, const Key& k) { return c.state().nonce(k.addr); }
static void conserved(const Chain& c, Amount total) {
    CHECK(c.state().circulating() + c.state().held_total() + c.state().burned_total() == total);
}
static constexpr Amount F = FEE_TRANSFER_V1;
static constexpr Amount BIG = 100000000;
static constexpr Amount FUND = 2000000;
static constexpr Amount GENESIS_G0 = 5000000;

// o = the account whose key is lost, g[0..2] = trusted people, n = the new key, x = a stranger
struct World {
    Key o = new_key(), g[3] = {new_key(), new_key(), new_key()}, n = new_key(), x = new_key(), y = new_key();
    Chain c;
    Amount total = BIG + GENESIS_G0;
    explicit World(std::uint64_t threshold, bool with_guardians = true) {
        CHECK(c.init_genesis(o.addr, BIG, g[0].addr, GENESIS_G0));
        CHECK(put(c, {transfer(o, g[1].addr, FUND, 0), transfer(o, g[2].addr, FUND, 1), transfer(o, x.addr, FUND, 2), transfer(o, y.addr, FUND, 3)}));
        if (with_guardians) CHECK(put(c, {setg(o, {g[0].addr, g[1].addr, g[2].addr}, threshold, 4)}));
    }
};

static void test_shape_rules() {
    World w(2);
    Chain& c = w.c; const Key &o = w.o, &g0 = w.g[0];
    auto bad = [&](Transaction tx, const Key& signer) { resign(tx, signer); return !c.append_block({tx}, g_ts++).ok; };
    const std::uint64_t on = nx(c, o); // = 5
    std::vector<Address> six; for (std::uint8_t i = 1; i <= 6; ++i) { Address a{}; a[0] = i; six.push_back(a); }
    Address a1{}, a2{}, a3{}; a1[0] = 1; a2[0] = 2; a3[0] = 3;
    CHECK(bad(setg_raw(o, {a2, a1}, 1, on), o));                    // not sorted
    CHECK(bad(setg_raw(o, {a1, a1}, 1, on), o));                    // duplicate
    CHECK(bad(setg_raw(o, {a1, o.addr}, 1, on), o));                // own address (sorted or not, never allowed)
    CHECK(bad(setg_raw(o, {Address{}, a1}, 1, on), o));             // empty address
    CHECK(bad(setg_raw(o, six, 3, on), o));                         // more than MAX_GUARDIANS
    CHECK(bad(setg_raw(o, {a1, a2}, 0, on), o));                    // threshold zero
    CHECK(bad(setg_raw(o, {a1, a2}, 3, on), o));                    // threshold above the number of guardians
    CHECK(bad(setg_raw(o, {}, 1, on), o));                          // threshold without guardians
    { auto t = setg_raw(o, {a1, a2}, 1, on); t.amount = 5; CHECK(bad(t, o)); }
    { auto t = setg_raw(o, {a1, a2}, 1, on); t.to = a3; CHECK(bad(t, o)); }
    { auto t = setg_raw(o, {a1, a2}, 1, on); t.new_owner = a3; CHECK(bad(t, o)); }
    { auto t = setg_raw(o, {a1, a2}, 1, on); t.ref[0] = 1; CHECK(bad(t, o)); }
    { auto t = setg_raw(o, {a1, a2}, 1, on); t.hold_blocks = 1; CHECK(bad(t, o)); }
    { auto t = setg_raw(o, {a1, a2}, 1, on); t.fee = 0; CHECK(bad(t, o)); }
    { auto t = setg_raw(o, {a1, a2}, 1, on); t.guardian_threshold = 2; CHECK(!c.append_block({t}, g_ts++).ok); } // signed for 1, changed to 2
    CHECK(bad(vote(g0, Address{}, w.n.addr, nx(c, g0)), g0));       // no account named
    CHECK(bad(vote(g0, g0.addr, w.n.addr, nx(c, g0)), g0));         // own account
    CHECK(bad(vote(g0, o.addr, Address{}, nx(c, g0)), g0));         // no new address
    CHECK(bad(vote(g0, o.addr, o.addr, nx(c, g0)), g0));            // new address equals old
    { auto t = vote(g0, o.addr, w.n.addr, nx(c, g0)); t.amount = 1; CHECK(bad(t, g0)); }
    { auto t = vote(g0, o.addr, w.n.addr, nx(c, g0)); t.guardians = {a1}; t.guardian_threshold = 1; CHECK(bad(t, g0)); }
    { auto t = vote(g0, o.addr, w.n.addr, nx(c, g0)); t.ref[1] = 1; CHECK(bad(t, g0)); }
    { auto t = vote(g0, o.addr, w.n.addr, nx(c, g0)); t.daily_limit = 1; CHECK(bad(t, g0)); }
    { auto t = rcancel(o, on); t.new_owner = a1; CHECK(bad(t, o)); }
    { auto t = rcancel(o, on); t.to = a1; CHECK(bad(t, o)); }
    { auto t = rcancel(o, on); t.guardians = {a1}; t.guardian_threshold = 1; CHECK(bad(t, o)); }
    { auto t = transfer(o, w.x.addr, 5, on); t.new_owner = a1; CHECK(bad(t, o)); }          // guardian fields inside old kinds
    { auto t = transfer(o, w.x.addr, 5, on); t.guardians = {a1}; CHECK(bad(t, o)); }
    { auto t = safety(o, 0, 0, 0, on); t.guardian_threshold = 1; CHECK(bad(t, o)); }
    { auto t = cancel_held(o, Hash32{}, on); t.new_owner = a1; CHECK(bad(t, o)); }
    CHECK(nx(c, o) == on); // nothing above changed anything
    // control cases: the correct forms are accepted, so the rejections above were about the defects
    CHECK(put(c, {setg(o, {a1, a2}, 1, on)}));                       // (a change: it waits, but it is a valid transaction)
    CHECK(put(c, {vote(g0, o.addr, w.n.addr, nx(c, g0))}));
    CHECK(put(c, {rcancel(o, on + 1)}));
}

static void test_happy_path() {
    World w(2); Chain& c = w.c; const Key &o = w.o, &g0 = w.g[0], &g1 = w.g[1], &g2 = w.g[2];
    CHECK(c.state().find(o.addr)->guardians.list.size() == 3 && c.state().find(o.addr)->guardians.threshold == 2);
    // a stranger and an account without guardians
    CHECK(!put(c, {vote(w.x, o.addr, w.n.addr, nx(c, w.x))}));
    CHECK(!put(c, {vote(g0, w.x.addr, w.n.addr, nx(c, g0))}));
    CHECK(!put(c, {vote(g0, g0.addr, w.n.addr, nx(c, g0))}));
    CHECK(put(c, {vote(g0, o.addr, w.n.addr, nx(c, g0))}));
    CHECK(!c.state().find(o.addr)->recovery.active && c.state().find(o.addr)->votes.size() == 1);
    CHECK(put(c, {vote(g1, o.addr, w.y.addr, nx(c, g1))}));          // a different new address: no agreement
    CHECK(!c.state().find(o.addr)->recovery.active && c.state().find(o.addr)->votes.size() == 2);
    CHECK(put(c, {vote(g0, o.addr, w.n.addr, nx(c, g0))}));          // the same guardian again does not count twice
    CHECK(!c.state().find(o.addr)->recovery.active && c.state().find(o.addr)->votes.size() == 2);
    CHECK(put(c, {vote(g1, o.addr, w.n.addr, nx(c, g1))}));          // g1 changes his mind: now two agree
    const std::uint64_t h = c.height();
    const Account* oa = c.state().find(o.addr);
    CHECK(oa->recovery.active && oa->recovery.target == w.n.addr && oa->recovery.ready_height == h + RECOVERY_DELAY_BLOCKS);
    CHECK(oa->votes.empty());
    // while it waits: nothing else may start, nothing changes hands
    CHECK(!put(c, {vote(g2, o.addr, w.y.addr, nx(c, g2))}));
    CHECK(!put(c, {setg(o, {g0.addr, g1.addr}, 1, nx(c, o))}));
    const Amount before = c.state().balance(o.addr);
    CHECK(before > 0);
    empty_until(c, h + RECOVERY_DELAY_BLOCKS - 1);
    CHECK(c.state().balance(o.addr) == before && c.state().balance(w.n.addr) == 0); // one block early: nothing yet
    empty_until(c, h + RECOVERY_DELAY_BLOCKS);
    CHECK(c.state().balance(o.addr) == 0 && c.state().balance(w.n.addr) == before);
    oa = c.state().find(o.addr);
    CHECK(oa && !oa->recovery.active && oa->guardians.is_default() && oa->votes.empty()); // retired key: no guardians left
    CHECK(oa->nonce == 5);                                           // nonce is kept, so old signatures stay useless
    conserved(c, w.total);
    // the new key works; the old key has nothing left
    CHECK(put(c, {transfer(w.n, w.x.addr, 1000, 0)}));
    CHECK(!put(c, {transfer(o, w.x.addr, 1000, nx(c, o))}));
    // a second recovery is impossible without new guardians
    CHECK(!put(c, {vote(g0, o.addr, w.y.addr, nx(c, g0))}));
    conserved(c, w.total);
}

static void test_single_guardian_and_all_guardians() {
    { World w(1); Chain& c = w.c; // one vote is enough
      CHECK(put(c, {vote(w.g[2], w.o.addr, w.n.addr, nx(c, w.g[2]))}));
      CHECK(c.state().find(w.o.addr)->recovery.active); }
    { World w(3); Chain& c = w.c; // all three are needed
      CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, 0 + nx(c, w.g[0]))}));
      CHECK(put(c, {vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
      CHECK(!c.state().find(w.o.addr)->recovery.active);
      CHECK(put(c, {vote(w.g[2], w.o.addr, w.n.addr, nx(c, w.g[2]))}));
      CHECK(c.state().find(w.o.addr)->recovery.active); }
    { World w(2); Chain& c = w.c; // two votes in the same block count; order inside the block is fine
      CHECK(put(c, {vote(w.g[2], w.o.addr, w.n.addr, nx(c, w.g[2])), vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0]))}));
      CHECK(c.state().find(w.o.addr)->recovery.active);
      // a third vote in the same block, after agreement, makes the whole block invalid
      World v(2); Chain& d = v.c;
      CHECK(!put(d, {vote(v.g[2], v.o.addr, v.n.addr, nx(d, v.g[2])), vote(v.g[0], v.o.addr, v.n.addr, nx(d, v.g[0])), vote(v.g[1], v.o.addr, v.n.addr, nx(d, v.g[1]))}));
      CHECK(!d.state().find(v.o.addr)->recovery.active && d.state().find(v.o.addr)->votes.empty()); }
}

static void test_owner_cancel() {
    World w(2); Chain& c = w.c;
    CHECK(!put(c, {rcancel(w.o, nx(c, w.o))}));                      // nothing to cancel
    CHECK(!put(c, {rcancel(w.x, nx(c, w.x))}));                      // a stranger has nothing to cancel either
    CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0]))}));
    CHECK(!put(c, {rcancel(w.g[0], nx(c, w.g[0]))}));                // a guardian cannot cancel (he has no recovery of his own)
    CHECK(put(c, {rcancel(w.o, nx(c, w.o))}));                       // owner clears even a partial vote
    CHECK(c.state().find(w.o.addr)->votes.empty());
    CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0]))}));
    CHECK(put(c, {vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
    const std::uint64_t ready = c.state().find(w.o.addr)->recovery.ready_height;
    CHECK(c.state().find(w.o.addr)->recovery.active);
    const Amount before = c.state().balance(w.o.addr);
    CHECK(put(c, {rcancel(w.o, nx(c, w.o))}));                       // the owner is back and stops it
    CHECK(!c.state().find(w.o.addr)->recovery.active);
    empty_until(c, ready + 5);
    CHECK(c.state().balance(w.o.addr) == before - F && c.state().balance(w.n.addr) == 0); // funds never moved (minus the cancel fee)
    CHECK(!c.state().find(w.o.addr)->guardians.is_default());        // guardians stay
    // the guardians can try again, and this time nobody stops it
    CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0]))}));
    CHECK(put(c, {vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
    empty_until(c, c.state().find(w.o.addr)->recovery.ready_height);
    CHECK(c.state().balance(w.o.addr) == 0 && c.state().balance(w.n.addr) == before - F);
    conserved(c, w.total);
}

static void test_guardian_changes_are_delayed() {
    World w(2); Chain& c = w.c;
    const Key &o = w.o, &g0 = w.g[0], &g1 = w.g[1];
    // changing the list waits, like loosening the safety rules
    CHECK(put(c, {setg(o, {g0.addr, w.x.addr}, 1, nx(c, o))}));
    const std::uint64_t ready = c.height() + SAFETY_DELAY_BLOCKS;
    const Account* oa = c.state().find(o.addr);
    CHECK(oa->pending_guardians.active && oa->pending_guardians.ready_height == ready);
    CHECK(oa->guardians.list.size() == 3 && oa->guardians.threshold == 2);
    CHECK(!put(c, {vote(w.x, o.addr, w.n.addr, nx(c, w.x))}));        // the new helper does not count yet
    // sending the current list again cancels the waiting change
    CHECK(put(c, {setg(o, {g0.addr, g1.addr, w.g[2].addr}, 2, nx(c, o))}));
    CHECK(!c.state().find(o.addr)->pending_guardians.active);
    // removing all guardians also waits
    CHECK(put(c, {setg(o, {}, 0, nx(c, o))}));
    CHECK(c.state().find(o.addr)->pending_guardians.active && c.state().find(o.addr)->guardians.list.size() == 3);
    CHECK(put(c, {vote(g0, o.addr, w.n.addr, nx(c, g0))}));            // so the old guardians still work meanwhile
    const std::uint64_t ready2 = c.state().find(o.addr)->pending_guardians.ready_height;
    empty_until(c, ready2 - 1);
    CHECK(c.state().find(o.addr)->guardians.list.size() == 3 && c.state().find(o.addr)->votes.size() == 1);
    empty_until(c, ready2);
    CHECK(c.state().find(o.addr)->guardians.is_default() && c.state().find(o.addr)->votes.empty()); // votes belonged to the old list
    CHECK(!put(c, {vote(g1, o.addr, w.n.addr, nx(c, g1))}));
    (void)ready;
}

// The thief scenario: a stolen key swaps in the thief's helpers, but the real guardians were already
// voting and their recovery finishes first (RECOVERY_DELAY_BLOCKS < SAFETY_DELAY_BLOCKS).
static void test_thief_cannot_outrun_guardians() {
    static_assert(RECOVERY_DELAY_BLOCKS < SAFETY_DELAY_BLOCKS);
    World w(2); Chain& c = w.c;
    CHECK(put(c, {setg(w.o, {w.x.addr}, 1, nx(c, w.o))}));            // thief names himself (waits)
    CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0]))}));
    CHECK(put(c, {vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
    const std::uint64_t swap_ready = c.state().find(w.o.addr)->pending_guardians.ready_height;
    const std::uint64_t rec_ready = c.state().find(w.o.addr)->recovery.ready_height;
    CHECK(rec_ready < swap_ready);
    const Amount before = c.state().balance(w.o.addr);
    empty_until(c, rec_ready);
    CHECK(c.state().balance(w.n.addr) == before && c.state().balance(w.o.addr) == 0);
    CHECK(!c.state().find(w.o.addr)->pending_guardians.active);       // the thief's swap died with the old key
    empty_until(c, swap_ready + 2);
    CHECK(c.state().find(w.o.addr)->guardians.is_default());
    conserved(c, w.total);
}

// begin_block can hit a state that a conserving chain never reaches (new address that cannot hold the funds),
// so this is tested on State directly.
static void test_overflow_and_empty_edges() {
    constexpr Amount MAXA = std::numeric_limits<Amount>::max();
    Key o = new_key(), g0 = new_key(), g1 = new_key(), n = new_key();
    auto fresh = [&](Amount ob, Amount nb) {
        State s; s.set_balance(o.addr, ob); s.set_balance(g0.addr, 100000); s.set_balance(g1.addr, 100000); if (nb) s.set_balance(n.addr, nb);
        CHECK(s.apply(setg(o, {g0.addr, g1.addr}, 2, 0), 1).ok);
        CHECK(s.apply(vote(g0, o.addr, n.addr, 0), 2).ok); CHECK(s.apply(vote(g1, o.addr, n.addr, 0), 3).ok);
        return s;
    };
    {   // the new address cannot hold the funds: nothing moves, the recovery ends, the guardians stay
        State s = fresh(100000000, MAXA - 5);
        const auto ev = s.begin_block(3 + RECOVERY_DELAY_BLOCKS);
        CHECK(ev.recoveries.size() == 1 && !ev.recoveries[0].moved && ev.recoveries[0].amount == 100000000 - F);
        CHECK(s.balance(n.addr) == MAXA - 5 && s.balance(o.addr) == 100000000 - F);
        CHECK(!s.find(o.addr)->recovery.active && s.find(o.addr)->guardians.list.size() == 2);
    }
    {   // an empty account is recovered: nothing is stored for the new address (canonical state)
        State s = fresh(F, 0);
        const std::size_t accounts = s.account_count();
        const auto ev = s.begin_block(3 + RECOVERY_DELAY_BLOCKS);
        CHECK(ev.recoveries.size() == 1 && ev.recoveries[0].moved && ev.recoveries[0].amount == 0);
        CHECK(s.find(n.addr) == nullptr && s.account_count() == accounts);
        CHECK(s.find(o.addr)->guardians.is_default());
    }
    {   // not due yet / exactly due
        State s = fresh(5000000, 0);
        CHECK(s.begin_block(3 + RECOVERY_DELAY_BLOCKS - 1).recoveries.empty());
        CHECK(s.begin_block(3 + RECOVERY_DELAY_BLOCKS).recoveries.size() == 1);
        CHECK(s.begin_block(3 + RECOVERY_DELAY_BLOCKS + 1).recoveries.empty()); // and only once
    }
    {   // the same State rebuilt twice has the same root (determinism)
        CHECK(fresh(5000000, 0).state_root() == fresh(5000000, 0).state_root());
    }
}

static void test_held_funds_and_statements() {
    World w(2); Chain& c = w.c;
    CHECK(put(c, {safety(w.o, 0, 500000, 20, nx(c, w.o))}));
    CHECK(put(c, {transfer(w.o, w.x.addr, 600000, nx(c, w.o))}));       // held for 20 blocks
    CHECK(c.state().held_total() == 600000);
    CHECK(put(c, {vote(w.g[0], w.o.addr, w.n.addr, nx(c, w.g[0]))}));
    CHECK(put(c, {vote(w.g[1], w.o.addr, w.n.addr, nx(c, w.g[1]))}));
    const std::uint64_t ready = c.state().find(w.o.addr)->recovery.ready_height;
    const Amount xb = c.state().balance(w.x.addr);
    empty_until(c, ready - 1);
    CHECK(c.state().held().empty() && c.state().balance(w.x.addr) == xb + 600000); // the hold was released long before
    const Amount before = c.state().balance(w.o.addr);
    empty_until(c, ready);
    CHECK(c.state().balance(w.n.addr) == before && c.state().balance(w.o.addr) == 0);
    conserved(c, w.total);
    // statements agree with the real state for everybody
    for (const Address& who : {w.o.addr, w.n.addr, w.x.addr, w.y.addr, w.g[0].addr, w.g[1].addr, w.g[2].addr, Address{9}}) {
        const auto s = wallet_statement(c, who);
        CHECK(s.balance == c.state().balance(who)); CHECK(s.nonce == c.state().nonce(who));
    }
    const auto so = wallet_statement(c, w.o.addr), sn = wallet_statement(c, w.n.addr), sg = wallet_statement(c, w.g[0].addr);
    CHECK(so.recovered_out == before && so.recovered_in == 0 && so.entries.back().kind == EntryKind::RecoveryMoved);
    CHECK(so.entries.back().counterparty == w.n.addr && so.entries.back().height == ready);
    CHECK(sn.recovered_in == before && sn.entries.size() == 1 && sn.entries[0].kind == EntryKind::RecoveryArrived);
    CHECK(sn.entries[0].counterparty == w.o.addr);
    bool saw_vote = false, saw_set = false;
    for (const auto& e : sg.entries) if (e.kind == EntryKind::RecoveryVoted) { saw_vote = true; CHECK(e.counterparty == w.o.addr && e.fee == F); }
    for (const auto& e : so.entries) if (e.kind == EntryKind::GuardiansChanged) saw_set = true;
    CHECK(saw_vote && saw_set);
    // fees: the owner paid for 3 transfers' worth of setup + setg + safety + transfer; the statement counts them all
    CHECK(so.total_fees == 7 * F);
    // a cancelled recovery shows up for the owner
    World v(2); CHECK(put(v.c, {vote(v.g[0], v.o.addr, v.n.addr, nx(v.c, v.g[0]))})); CHECK(put(v.c, {rcancel(v.o, nx(v.c, v.o))}));
    CHECK(wallet_statement(v.c, v.o.addr).entries.back().kind == EntryKind::RecoveryCancelled);
}

static void test_state_roots_and_export() {
    World a(2), b(2);
    CHECK(to_json(a.c).find("tazvera-chain-export-v3") != std::string::npos);
    World p(2, false); CHECK(to_json(p.c).find("tazvera-chain-export-v1") != std::string::npos); // no guardians used => still v1
    CHECK(put(p.c, {safety(p.o, 0, 0, 0, nx(p.c, p.o))})); CHECK(to_json(p.c).find("tazvera-chain-export-v2") != std::string::npos);
    // different guardian data => different roots
    World x(2), y(3), z(2);
    CHECK(x.c.state().state_root() != y.c.state().state_root() || x.o.addr != y.o.addr); // (keys differ anyway; real comparison below)
    State s1, s2; s1.set_balance(x.o.addr, 5000000); s2.set_balance(x.o.addr, 5000000);
    for (State* s : {&s1, &s2}) { s->set_balance(x.g[0].addr, 100000); s->set_balance(x.g[1].addr, 100000); }
    CHECK(s1.state_root() == s2.state_root());
    CHECK(s1.apply(setg(x.o, {x.g[0].addr, x.g[1].addr}, 1, 0), 1).ok); CHECK(s2.apply(setg(x.o, {x.g[0].addr, x.g[1].addr}, 2, 0), 1).ok);
    CHECK(s1.state_root() != s2.state_root());                           // the threshold is part of the root
    State s3 = s1; CHECK(s3.state_root() == s1.state_root());
    CHECK(s3.apply(vote(x.g[0], x.o.addr, x.n.addr, 0), 2).ok);
    CHECK(s3.state_root() != s1.state_root());                           // a vote (or a scheduled recovery) is part of the root
    State s4 = s1; CHECK(s4.apply(vote(x.g[0], x.o.addr, x.y.addr, 0), 2).ok);
    CHECK(s3.state_root() != s4.state_root());                           // and so is WHICH new address was voted for
    // large numbers stay exact in the JSON
    CHECK(to_json(a.c).find("\"guardian_threshold\": \"2\"") != std::string::npos);
    (void)b; (void)z;
}


// A signature must cover every field that changes what the transaction does (found by mutation testing).
static void test_signatures_cover_fields() {
    World w(2); Chain& c = w.c; const Key& g0 = w.g[0];
    auto rejected = [&](Transaction tx) { return !c.append_block({tx}, g_ts++).ok; };
    { auto t = vote(g0, w.o.addr, w.n.addr, nx(c, g0)); t.new_owner = w.y.addr; CHECK(rejected(t)); }        // new address changed after signing
    { auto t = vote(g0, w.o.addr, w.n.addr, nx(c, g0)); t.to = w.x.addr; CHECK(rejected(t)); }               // account changed after signing
    { auto t = vote(g0, w.o.addr, w.n.addr, nx(c, g0)); t.nonce += 1; CHECK(rejected(t)); }
    { auto t = setg(w.o, {w.g[0].addr, w.g[1].addr}, 1, nx(c, w.o)); t.guardians[1] = w.x.addr; CHECK(rejected(t)); }
    { auto t = setg(w.o, {w.g[0].addr, w.g[1].addr}, 1, nx(c, w.o)); t.guardian_threshold = 2; CHECK(rejected(t)); }
    { auto t = rcancel(w.o, nx(c, w.o)); t.nonce += 1; CHECK(rejected(t)); }
    CHECK(put(c, {vote(g0, w.o.addr, w.n.addr, nx(c, g0))}));                                              // control: the untouched forms work
    // different kinds have different signatures even over the same fields
    auto a = vote(g0, w.o.addr, w.n.addr, nx(c, g0)); auto b = rcancel(g0, nx(c, g0));
    CHECK(a.signable_bytes() != b.signable_bytes());
}

// Two recoveries that come due in the same block are carried out in a fixed order: ascending address.
static void test_same_block_order_is_fixed() {
    Key a = new_key(), b = new_key(), g = new_key(), r = new_key();
    Key p = a, q = b; if (q.addr < p.addr) std::swap(p, q); // p has the smaller address
    State s; s.set_balance(p.addr, 3000000); s.set_balance(q.addr, 5000000); s.set_balance(g.addr, 1000000);
    CHECK(s.apply(setg(p, {g.addr}, 1, 0), 1).ok); CHECK(s.apply(setg(q, {g.addr}, 1, 0), 1).ok);
    CHECK(s.apply(vote(g, p.addr, q.addr, 0), 2).ok);   // p's funds go to q
    CHECK(s.apply(vote(g, q.addr, r.addr, 1), 2).ok);   // q's funds go to r
    const Amount pb = s.balance(p.addr), qb = s.balance(q.addr);
    const auto ev = s.begin_block(2 + RECOVERY_DELAY_BLOCKS);
    CHECK(ev.recoveries.size() == 2 && ev.recoveries[0].from == p.addr && ev.recoveries[1].from == q.addr);
    CHECK(s.balance(r.addr) == pb + qb && s.balance(q.addr) == 0 && s.balance(p.addr) == 0); // p first, so q forwards both
}

static void test_root_covers_recovery_data() {
    Key o = new_key(), g0 = new_key(), g1 = new_key(), n = new_key(), m = new_key();
    auto base_state = [&] { State s; s.set_balance(o.addr, 5000000); s.set_balance(g0.addr, 1000000); s.set_balance(g1.addr, 1000000);
        CHECK(s.apply(setg(o, {g0.addr, g1.addr}, 2, 0), 1).ok); return s; };
    { State s1 = base_state(), s2 = base_state(); // same votes at different heights => scheduled at different heights
      CHECK(s1.apply(vote(g0, o.addr, n.addr, 0), 2).ok); CHECK(s1.apply(vote(g1, o.addr, n.addr, 0), 3).ok);
      CHECK(s2.apply(vote(g0, o.addr, n.addr, 0), 2).ok); CHECK(s2.apply(vote(g1, o.addr, n.addr, 0), 4).ok);
      CHECK(s1.state_root() != s2.state_root()); }
    { State s1 = base_state(), s2 = base_state(); // an unfinished vote for different new addresses
      CHECK(s1.apply(vote(g0, o.addr, n.addr, 0), 2).ok); CHECK(s2.apply(vote(g0, o.addr, m.addr, 0), 2).ok);
      CHECK(s1.state_root() != s2.state_root()); }
    { State s1 = base_state(), s2 = base_state(); // a waiting change of the guardian list
      CHECK(s1.apply(setg(o, {g0.addr}, 1, 1), 2).ok); CHECK(s2.apply(setg(o, {g1.addr}, 1, 1), 2).ok);
      CHECK(s1.state_root() != s2.state_root()); }
}

// Many blocks of random mixed transactions; every block must keep the supply exact, a follower node
// that only receives blocks must reach the same root, and statements must match the state.
static void test_random_run() {
    constexpr int K = 5;
    Key k[K]; for (auto& x : k) x = new_key();
    Chain prod, foll; const Amount total = 3 * BIG;
    for (Chain* c : {&prod, &foll}) CHECK(c->init_genesis(k[0].addr, BIG * 2, k[1].addr, BIG));
    std::uint64_t seed = 987654321; auto rnd = [&seed](std::uint64_t n) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return (seed >> 33) % n; };
    int blocks_ok = 0, scheduled = 0, executed = 0, cancelled = 0, swaps = 0;
    auto accept = [&](const std::vector<Transaction>& txs) {
        const bool had_rec = [&] { for (auto& x : k) if (auto* a = prod.state().find(x.addr)) if (a->recovery.active) return true; return false; }();
        (void)had_rec;
        if (!prod.append_block(txs, g_ts).ok) { conserved(prod, total); ++g_ts; return false; }
        ++g_ts; ++blocks_ok;
        CHECK(foll.accept_block(prod.blocks().back()).ok);
        CHECK(foll.state().state_root() == prod.state().state_root());
        conserved(prod, total); return true;
    };
    // fund everybody so that fees can be paid
    CHECK(accept({transfer(k[0], k[2].addr, 3000000, 0), transfer(k[0], k[3].addr, 3000000, 1), transfer(k[0], k[4].addr, 3000000, 2), transfer(k[1], k[4].addr, 1000000, 0)}));
    for (int step = 0; step < 3500; ++step) {
        std::vector<Transaction> txs; std::uint64_t local[K]; for (int i = 0; i < K; ++i) local[i] = nx(prod, k[i]);
        const int n = static_cast<int>(rnd(3));
        for (int i = 0; i < n; ++i) {
            const std::size_t f = rnd(K), t = (f + 1 + rnd(K - 1)) % K; const auto roll = rnd(100);
            if (roll < 30) txs.push_back(transfer(k[f], k[t].addr, 10000 + rnd(2000000), local[f]++));
            else if (roll < 36) { const Amount th = rnd(2) ? 0 : 400000 + rnd(1000000); txs.push_back(safety(k[f], rnd(2) ? 0 : 3000000 + rnd(30000000), th, th ? 1 + rnd(8) : 0, local[f]++)); }
            else if (roll < 42 && !prod.state().held().empty()) {
                auto it = prod.state().held().begin(); std::advance(it, static_cast<long>(rnd(prod.state().held().size())));
                for (int q = 0; q < K; ++q) if (k[q].addr == it->second.from) txs.push_back(cancel_held(k[q], it->first, local[q]++));
            } else if (roll < 50) { // name guardians: a random subset of the others
                std::vector<Address> list; for (int q = 0; q < K; ++q) if (static_cast<std::size_t>(q) != f && rnd(3) != 0) list.push_back(k[q].addr);
                if (list.empty()) continue;
                if (rnd(5) == 0) txs.push_back(setg(k[f], {}, 0, local[f]++));
                else txs.push_back(setg(k[f], list, 1 + rnd(list.size()), local[f]++));
                ++swaps;
            } else if (roll < 90) { // a guardian votes for a protected account that has guardians (sometimes wrongly)
                const std::size_t who = rnd(K); const Account* wa = prod.state().find(k[who].addr);
                if (!wa || wa->guardians.is_default() || wa->recovery.active) continue;
                const std::size_t gi = rnd(wa->guardians.list.size() + 1); // one past the end = a non-guardian sometimes
                std::size_t voter = f;
                if (gi < wa->guardians.list.size()) for (int q = 0; q < K; ++q) if (k[q].addr == wa->guardians.list[gi]) voter = static_cast<std::size_t>(q);
                if (voter == who) continue;
                Address target = k[rnd(K)].addr; if (rnd(4) != 0) target = k[(who + 1 + (step / 700) % (K - 1)) % K].addr; // mostly agree
                if (target == k[who].addr) continue;
                txs.push_back(vote(k[voter], k[who].addr, target, local[voter]++));
            } else { txs.push_back(rcancel(k[f], local[f]++)); }
        }
        const Block* before_tip = &prod.blocks().back(); (void)before_tip;
        std::size_t pend_before = 0; for (auto& x : k) if (auto* a = prod.state().find(x.addr)) if (a->recovery.active) ++pend_before;
        if (!accept(txs)) continue;
        std::size_t pend_after = 0; for (auto& x : k) if (auto* a = prod.state().find(x.addr)) if (a->recovery.active) ++pend_after;
        for (const Transaction& tx : prod.blocks().back().transactions) if (tx.kind == TxKind::RecoveryCancel) ++cancelled;
        if (pend_after > pend_before) scheduled += static_cast<int>(pend_after - pend_before);
        if (pend_after < pend_before) executed += static_cast<int>(pend_before - pend_after);
    }
    CHECK(blocks_ok > 300); CHECK(scheduled > 3); CHECK(executed > 1); CHECK(swaps > 5);
    for (const auto& kk : k) { const auto s = wallet_statement(prod, kk.addr); CHECK(s.balance == prod.state().balance(kk.addr)); CHECK(s.nonce == prod.state().nonce(kk.addr)); }
    if (const char* path = std::getenv("TAZ_EXPORT_RANDOM")) { std::ofstream(path, std::ios::binary) << to_json(prod); }
    std::cout << "random run: " << blocks_ok << " blocks, recoveries scheduled " << scheduled << ", ended " << executed << ", cancels " << cancelled << ", guardian settings " << swaps << "\n";
}

int main() {
    test_shape_rules(); test_happy_path(); test_single_guardian_and_all_guardians(); test_owner_cancel();
    test_guardian_changes_are_delayed(); test_thief_cannot_outrun_guardians(); test_overflow_and_empty_edges();
    test_held_funds_and_statements(); test_state_roots_and_export(); test_signatures_cover_fields();
    test_same_block_order_is_fixed(); test_root_covers_recovery_data(); test_random_run();
    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "Recovery tests passed. This is not a security audit.\n"; return EXIT_SUCCESS;
}
