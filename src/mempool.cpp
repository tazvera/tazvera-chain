#include "taz/mempool.hpp"
#include <algorithm>

namespace taz {

bool Mempool::add(const Transaction& tx, uint64_t current_height) {
    if (pool_.size() >= max_capacity_) return false;
    if (tx.expiry_height != 0 && current_height > tx.expiry_height) return false;

    for (const auto& existing : pool_) {
        if (existing.from == tx.from && existing.nonce == tx.nonce) return false;
    }

    pool_.push_back(tx);
    return true;
}

std::vector<Transaction> Mempool::get_pending(size_t limit) const {
    std::vector<Transaction> result;
    size_t count = std::min(limit, pool_.size());
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) result.push_back(pool_[i]);
    return result;
}

void Mempool::remove_included(const std::vector<Transaction>& included_txs) {
    for (const auto& mined : included_txs) {
        pool_.erase(
            std::remove_if(pool_.begin(), pool_.end(),
                [&mined](const Transaction& tx) {
                    return tx.from == mined.from && tx.nonce == mined.nonce;
                }),
            pool_.end()
        );
    }
}

size_t Mempool::size() const { return pool_.size(); }

}
