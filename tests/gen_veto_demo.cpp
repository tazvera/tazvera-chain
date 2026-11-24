// Writes three chain files that exercise the v8.2 veto rules, for the explorer cross-check
// (explorer/check_demos.js). Usage: taz_gen_veto_demo <output-directory>
//   veto_postpone.json   a live veto key postpones a recovery once; the recovery then completes
//   pending_discard.json a thief's pending veto is discarded because the guardians' recovery is already agreed
//   owner_cancel.json    no veto key: the owner cancels a recovery twice, then the guardians win
//   vote_expiry.json     v9.3: a guardian's vote expires, so the next guardian's vote stands alone
#include "net_ctx.hpp"
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
using namespace taz;
struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() { Key k; if (!generate_keypair(k.sk, k.pk)) { std::cerr << "keygen failed\n"; std::exit(2); } k.addr = address_from_pubkey(k.pk); return k; }
static void resign(Transaction& tx, const Key& k) { tx.network_id = taztest::g_net; if (!sign(k.sk, tx.signable_bytes(), tx.signature)) std::exit(2); }
static Transaction base(const Key& f, std::uint64_t n) { Transaction tx; tx.from = f.addr; tx.nonce = n; tx.pubkey = f.pk; return tx; }
static Transaction transfer(const Key& f, const Address& to, Amount a, std::uint64_t n) { Transaction tx = base(f, n); tx.to = to; tx.amount = a; resign(tx, f); return tx; }
static Transaction setg(const Key& f, std::vector<Address> l, std::uint64_t thr, const Address& veto, std::uint64_t n) {
    std::sort(l.begin(), l.end()); Transaction tx = base(f, n); tx.kind = TxKind::SetGuardians; tx.guardians = l; tx.guardian_threshold = thr; tx.veto = veto; resign(tx, f); return tx;
}
static Transaction vote(const Key& g, const Address& who, const Address& to, std::uint64_t n) { Transaction tx = base(g, n); tx.kind = TxKind::RecoveryVote; tx.to = who; tx.new_owner = to; resign(tx, g); return tx; }
static Transaction cancel(const Key& k, const Address& who, std::uint64_t n) { Transaction tx = base(k, n); tx.kind = TxKind::RecoveryCancel; tx.to = who; resign(tx, k); return tx; }
static std::int64_t ts = 100;
static void put(Chain& c, std::vector<Transaction> txs) { const auto r = c.append_block(txs, ts++); if (!r.ok) { std::cerr << "block refused: " << r.error << "\n"; std::exit(1); } }
static void until(Chain& c, std::uint64_t h) { while (c.height() < h) put(c, {}); }
static std::uint64_t nx(const Chain& c, const Key& k) { return c.state().nonce(k.addr); }
static void save(const Chain& c, const std::string& dir, const char* name) { std::ofstream(dir + "/" + name, std::ios::binary) << to_json(c); }
struct World {
    Key o = new_key(), g[3] = {new_key(), new_key(), new_key()}, n = new_key(), x = new_key(); Chain c;
    World() {
        if (!c.init_genesis(o.addr, 100000000, n.addr, 0)) std::exit(1);
        taztest::use(c);
        put(c, {transfer(o, g[0].addr, 2000000, 0), transfer(o, g[1].addr, 2000000, 1), transfer(o, g[2].addr, 2000000, 2), transfer(o, x.addr, 2000000, 3)});
        put(c, {setg(o, {g[0].addr, g[1].addr, g[2].addr}, 2, Address{}, 4)});
    }
    void thief_veto() { put(c, {setg(o, {g[0].addr, g[1].addr, g[2].addr}, 2, x.addr, nx(c, o))}); }
    void votes() { put(c, {vote(g[0], o.addr, n.addr, nx(c, g[0])), vote(g[1], o.addr, n.addr, nx(c, g[1]))}); }
};
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: taz_gen_veto_demo <directory>\n"; return 2; }
    const std::string dir = argv[1];
    { World w; w.thief_veto(); until(w.c, w.c.state().find(w.o.addr)->pending_guardians.ready_height); w.votes();
      put(w.c, {cancel(w.x, w.o.addr, nx(w.c, w.x))}); until(w.c, w.c.state().find(w.o.addr)->recovery.ready_height);
      if (w.c.state().balance(w.n.addr) == 0) return 1; save(w.c, dir, "veto_postpone.json"); }
    { World w; w.thief_veto(); const auto sr = w.c.state().find(w.o.addr)->pending_guardians.ready_height; until(w.c, sr - 200); w.votes();
      until(w.c, sr + 1); if (w.c.state().find(w.o.addr)->guardians.veto != Address{}) return 1;
      until(w.c, w.c.state().find(w.o.addr)->recovery.ready_height); if (w.c.state().balance(w.n.addr) == 0) return 1; save(w.c, dir, "pending_discard.json"); }
    { World w; for (int i = 0; i < 2; ++i) { w.votes(); put(w.c, {cancel(w.o, Address{}, nx(w.c, w.o))}); }
      w.votes(); until(w.c, w.c.state().find(w.o.addr)->recovery.ready_height); if (w.c.state().balance(w.n.addr) == 0) return 1; save(w.c, dir, "owner_cancel.json"); }
    // v9.3: one guardian votes, the vote expires, a second guardian's vote then stands alone; only a third fresh vote agrees.
    { World w; put(w.c, {vote(w.g[0], w.o.addr, w.n.addr, nx(w.c, w.g[0]))}); const auto cast = w.c.height();
      until(w.c, cast + VOTE_EXPIRY_BLOCKS); put(w.c, {vote(w.g[1], w.o.addr, w.n.addr, nx(w.c, w.g[1]))});
      if (w.c.state().find(w.o.addr)->recovery.active || w.c.state().find(w.o.addr)->votes.size() != 1) return 1;   // the old vote is gone
      put(w.c, {vote(w.g[2], w.o.addr, w.n.addr, nx(w.c, w.g[2]))}); if (!w.c.state().find(w.o.addr)->recovery.active) return 1;
      until(w.c, w.c.state().find(w.o.addr)->recovery.ready_height); if (w.c.state().balance(w.n.addr) == 0) return 1; save(w.c, dir, "vote_expiry.json"); }
    return 0;
}
