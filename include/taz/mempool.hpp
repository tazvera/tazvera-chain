#pragma once
#include "taz/state.hpp"
#include "taz/tx.hpp"
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace taz {

// Local transaction queue for block building. Not a network gossip protocol.
class Mempool {
public:
    explicit Mempool(std::size_t max_capacity = 10'000,
                     std::size_t max_per_sender = 64)
        : max_capacity_(max_capacity), max_per_sender_(max_per_sender) {}

    // Validate shape/signature/nonce/balance against `state` after applying
    // already-pooled txs in order. Rejects duplicates and capacity limits.
    bool submit(const Transaction& tx, const State& state, std::uint64_t current_height,
                std::string* error = nullptr);

    // Transactions that still apply cleanly, in pool order, up to `limit`.
    std::vector<Transaction> select(const State& state, std::uint64_t current_height,
                                    std::size_t limit) const;

    void remove_included(const std::vector<Transaction>& included);
    void drop_expired(std::uint64_t current_height);
    // Remove entries that can no longer apply (their nonce was used by another transaction, the balance is gone,
    // a guardian vote came too late ...). Call after every accepted block so dead entries cannot fill the pool.
    // Returns how many were removed.
    std::size_t prune_stale(const State& state, std::uint64_t current_height);
    void clear();
    std::size_t size() const noexcept { return txs_.size(); }

    // Compatibility aliases used by older notes
    bool add(const Transaction& tx, const State& state, std::uint64_t height, std::string* err = nullptr) {
        return submit(tx, state, height, err);
    }
    std::vector<Transaction> get_pending(std::size_t limit) const {
        // Without state, only return a prefix (caller should prefer select).
        if (limit >= txs_.size()) return txs_;
        return std::vector<Transaction>(txs_.begin(), txs_.begin() + static_cast<std::ptrdiff_t>(limit));
    }

private:
    std::vector<Transaction> txs_;
    std::set<Hash32> seen_;
    std::size_t max_capacity_;
    std::size_t max_per_sender_;
};

}  // namespace taz
