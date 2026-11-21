// v9.0: the fork tie-break. Deterministic, not consensus — and the tests say so by checking that two nodes
// looking at the same pair always make the same choice, in both directions.
#include "taz/fork.hpp"
#include "taz/crypto.hpp"
#include <cstdlib>
#include <iostream>

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
static void resign(Transaction& tx, const Key& k) { if (!sign(k.sk, tx.signable_bytes(), tx.signature)) std::exit(2); }
static Transaction transfer(const Key& f, const Address& to, Amount a, std::uint64_t n) {
    Transaction tx; tx.from = f.addr; tx.to = to; tx.amount = a; tx.nonce = n; tx.pubkey = f.pk;
    resign(tx, f); return tx;
}
static std::int64_t ts = 900;

static void f01_unrelated_chains() {
    Key o1 = new_key(), g = new_key(), o2 = new_key();
    Chain a; CHECK(a.init_genesis(o1.addr, 1000, g.addr, 0));
    Chain b; CHECK(b.init_genesis(o2.addr, 1000, g.addr, 0));
    CHECK(choose_chain(a, b).choice == ForkChoice::DifferentChain);
    CHECK(choose_chain(b, a).choice == ForkChoice::DifferentChain);
}

static void f02_longer_wins_both_ways() {
    Key o = new_key(), g = new_key();
    Chain shortc; CHECK(shortc.init_genesis(o.addr, 100000, g.addr, 0));
    Chain longc = shortc;
    CHECK(longc.append_block({transfer(o, g.addr, 1, 0)}, ts++).ok);
    CHECK(choose_chain(shortc, longc).choice == ForkChoice::TakeTheirs);
    CHECK(choose_chain(longc, shortc).choice == ForkChoice::KeepMine);
}

static void f03_equal_chains() {
    Key o = new_key(), g = new_key();
    Chain a; CHECK(a.init_genesis(o.addr, 1000, g.addr, 0));
    Chain b = a;
    CHECK(choose_chain(a, b).choice == ForkChoice::SameChain);
}

static void f04_equal_height_disagreement_is_decided_the_same_way() {
    Key o = new_key(), g = new_key();
    Chain a; CHECK(a.init_genesis(o.addr, 100000, g.addr, 0));
    CHECK(a.append_block({transfer(o, g.addr, 1, 0)}, ts++).ok);

    // the same chain with a different second block: same height, same genesis, different state
    Chain b; CHECK(b.init_genesis(o.addr, 100000, g.addr, 0));
    CHECK(b.append_block({transfer(o, g.addr, 2, 0)}, ts++).ok);
    CHECK(a.height() == b.height());
    CHECK(a.state().state_root() != b.state().state_root());

    const ForkDecision ab = choose_chain(a, b);
    const ForkDecision ba = choose_chain(b, a);
    CHECK(ab.choice != ForkChoice::SameChain);
    // whichever side is asked, both agree on which chain survives
    const bool a_wins = ab.choice == ForkChoice::KeepMine;
    CHECK(a_wins ? (ba.choice == ForkChoice::TakeTheirs) : (ba.choice == ForkChoice::KeepMine));
}

// v9.0: the rollback that makes a fork choice real. Without it, "the other chain wins" cannot be acted on.
static void f05_truncate_rebuilds_the_state() {
    Key o = new_key(), g = new_key();
    // Fixed timestamps, because a block hash covers its timestamp: replaying the same transactions at other
    // times gives the same state and a different hash, and this test wants to prove the chain rebuilds exactly.
    const std::int64_t t0 = ts;
    ts += 3;

    Chain full; CHECK(full.init_genesis(o.addr, 100000, g.addr, 0));
    CHECK(full.append_block({transfer(o, g.addr, 10, 0)}, t0).ok);
    CHECK(full.append_block({transfer(o, g.addr, 20, 1)}, t0 + 1).ok);
    CHECK(full.append_block({transfer(o, g.addr, 30, 2)}, t0 + 2).ok);
    CHECK(full.height() == 3);

    Chain cut = full;
    CHECK(cut.truncate_to(1).ok);
    CHECK(cut.height() == 1);
    CHECK(cut.state().balance(g.addr) == 10);            // only the first transfer happened
    CHECK(cut.state().nonce(o.addr) == 1);
    CHECK(cut.blocks().back().hash() == full.blocks()[1].hash());

    // replaying the same transactions at the same times must rebuild the same chain, block for block
    CHECK(cut.append_block({transfer(o, g.addr, 20, 1)}, t0 + 1).ok);
    CHECK(cut.append_block({transfer(o, g.addr, 30, 2)}, t0 + 2).ok);
    CHECK(cut.height() == full.height());
    CHECK(cut.state().state_root() == full.state().state_root());
    CHECK(cut.state().balance(g.addr) == full.state().balance(g.addr));
    CHECK(cut.state().nonce(o.addr) == full.state().nonce(o.addr));
    CHECK(cut.blocks().back().hash() == full.blocks().back().hash());

    // edges: truncating to the current height changes nothing, growing is refused, 0 leaves genesis
    CHECK(full.truncate_to(3).ok);
    CHECK(!full.truncate_to(4).ok);
    Chain genesis_only = full;
    CHECK(genesis_only.truncate_to(0).ok);
    CHECK(genesis_only.height() == 0);
    CHECK(genesis_only.state().balance(o.addr) == 100000);
}

// a producer chain must survive the rebuild too, or a reorg would silently drop the producer set
static void f06_truncate_keeps_the_producers() {
    Key o = new_key(), g = new_key();
    std::vector<Address> producers{o.addr};
    Chain c; CHECK(c.init_genesis(o.addr, 100000, g.addr, 0, producers));
    CHECK(!c.producers().empty());
    CHECK(c.append_block_signed({transfer(o, g.addr, 5, 0)}, ts++, o.sk, o.pk).ok);
    CHECK(c.append_block_signed({transfer(o, g.addr, 6, 1)}, ts++, o.sk, o.pk).ok);

    Chain cut = c;
    CHECK(cut.truncate_to(1).ok);
    CHECK(cut.producers() == producers);
    CHECK(cut.height() == 1);
    CHECK(cut.state().balance(g.addr) == 5);
}

// v9.0: the same state can come from two different histories. Comparing state roots would call those two
// chains "the same" and leave both nodes on different histories while each believed they agreed.
static void f07_same_state_different_history_is_not_the_same_chain() {
    Key o = new_key(), g = new_key();
    Chain a; CHECK(a.init_genesis(o.addr, 100000, g.addr, 0));
    Chain b; CHECK(b.init_genesis(o.addr, 100000, g.addr, 0));

    // the same transfer, produced at two different times: identical state, different block hash
    CHECK(a.append_block({transfer(o, g.addr, 7, 0)}, 5000).ok);
    CHECK(b.append_block({transfer(o, g.addr, 7, 0)}, 6000).ok);

    CHECK(a.state().state_root() == b.state().state_root());
    CHECK(a.blocks().back().hash() != b.blocks().back().hash());

    const ForkDecision ab = choose_chain(a, b);
    CHECK(ab.choice != ForkChoice::SameChain);          // the old rule said SameChain here
    const ForkDecision ba = choose_chain(b, a);
    CHECK((ab.choice == ForkChoice::KeepMine) == (ba.choice == ForkChoice::TakeTheirs));
}

int main() {
    f01_unrelated_chains();
    f02_longer_wins_both_ways();
    f03_equal_chains();
    f04_equal_height_disagreement_is_decided_the_same_way();
    f05_truncate_rebuilds_the_state();
    f06_truncate_keeps_the_producers();
    f07_same_state_different_history_is_not_the_same_chain();
    if (failures == 0) { std::cout << "fork tests: all passed\n"; return 0; }
    std::cerr << "fork tests: " << failures << " failure(s)\n";
    return 1;
}
