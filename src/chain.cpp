#include "taz/chain.hpp"
#include "taz/crypto.hpp"

namespace taz {

void Chain::init_genesis(const Address& alice, Amount alice_bal,
                         const Address& bob, Amount bob_bal) {
  blocks_.clear();
  state_ = State{};
  state_.set_balance(alice, alice_bal);
  state_.set_balance(bob, bob_bal);

  Block g;
  g.header.height = 0;
  g.header.timestamp = 0;
  g.header.prev_hash = {};
  g.header.tx_root = Block::merkle_tx_root({});
  g.header.state_root = state_.state_root();
  blocks_.push_back(g);
}

ApplyResult Chain::append_transfer(const Transaction& tx, uint64_t timestamp) {
  return append_block(std::vector<Transaction>{tx}, timestamp);
}

ApplyResult Chain::append_block(const std::vector<Transaction>& txs, uint64_t timestamp) {
  if (blocks_.empty()) return {false, "chain not initialized", 0};
  if (txs.empty()) return {false, "empty block not allowed in prototype", 0};

  State trial = state_;
  Amount burned = 0;
  for (const auto& tx : txs) {
    auto ar = trial.apply(tx, height());
    if (!ar.ok) return ar;
    burned += ar.burned;
  }

  Block b;
  b.header.height = height() + 1;
  b.header.timestamp = timestamp;
  b.header.prev_hash = blocks_.back().block_hash();
  b.txs = txs;
  b.header.tx_root = Block::merkle_tx_root(b.txs);
  b.header.state_root = trial.state_root();

  state_ = std::move(trial);
  blocks_.push_back(std::move(b));
  return {true, "", burned};
}

ApplyResult Chain::seal_from_mempool(Mempool& pool, uint64_t timestamp, size_t max_txs) {
  auto batch = pool.select(state_, height(), max_txs);
  if (batch.empty()) return {false, "mempool empty or no applicable txs", 0};
  auto r = append_block(batch, timestamp);
  if (r.ok) pool.remove_included(batch);
  return r;
}

}  // namespace taz
