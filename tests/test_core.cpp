#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/json_io.hpp"
#include "taz/mempool.hpp"
#include "taz/tx.hpp"
#include <iostream>

using namespace taz;

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::cerr << "FAIL: " << msg << "\n"; ++fails; } } while (0)

static Transaction signed_tx(const PrivKey& priv, const PubKey& pub,
                             const Address& from, const Address& to,
                             Amount amount, uint64_t nonce, Amount fee = FEE_TRANSFER_V1) {
  Transaction tx;
  tx.from = from;
  tx.to = to;
  tx.amount = amount;
  tx.fee = fee;
  tx.nonce = nonce;
  tx.pubkey = pub;
  auto msg = tx.signable_bytes();
  sign(priv, msg.data(), msg.size(), tx.signature);
  return tx;
}

int main() {
  PrivKey ap, bp;
  PubKey apk, bpk;
  demo_keypair(10, ap, apk);
  demo_keypair(20, bp, bpk);
  Address alice = address_from_pubkey(apk);
  Address bob = address_from_pubkey(bpk);
  Amount A0 = tzv_to_utzv(500);
  Amount B0 = tzv_to_utzv(50);

  {
    Chain c;
    c.init_genesis(alice, A0, bob, B0);
    auto tx = signed_tx(ap, apk, alice, bob, tzv_to_utzv(5), 0);
    auto r = c.append_transfer(tx, 100);
    CHECK(r.ok, "p1 transfer");
    CHECK(c.state().circulating() + c.state().burned_total() == A0 + B0, "p1 supply");
  }

  {
    auto tx = signed_tx(ap, apk, alice, bob, tzv_to_utzv(1), 0);
    auto j = tx_to_json(tx);
    auto back = tx_from_json(j);
    CHECK(back.has_value(), "tx json parse");
    CHECK(back->txid() == tx.txid(), "txid stable");
    CHECK(back->amount == tx.amount && back->nonce == tx.nonce, "fields");
    auto j2 = tx_to_json(*back);
    CHECK(j2 == j, "canonical json stable");
  }

  {
    Chain c;
    c.init_genesis(alice, A0, bob, B0);
    auto t0 = signed_tx(ap, apk, alice, bob, tzv_to_utzv(1), 0);
    auto t1 = signed_tx(ap, apk, alice, bob, tzv_to_utzv(1), 1);
    CHECK(c.append_block({t0, t1}, 200).ok, "multi block");
    auto j = block_to_json(c.blocks().back());
    auto b = block_from_json(j);
    CHECK(b.has_value(), "block parse");
    CHECK(b->txs.size() == 2, "2 txs");
    CHECK(b->block_hash() == c.blocks().back().block_hash(), "block hash");
    CHECK(b->header.state_root == c.blocks().back().header.state_root, "state root");
  }

  {
    Chain c;
    c.init_genesis(alice, A0, bob, B0);
    Mempool m;
    std::string err;
    auto good = signed_tx(ap, apk, alice, bob, tzv_to_utzv(1), 0);
    CHECK(m.submit(good, c.state(), 0, &err), "submit good");
    CHECK(!m.submit(good, c.state(), 0, &err), "reject dup");
    CHECK(err.find("duplicate") != std::string::npos, "dup msg");

    auto bad = good;
    bad.signature[3] ^= 0x11;
    CHECK(!m.submit(bad, c.state(), 0, &err), "reject bad sig");

    auto wrong_n = signed_tx(ap, apk, alice, bob, tzv_to_utzv(1), 9);
    CHECK(!m.submit(wrong_n, c.state(), 0, &err), "reject bad nonce");

    auto t1 = signed_tx(ap, apk, alice, bob, tzv_to_utzv(1), 1);
    CHECK(m.submit(t1, c.state(), 0, &err), "submit nonce1");
    CHECK(m.size() == 2, "pool size 2");

    auto r = c.seal_from_mempool(m, 300, 10);
    CHECK(r.ok, "seal");
    CHECK(c.height() == 1, "height");
    CHECK(m.size() == 0, "pool drained");
    CHECK(c.blocks().back().txs.size() == 2, "2 in block");
  }

  {
    Chain c;
    c.init_genesis(alice, A0, bob, B0);
    auto t0 = signed_tx(ap, apk, alice, bob, tzv_to_utzv(1), 0);
    auto t_bad = signed_tx(ap, apk, alice, bob, tzv_to_utzv(1), 0);
    auto r = c.append_block({t0, t_bad}, 400);
    CHECK(!r.ok, "multi fail");
    CHECK(c.height() == 0, "no block");
    CHECK(c.state().balance(alice) == A0, "unchanged");
  }

  if (fails) {
    std::cerr << fails << " test(s) failed\n";
    return 1;
  }
  std::cout << "All tests passed (Phase 1 + Phase 2 Package 1).\n";
  return 0;
}
