#pragma once
#include "taz/state.hpp"
#include "taz/tx.hpp"
#include <set>
#include <string>
#include <vector>

namespace taz {

struct MempoolReject {
  Hash32 txid{};
  std::string reason;
};

class Mempool {
 public:
  // Validate against current state snapshot + pool contents. Does not mutate chain state.
  bool submit(const Transaction& tx, const State& state, uint64_t current_height,
              std::string* err = nullptr);

  // Select up to max_count txs that still apply cleanly in order (simulates block building).
  std::vector<Transaction> select(const State& state, uint64_t current_height,
                                  size_t max_count) const;

  void remove_included(const std::vector<Transaction>& txs);
  void clear();
  size_t size() const { return txs_.size(); }
  const std::vector<Transaction>& txs() const { return txs_; }

 private:
  std::vector<Transaction> txs_;
  std::set<Hash32> seen_;  // txids in pool
};

}  // namespace taz
