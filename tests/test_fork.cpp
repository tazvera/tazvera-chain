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

int main() {
    f01_unrelated_chains();
    f02_longer_wins_both_ways();
    f03_equal_chains();
    f04_equal_height_disagreement_is_decided_the_same_way();
    if (failures == 0) { std::cout << "fork tests: all passed\n"; return 0; }
    std::cerr << "fork tests: " << failures << " failure(s)\n";
    return 1;
}
