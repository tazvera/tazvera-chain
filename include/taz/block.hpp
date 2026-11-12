#pragma once
#include "taz/tx.hpp"
#include "taz/types.hpp"
#include <vector>

namespace taz {

struct BlockHeader {
  uint32_t protocol_version = PROTOCOL_VERSION;
  uint32_t chain_id = CHAIN_ID;
  uint64_t height = 0;
  Hash32 prev_hash{};
  uint64_t timestamp = 0;
  Hash32 tx_root{};
  Hash32 state_root{};
};

struct Block {
  BlockHeader header;
  std::vector<Transaction> txs;

  Hash32 block_hash() const;
  static Hash32 merkle_tx_root(const std::vector<Transaction>& txs);
};

}  // namespace taz
