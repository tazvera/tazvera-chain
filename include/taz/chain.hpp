#pragma once
#include "taz/block.hpp"
#include "taz/mempool.hpp"
#include "taz/state.hpp"
#include <vector>

namespace taz {

class Chain {
 public:
  void init_genesis(const Address& alice, Amount alice_bal,
                    const Address& bob, Amount bob_bal);

  const Block& genesis() const { return blocks_.front(); }
  const std::vector<Block>& blocks() const { return blocks_; }
  const State& state() const { return state_; }
  uint64_t height() const { return blocks_.empty() ? 0 : blocks_.back().header.height; }

  ApplyResult append_transfer(const Transaction& tx, uint64_t timestamp);
  ApplyResult append_block(const std::vector<Transaction>& txs, uint64_t timestamp);
  ApplyResult seal_from_mempool(Mempool& pool, uint64_t timestamp, size_t max_txs = 64);

 private:
  State state_;
  std::vector<Block> blocks_;
};

}  // namespace taz
