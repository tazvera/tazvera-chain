#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include "taz/mempool.hpp"
#include "taz/persist.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace taz;
static int failures = 0;
#define CHECK(x) do { if(!(x)){ std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while(0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() { Key k; CHECK(generate_keypair(k.sk, k.pk)); k.addr = address_from_pubkey(k.pk); return k; }
static void resign(Transaction& tx, const Key& k) { CHECK(sign(k.sk, tx.signable_bytes(), tx.signature)); }
static Transaction base(const Key& f, std::uint64_t n) { Transaction tx; tx.from = f.addr; tx.nonce = n; tx.pubkey = f.pk; return tx; }
static Transaction transfer(const Key& f, const Address& to, Amount a, std::uint64_t n, std::uint64_t expiry = 0) {
    Transaction tx = base(f, n); tx.to = to; tx.amount = a; tx.expiry_height = expiry; resign(tx, f); return tx;
}
static Transaction safety(const Key& f, Amount limit, std::uint64_t n) {
    Transaction tx = base(f, n); tx.kind = TxKind::SetSafety; tx.daily_limit = limit; resign(tx, f); return tx;
}
static Transaction setg(const Key& f, std::vector<Address> l, std::uint64_t thr, std::uint64_t n) {
    std::sort(l.begin(), l.end()); Transaction tx = base(f, n); tx.kind = TxKind::SetGuardians; tx.guardians = l; tx.guardian_threshold = thr; resign(tx, f); return tx;
}
static Transaction vote(const Key& g, const Address& who, const Address& to, std::uint64_t n) {
    Transaction tx = base(g, n); tx.kind = TxKind::RecoveryVote; tx.to = who; tx.new_owner = to; resign(tx, g); return tx;
}
static std::int64_t g_ts = 10;
static constexpr Amount INITIAL = 100000000;
static bool rejected(Mempool& p, const Chain& c, const Transaction& t, const char* needle) {
    std::string err; const bool ok = p.submit(t, c.state(), c.height(), &err);
    return !ok && err.find(needle) != std::string::npos;
}

int main() {
    Key alice = new_key(), bob = new_key(), g1 = new_key(), g2 = new_key(), fresh = new_key();
    Chain chain; CHECK(chain.init_genesis(alice.addr, INITIAL, bob.addr, 5000000));
    CHECK(chain.append_block({transfer(alice, g1.addr, 3000000, 0), transfer(alice, g2.addr, 3000000, 1)}, g_ts++).ok);
    Mempool pool(100, 8); std::string err;

    const auto t2 = transfer(alice, bob.addr, 1000, 2), t3 = transfer(alice, bob.addr, 2000, 3), t4 = transfer(alice, bob.addr, 3000, 4);
    CHECK(rejected(pool, chain, t3, "nonce"));                       // a gap in the nonces is refused
    CHECK(pool.submit(t2, chain.state(), chain.height(), &err)); CHECK(pool.submit(t3, chain.state(), chain.height(), &err));
    CHECK(pool.submit(t4, chain.state(), chain.height(), &err)); CHECK(pool.size() == 3);
    CHECK(rejected(pool, chain, t2, "duplicate"));
    { auto bad = t2; bad.signature[1] ^= 0xff; CHECK(!pool.submit(bad, chain.state(), chain.height(), &err)); }
    { auto bad = t2; bad.amount += 1; CHECK(!pool.submit(bad, chain.state(), chain.height(), &err)); }
    CHECK(rejected(pool, chain, transfer(alice, bob.addr, INITIAL * 2, 5), "balance"));
    CHECK(rejected(pool, chain, transfer(alice, bob.addr, 1, 5, 1), "expired"));   // expiry height 1 is already past
    CHECK(pool.size() == 3);

    // select respects the limit and keeps order; a block built from it is accepted
    CHECK(pool.select(chain.state(), chain.height(), 2).size() == 2);
    const auto batch = pool.select(chain.state(), chain.height(), 10);
    CHECK(batch.size() == 3 && batch[0].txid() == t2.txid() && batch[2].txid() == t4.txid());
    CHECK(chain.append_block(batch, g_ts++).ok);
    pool.remove_included(batch); CHECK(pool.size() == 0);
    CHECK(rejected(pool, chain, t2, "nonce"));                       // an included transaction cannot come back

    // capacity and per-sender limits
    { Mempool tiny(1, 8); CHECK(tiny.submit(transfer(alice, bob.addr, 1, 5), chain.state(), chain.height(), &err));
      CHECK(rejected(tiny, chain, transfer(alice, bob.addr, 1, 6), "full")); }
    { Mempool per(100, 2); CHECK(per.submit(transfer(alice, bob.addr, 1, 5), chain.state(), chain.height(), &err));
      CHECK(per.submit(transfer(alice, bob.addr, 1, 6), chain.state(), chain.height(), &err));
      CHECK(rejected(per, chain, transfer(alice, bob.addr, 1, 7), "too many"));
      CHECK(per.submit(transfer(bob, alice.addr, 1, 0), chain.state(), chain.height(), &err)); } // another sender is unaffected

    // the safety rules and guardian rules apply to pooled transactions too
    { Mempool p; CHECK(p.submit(safety(alice, 5000, 5), chain.state(), chain.height(), &err));
      CHECK(rejected(p, chain, transfer(alice, bob.addr, 6000, 6), "daily limit"));   // sees the limit that is still only in the pool
      CHECK(p.submit(transfer(alice, bob.addr, 4000, 6), chain.state(), chain.height(), &err)); }
    CHECK(chain.append_block({setg(alice, {g1.addr, g2.addr}, 2, 5)}, g_ts++).ok);
    { Mempool p;
      CHECK(rejected(p, chain, vote(bob, alice.addr, fresh.addr, 0), "guardian"));    // bob is not a guardian
      CHECK(p.submit(vote(g1, alice.addr, fresh.addr, 0), chain.state(), chain.height(), &err));
      CHECK(p.submit(vote(g2, alice.addr, fresh.addr, 0), chain.state(), chain.height(), &err));
      const auto votes = p.select(chain.state(), chain.height(), 10); CHECK(votes.size() == 2);
      CHECK(chain.append_block(votes, g_ts++).ok); p.remove_included(votes);
      CHECK(chain.state().find(alice.addr)->recovery.active);
      CHECK(rejected(p, chain, vote(g1, alice.addr, bob.addr, 1), "already waiting")); }

    // stale entries are pruned: the pool entry's nonce was used by a different transaction
    { Chain c2; CHECK(c2.init_genesis(alice.addr, INITIAL, bob.addr, 0)); Mempool p;
      const auto mine = transfer(alice, bob.addr, 7000, 0); CHECK(p.submit(mine, c2.state(), c2.height(), &err));
      CHECK(p.submit(transfer(alice, bob.addr, 8000, 1), c2.state(), c2.height(), &err));
      CHECK(c2.append_block({transfer(alice, bob.addr, 9999, 0)}, g_ts++).ok);       // someone else's block used nonce 0
      p.remove_included({transfer(alice, bob.addr, 9999, 0)}); CHECK(p.size() == 2);  // not the same transaction, so both stay
      CHECK(p.select(c2.state(), c2.height(), 10).size() == 1);                       // select skips the dead nonce-0 entry
      CHECK(p.prune_stale(c2.state(), c2.height()) == 1);                             // and prune removes it for good
      CHECK(p.size() == 1 && p.select(c2.state(), c2.height(), 10).size() == 1);
      CHECK(p.prune_stale(c2.state(), c2.height()) == 0); }
    { Chain c3; CHECK(c3.init_genesis(alice.addr, INITIAL, bob.addr, 0)); Mempool p;   // expiry
      CHECK(p.submit(transfer(alice, bob.addr, 1, 0, 3), c3.state(), c3.height(), &err));
      CHECK(p.select(c3.state(), 5, 10).empty()); p.drop_expired(5); CHECK(p.size() == 0); }

    // saving: the file equals the export, no temporary file is left, and a bad path fails cleanly
    const std::string path = "taz_mempool_test_chain.json";
    CHECK(save_chain_json(chain, path, &err));
    { std::ifstream in(path, std::ios::binary); std::string body((std::istreambuf_iterator<char>(in)), {}); CHECK(body == to_json(chain)); }
    { std::ifstream tmp(path + ".tmp"); CHECK(!tmp.good()); }
    CHECK(save_chain_json(chain, path, &err));                       // overwriting works
    std::remove(path.c_str());
    CHECK(!save_chain_json(chain, "/nonexistent-dir/x/chain.json", &err) && !err.empty());

    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "Mempool + persist tests passed. This is not a security audit.\n"; return 0;
}
