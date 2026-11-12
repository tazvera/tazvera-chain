#include "taz/mempool.hpp"
#include "taz/crypto.hpp"

namespace taz {

bool Mempool::submit(const Transaction& tx, const State& state, uint64_t current_height,
                     std::string* err) {
  auto id = tx.txid();
  if (seen_.count(id)) {
    if (err) *err = "duplicate transaction";
    return false;
  }

  // Shape + signature + nonce/balance against live state (not yet including pool)
  State trial = state;
  // Apply already-pooled txs from same sender in order for nonce/balance checks
  for (const auto& p : txs_) {
    auto ar = trial.apply(p, current_height);
    if (!ar.ok) {
      // Pool should only hold valid txs; skip corrupt entry defensively
      continue;
    }
  }

  auto ar = trial.apply(tx, current_height);
  if (!ar.ok) {
    if (err) *err = ar.error;
    return false;
  }

  txs_.push_back(tx);
  seen_.insert(id);
  return true;
}

std::vector<Transaction> Mempool::select(const State& state, uint64_t current_height,
                                         size_t max_count) const {
  std::vector<Transaction> out;
  State trial = state;
  for (const auto& tx : txs_) {
    if (out.size() >= max_count) break;
    auto ar = trial.apply(tx, current_height);
    if (!ar.ok) continue;
    out.push_back(tx);
  }
  return out;
}

void Mempool::remove_included(const std::vector<Transaction>& txs) {
  for (const auto& tx : txs) {
    auto id = tx.txid();
    seen_.erase(id);
  }
  std::vector<Transaction> keep;
  keep.reserve(txs_.size());
  for (const auto& t : txs_) {
    if (seen_.count(t.txid())) keep.push_back(t);
  }
  // Rebuild seen from keep (remove_included erased ids; keep those still in seen)
  // Actually we erased included ids from seen, so keep = those still in seen
  txs_.swap(keep);
}

void Mempool::clear() {
  txs_.clear();
  seen_.clear();
}

}  // namespace taz
