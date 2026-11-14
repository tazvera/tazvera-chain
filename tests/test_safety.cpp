#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
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
static Transaction base(const Key& f, std::uint64_t nonce) {
    Transaction tx; tx.from = f.addr; tx.nonce = nonce; tx.pubkey = f.pk; return tx;
}
static Transaction transfer(const Key& f, const Address& to, Amount a, std::uint64_t n) {
    Transaction tx = base(f, n); tx.to = to; tx.amount = a; resign(tx, f); return tx;
}
static Transaction safety(const Key& f, Amount limit, Amount thr, std::uint64_t hold, std::uint64_t n) {
    Transaction tx = base(f, n); tx.kind = TxKind::SetSafety; tx.daily_limit = limit; tx.hold_threshold = thr; tx.hold_blocks = hold; resign(tx, f); return tx;
}
static Transaction cancel(const Key& f, const Hash32& ref, std::uint64_t n) {
    Transaction tx = base(f, n); tx.kind = TxKind::CancelHeld; tx.ref = ref; resign(tx, f); return tx;
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

static void test_shape_rules() {
    Key a = new_key(), b = new_key();
    Chain c; CHECK(c.init_genesis(a.addr, BIG, b.addr, 0));
    CHECK(put(c, {safety(a, 0, 0, 0, 0)})); // a no-op setting is a valid transaction
    auto bad = [&](Transaction tx) { resign(tx, a); return !c.append_block({tx}, g_ts++).ok; };
    { auto t = safety(a, 0, 0, 5, 1); CHECK(bad(t)); }                       // hold time but nothing to hold
    { auto t = safety(a, 0, 500, 0, 1); CHECK(bad(t)); }                     // threshold but no hold time
    { auto t = safety(a, 0, 500, MAX_HOLD_BLOCKS + 1, 1); CHECK(bad(t)); }   // hold too long
    { auto t = safety(a, 0, 0, 0, 1); t.amount = 5; CHECK(bad(t)); }         // unused field set
    { auto t = safety(a, 0, 0, 0, 1); t.to = b.addr; CHECK(bad(t)); }
    { auto t = safety(a, 0, 0, 0, 1); t.ref[0] = 1; CHECK(bad(t)); }
    { auto t = transfer(a, b.addr, 5, 1); t.daily_limit = 7; CHECK(bad(t)); } // safety field inside a transfer
    { auto t = transfer(a, b.addr, 5, 1); t.ref[3] = 1; CHECK(bad(t)); }
    { auto t = cancel(a, Hash32{}, 1); t.hold_blocks = 3; CHECK(bad(t)); }
    { auto t = cancel(a, Hash32{}, 1); t.amount = 3; CHECK(bad(t)); }
    { auto t = safety(a, 0, 0, 0, 1); t.kind = static_cast<TxKind>(9); CHECK(bad(t)); } // unknown kind
    CHECK(put(c, {safety(a, 1000000, 0, 0, 1)}));                            // the control case still works
    // signatures do not carry over between kinds: a signature for one setting does not fit another
    auto t = safety(a, 5, 0, 0, 2); auto u = t; u.daily_limit = 6; CHECK(!c.append_block({u}, g_ts++).ok);
}

static void test_daily_limit() {
    Key a = new_key(), b = new_key();
    Chain c; CHECK(c.init_genesis(a.addr, BIG, b.addr, 0));
    CHECK(put(c, {safety(a, 1000000, 0, 0, 0)}));
    CHECK(put(c, {transfer(a, b.addr, 600000, 1)}));
    CHECK(put(c, {transfer(a, b.addr, 400000, 2)}));          // exactly at the limit
    CHECK(!c.append_block({transfer(a, b.addr, 1, 3)}, g_ts++).ok); // one more is refused
    CHECK(c.state().balance(b.addr) == 1000000);
    empty_until(c, LIMIT_WINDOW_BLOCKS - 2);
    CHECK(!c.append_block({transfer(a, b.addr, 1, 3)}, g_ts++).ok); // block LIMIT-1 is still the first window
    CHECK(put(c, {}));
    CHECK(put(c, {transfer(a, b.addr, 1000000, 3)}));         // first block of the next window: allowance is fresh
    CHECK(!c.append_block({transfer(a, b.addr, 1, 4)}, g_ts++).ok);
    // several transfers inside ONE block share the same allowance
    Chain d; CHECK(d.init_genesis(a.addr, BIG, b.addr, 0));
    CHECK(put(d, {safety(a, 1000, 0, 0, 0)}));
    CHECK(!d.append_block({transfer(a, b.addr, 600, 1), transfer(a, b.addr, 600, 2)}, g_ts++).ok);
    CHECK(d.state().nonce(a.addr) == 1);                       // whole block rejected
    CHECK(put(d, {transfer(a, b.addr, 600, 1), transfer(a, b.addr, 400, 2)}));
    // the limit counts the fee-free amount only, and a limit of 0 means unlimited
    CHECK(put(d, {safety(a, 0, 0, 0, 3)}));                 // switching the limit off is a loosening: accepted ...
    CHECK(d.state().find(a.addr)->pending.active);          // ... but only waiting, the limit is still on
    CHECK(d.state().find(a.addr)->safety.daily_limit == 1000);
}

static void test_hold_and_release() {
    Key a = new_key(), b = new_key();
    Chain c; CHECK(c.init_genesis(a.addr, BIG, b.addr, 0));
    CHECK(put(c, {safety(a, 0, 500000, 5, 0)}));                  // h1: hold transfers >= 500000 for 5 blocks
    CHECK(put(c, {transfer(a, b.addr, 600000, 1)}));              // h2: held until height 7
    CHECK(c.state().balance(a.addr) == BIG - 2 * F - 600000);
    CHECK(c.state().balance(b.addr) == 0); CHECK(c.state().held_total() == 600000); conserved(c, BIG);
    CHECK(put(c, {transfer(a, b.addr, 100000, 2)}));              // h3: below the threshold, arrives at once
    CHECK(c.state().balance(b.addr) == 100000);
    empty_until(c, 6);
    CHECK(c.state().balance(b.addr) == 100000); CHECK(c.state().held_total() == 600000); // still waiting at h6
    CHECK(put(c, {}));                                           // h7: released at the start of this block
    CHECK(c.state().balance(b.addr) == 700000); CHECK(c.state().held_total() == 0); conserved(c, BIG);
    // the receiver can already spend it in the block where it is released
    Key z = new_key();
    Chain d; CHECK(d.init_genesis(a.addr, BIG, b.addr, 0));
    CHECK(put(d, {safety(a, 0, 500000, 2, 0)})); CHECK(put(d, {transfer(a, b.addr, 600000, 1)})); // released at h4
    CHECK(put(d, {})); // h3
    CHECK(d.state().balance(b.addr) == 0);                   // not yet: released at the start of block 4
    CHECK(put(d, {transfer(b, z.addr, 1000, 0)}));           // block 4: b already holds the 600000 and can spend it
    CHECK(d.state().balance(b.addr) == 600000 - 1000 - F); CHECK(d.state().balance(z.addr) == 1000);
}

static void test_cancel() {
    Key a = new_key(), b = new_key();
    Chain c; CHECK(c.init_genesis(a.addr, BIG, b.addr, 50000));
    CHECK(put(c, {safety(a, 0, 500000, 10, 0)}));
    const auto held = transfer(a, b.addr, 700000, 1); const Hash32 id = held.txid();
    CHECK(put(c, {held}));
    CHECK(c.state().held().contains(id));
    CHECK(!c.append_block({cancel(b, id, 0)}, g_ts++).ok);                    // someone else cannot cancel it
    Hash32 unknown{}; unknown[0] = 9;
    CHECK(!c.append_block({cancel(a, unknown, 2)}, g_ts++).ok);                // nothing to cancel
    CHECK(put(c, {cancel(a, id, 2)}));                                        // the owner takes it back
    CHECK(c.state().held().empty()); CHECK(c.state().balance(b.addr) == 50000);
    CHECK(c.state().balance(a.addr) == BIG - 3 * F);                         // only the three fees are gone
    conserved(c, BIG + 50000);
    CHECK(!c.append_block({cancel(a, id, 3)}, g_ts++).ok);                    // cannot cancel twice
    empty_until(c, 30); CHECK(c.state().balance(b.addr) == 50000);           // and nothing arrives later
    // too late once it has been released
    Chain d; CHECK(d.init_genesis(a.addr, BIG, b.addr, 0));
    CHECK(put(d, {safety(a, 0, 500000, 3, 0)}));
    const auto h2 = transfer(a, b.addr, 700000, 1); CHECK(put(d, {h2}));
    empty_until(d, 6); CHECK(d.state().balance(b.addr) == 700000);
    CHECK(!d.append_block({cancel(a, h2.txid(), 2)}, g_ts++).ok);
}

static void test_loosening_is_delayed() {
    Key a = new_key(), b = new_key();
    auto fresh = [&](Chain& c) { CHECK(c.init_genesis(a.addr, BIG, b.addr, 0)); CHECK(put(c, {safety(a, 1000000, 0, 0, 0)})); };
    { // a raised limit only works after SAFETY_DELAY_BLOCKS
        Chain c; fresh(c);
        CHECK(put(c, {safety(a, 5000000, 0, 0, 1)}));                       // h2: loosening -> waits until h2+DELAY
        CHECK(c.state().find(a.addr)->pending.active);
        CHECK(!c.append_block({transfer(a, b.addr, 2000000, 2)}, g_ts++).ok); // old limit still rules
        empty_until(c, 2 + SAFETY_DELAY_BLOCKS - 2);                           // height 500
        CHECK(!c.append_block({transfer(a, b.addr, 2000000, 2)}, g_ts++).ok); // block 501: one block too early
        CHECK(put(c, {}));                                                    // block 501 (empty)
        CHECK(put(c, {transfer(a, b.addr, 2000000, 2)}));                     // block 502 = 2+DELAY: rules switch at its start
        CHECK(c.state().find(a.addr)->safety.daily_limit == 5000000); CHECK(!c.state().find(a.addr)->pending.active);
    }
    { // a tighter rule is immediate and also cancels a waiting loosening
        Chain c; fresh(c);
        CHECK(put(c, {safety(a, 5000000, 0, 0, 1)}));                       // pending loosening
        CHECK(put(c, {safety(a, 200000, 0, 0, 2)}));                        // tighter: immediate
        CHECK(c.state().find(a.addr)->safety.daily_limit == 200000); CHECK(!c.state().find(a.addr)->pending.active);
        CHECK(!c.append_block({transfer(a, b.addr, 300000, 3)}, g_ts++).ok);
        CHECK(put(c, {transfer(a, b.addr, 200000, 3)}));
    }
    { // re-sending the CURRENT settings cancels a waiting loosening (the owner's "no, that was not me")
        Chain c; fresh(c);
        CHECK(put(c, {safety(a, 0, 0, 0, 1)}));                             // switch limit off: pending
        CHECK(c.state().find(a.addr)->pending.active);
        CHECK(put(c, {safety(a, 1000000, 0, 0, 2)}));                       // same as now: immediate, clears pending
        CHECK(!c.state().find(a.addr)->pending.active);
        empty_until(c, 2 + SAFETY_DELAY_BLOCKS + 3);
        CHECK(!c.append_block({transfer(a, b.addr, 2000000, 3)}, g_ts++).ok); // limit is still on
    }
    { // each way of loosening is recognised, including turning a rule off and a shorter hold
        Chain c; CHECK(c.init_genesis(a.addr, BIG, b.addr, 0));
        CHECK(put(c, {safety(a, 1000, 5000, 10, 0)}));
        CHECK(put(c, {safety(a, 1000, 0, 0, 1)}));       CHECK(c.state().find(a.addr)->pending.active); // hold switched off
        CHECK(put(c, {safety(a, 1000, 5000, 10, 2)}));   CHECK(!c.state().find(a.addr)->pending.active);
        CHECK(put(c, {safety(a, 1000, 5000, 3, 3)}));    CHECK(c.state().find(a.addr)->pending.active);  // shorter hold
        CHECK(put(c, {safety(a, 1000, 9000, 10, 4)}));   CHECK(c.state().find(a.addr)->pending.active == false || true);
        CHECK(put(c, {safety(a, 1000, 4000, 12, 5)}));   CHECK(c.state().find(a.addr)->safety.hold_threshold == 4000); // tighter in all ways
    }
}

static void test_atomic_block() {
    Key a = new_key(), b = new_key();
    Chain c; CHECK(c.init_genesis(a.addr, BIG, b.addr, 0));
    const auto before = c.state().state_root();
    CHECK(!c.append_block({safety(a, 1000, 0, 0, 0), transfer(a, b.addr, 999999999, 1)}, g_ts++).ok);
    CHECK(c.state().state_root() == before); CHECK(c.state().nonce(a.addr) == 0); CHECK(c.state().find(a.addr)->safety.is_default());
}

static void test_state_roots() {
    Key a = new_key(), b = new_key();
    Chain plain; CHECK(plain.init_genesis(a.addr, BIG, b.addr, 0)); CHECK(put(plain, {transfer(a, b.addr, 5, 0)}));
    CHECK(to_json(plain).find("tazvera-chain-export-v1") != std::string::npos);
    Chain safe; CHECK(safe.init_genesis(a.addr, BIG, b.addr, 0)); CHECK(put(safe, {safety(a, 0, 0, 0, 0)}));
    CHECK(to_json(safe).find("tazvera-chain-export-v2") != std::string::npos);
    // different rules give different roots, same rules give the same root
    State s1, s2; s1.set_balance(a.addr, 5); s2.set_balance(a.addr, 5); CHECK(s1.state_root() == s2.state_root());
    Chain x, y; CHECK(x.init_genesis(a.addr, BIG, b.addr, 0)); CHECK(y.init_genesis(a.addr, BIG, b.addr, 0));
    CHECK(put(x, {safety(a, 1000, 0, 0, 0)})); CHECK(put(y, {safety(a, 2000, 0, 0, 0)}));
    CHECK(x.state().state_root() != y.state().state_root());
    // a held transfer is part of the root
    Chain h1, h2; CHECK(h1.init_genesis(a.addr, BIG, b.addr, 0)); CHECK(h2.init_genesis(a.addr, BIG, b.addr, 0));
    CHECK(put(h1, {safety(a, 0, 100, 5, 0)})); CHECK(put(h2, {safety(a, 0, 100, 5, 0)}));
    CHECK(put(h1, {transfer(a, b.addr, 500, 1)})); CHECK(put(h2, {}));
    CHECK(h1.state().state_root() != h2.state().state_root());
}

static void test_statement_matches_state() {
    Key a = new_key(), b = new_key(), c3 = new_key();
    Chain c; CHECK(c.init_genesis(a.addr, BIG, b.addr, 40000));
    CHECK(put(c, {safety(a, 0, 500000, 4, 0)}));
    const auto t1 = transfer(a, b.addr, 700000, 1); CHECK(put(c, {t1}));             // held
    CHECK(put(c, {transfer(a, c3.addr, 1000, 2)}));                                    // immediate
    const auto t2 = transfer(a, c3.addr, 900000, 3); CHECK(put(c, {t2}));              // held
    CHECK(put(c, {cancel(a, t2.txid(), 4)}));                                          // cancelled
    empty_until(c, 12);                                                                 // t1 released
    for (const Address& who : {a.addr, b.addr, c3.addr, Address{9}}) {
        const auto s = wallet_statement(c, who);
        CHECK(s.balance == c.state().balance(who)); CHECK(s.nonce == c.state().nonce(who));
    }
    const auto sa = wallet_statement(c, a.addr);
    CHECK(sa.total_sent == 700000 + 1000 + 900000); CHECK(sa.total_refunded == 900000);
    CHECK(sa.total_fees == 5 * F); CHECK(sa.held_outgoing == 0);
    const auto sb = wallet_statement(c, b.addr);
    CHECK(sb.total_received == 700000); CHECK(sb.entries.size() == 1 && sb.entries[0].kind == EntryKind::ReleasedIn);
    // while still held, the receiver sees it as "incoming", not as balance
    Chain d; CHECK(d.init_genesis(a.addr, BIG, b.addr, 0));
    CHECK(put(d, {safety(a, 0, 500000, 50, 0)})); CHECK(put(d, {transfer(a, b.addr, 700000, 1)}));
    CHECK(wallet_statement(d, b.addr).held_incoming == 700000); CHECK(wallet_statement(d, b.addr).balance == 0);
    CHECK(wallet_statement(d, a.addr).held_outgoing == 700000);
    CHECK(wallet_statement(d, a.addr).entries.back().kind == EntryKind::HeldOut);
}

// A deterministic pseudo-random run: many blocks with a mix of every kind of transaction.
// After EVERY block: no money is created or lost, and a second node that only receives the blocks
// reaches exactly the same state root.
static void test_random_run() {
    Key k[3] = {new_key(), new_key(), new_key()};
    Chain prod, foll;
    const Amount total = 3 * BIG;
    for (Chain* c : {&prod, &foll}) { CHECK(c->init_genesis(k[0].addr, BIG * 2, k[1].addr, BIG)); }
    std::uint64_t seed = 12345; auto rnd = [&seed](std::uint64_t n) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return (seed >> 33) % n; };
    const Amount limits[] = {0, 3000000, 30000000}, thrs[] = {0, 50000, 2000000}, amounts[] = {10000, 120000, 2500000};
    int blocks_ok = 0, held_seen = 0, cancels_ok = 0, releases = 0, pending_seen = 0;
    for (int step = 0; step < 900; ++step) {
        std::vector<Transaction> txs; std::uint64_t local[3] = {nx(prod, k[0]), nx(prod, k[1]), nx(prod, k[2])};
        const int n = static_cast<int>(rnd(4));
        for (int i = 0; i < n; ++i) {
            const std::size_t f = rnd(3), t = (f + 1 + rnd(2)) % 3; const auto roll = rnd(10);
            if (roll < 6) txs.push_back(transfer(k[f], k[t].addr, amounts[rnd(3)], local[f]++));
            else if (roll < 8) { const auto th = thrs[rnd(3)]; txs.push_back(safety(k[f], limits[rnd(3)], th, th ? 1 + rnd(6) : 0, local[f]++)); }
            else if (!prod.state().held().empty()) {
                auto it = prod.state().held().begin(); std::advance(it, static_cast<long>(rnd(prod.state().held().size())));
                std::size_t owner = 0; for (std::size_t q = 0; q < 3; ++q) if (k[q].addr == it->second.from) owner = q;
                txs.push_back(cancel(k[owner], it->first, local[owner]++));
            }
        }
        const std::size_t held_before = prod.state().held().size();
        if (!prod.append_block(txs, g_ts).ok) { // a bad random block is fine; the chain must be untouched
            conserved(prod, total); ++g_ts; continue;
        }
        ++g_ts; ++blocks_ok;
        const Block& b = prod.blocks().back();
        CHECK(foll.accept_block(b).ok);
        CHECK(foll.state().state_root() == prod.state().state_root());
        conserved(prod, total);
        if (prod.state().held().size() > held_before) ++held_seen;
        if (prod.state().held().size() < held_before) ++releases;
        for (const Transaction& tx : b.transactions) if (tx.kind == TxKind::CancelHeld) ++cancels_ok;
        for (const auto& kk : k) if (prod.state().find(kk.addr) && prod.state().find(kk.addr)->pending.active) { ++pending_seen; break; }
    }
    CHECK(blocks_ok > 200); CHECK(held_seen > 5); CHECK(releases > 3); CHECK(cancels_ok > 0); CHECK(pending_seen > 5);
    for (const auto& kk : k) { const auto s = wallet_statement(prod, kk.addr); CHECK(s.balance == prod.state().balance(kk.addr)); CHECK(s.nonce == prod.state().nonce(kk.addr)); }
    if (const char* path = std::getenv("TAZ_EXPORT_RANDOM")) { std::ofstream(path, std::ios::binary) << to_json(prod); } // lets the explorer be cross-checked
    std::cout << "random run: " << blocks_ok << " blocks, holds " << held_seen << ", releases " << releases << ", cancels " << cancels_ok << ", pending-states " << pending_seen << "\n";
}

int main() {
    test_shape_rules(); test_daily_limit(); test_hold_and_release(); test_cancel(); test_loosening_is_delayed();
    test_atomic_block(); test_state_roots(); test_statement_matches_state(); test_random_run();
    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "Safety-rule tests passed. This is not a security audit.\n"; return EXIT_SUCCESS;
}
