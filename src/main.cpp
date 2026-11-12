#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/json_io.hpp"
#include "taz/mempool.hpp"
#include "taz/tx.hpp"
#include <iostream>

using namespace taz;

static Transaction make_signed_transfer(const PrivKey& priv, const PubKey& pub,
                                        const Address& from, const Address& to,
                                        Amount amount, uint64_t nonce) {
  Transaction tx;
  tx.from = from;
  tx.to = to;
  tx.amount = amount;
  tx.fee = FEE_TRANSFER_V1;
  tx.nonce = nonce;
  tx.pubkey = pub;
  auto msg = tx.signable_bytes();
  if (!sign(priv, msg.data(), msg.size(), tx.signature))
    throw std::runtime_error("sign failed");
  return tx;
}

int main() {
  std::cout << "=== TAZVERA Chain · Phase 2 Package 1 ===\n";
  std::cout << "JSON + Mempool + multi-tx block (local prototype)\n\n";

  PrivKey alice_priv{}, bob_priv{};
  PubKey alice_pub{}, bob_pub{};
  demo_keypair(1, alice_priv, alice_pub);
  demo_keypair(2, bob_priv, bob_pub);
  Address alice = address_from_pubkey(alice_pub);
  Address bob = address_from_pubkey(bob_pub);

  const Amount alice0 = tzv_to_utzv(1000);
  const Amount bob0 = tzv_to_utzv(100);

  Chain chain;
  chain.init_genesis(alice, alice0, bob, bob0);

  Mempool pool;
  std::string err;
  auto t0 = make_signed_transfer(alice_priv, alice_pub, alice, bob, tzv_to_utzv(3), 0);
  auto t1 = make_signed_transfer(alice_priv, alice_pub, alice, bob, tzv_to_utzv(2), 1);
  auto t2 = make_signed_transfer(alice_priv, alice_pub, alice, bob, tzv_to_utzv(1), 2);

  if (!pool.submit(t0, chain.state(), chain.height(), &err)) {
    std::cerr << "submit t0: " << err << "\n";
    return 1;
  }
  if (!pool.submit(t1, chain.state(), chain.height(), &err)) {
    std::cerr << "submit t1: " << err << "\n";
    return 1;
  }
  if (!pool.submit(t2, chain.state(), chain.height(), &err)) {
    std::cerr << "submit t2: " << err << "\n";
    return 1;
  }
  if (pool.submit(t0, chain.state(), chain.height(), &err)) {
    std::cerr << "duplicate should fail\n";
    return 1;
  }
  std::cout << "Mempool size after 3 unique + 1 dup reject: " << pool.size() << "\n";

  auto sealed = chain.seal_from_mempool(pool, 1700000001ULL, 64);
  if (!sealed.ok) {
    std::cerr << "seal failed: " << sealed.error << "\n";
    return 1;
  }
  std::cout << "Sealed block height " << chain.height()
            << " burned_total_fee=" << sealed.burned
            << " pool_left=" << pool.size() << "\n";
  std::cout << "Alice=" << chain.state().balance(alice)
            << " Bob=" << chain.state().balance(bob)
            << " burned=" << chain.state().burned_total() << "\n";

  const Block& tip = chain.blocks().back();
  std::string j = block_to_json(tip);
  std::cout << "\nBlock JSON (truncated):\n" << j.substr(0, 200) << "...\n";

  auto back = block_from_json(j);
  if (!back) {
    std::cerr << "JSON import failed\n";
    return 1;
  }
  if (back->block_hash() != tip.block_hash()) {
    std::cerr << "block hash mismatch after JSON round-trip\n";
    return 1;
  }
  if (back->txs.size() != tip.txs.size()) {
    std::cerr << "tx count mismatch\n";
    return 1;
  }
  for (size_t i = 0; i < tip.txs.size(); ++i) {
    if (back->txs[i].txid() != tip.txs[i].txid()) {
      std::cerr << "txid mismatch at " << i << "\n";
      return 1;
    }
  }
  std::cout << "JSON round-trip OK (" << tip.txs.size() << " txs). Hash match.\n";
  std::cout << "Supply check: circ+burn="
            << (chain.state().circulating() + chain.state().burned_total())
            << " genesis=" << (alice0 + bob0) << "\n";
  return 0;
}
