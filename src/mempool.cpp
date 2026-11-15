#include "taz/mempool.hpp"

namespace taz {

bool Mempool::submit(const Transaction& tx, const State& state, std::uint64_t current_height,
                     std::string* error) {
    const Hash32 id = tx.txid();
    if (seen_.count(id)) {
        if (error) *error = "duplicate transaction";
        return false;
    }
    if (txs_.size() >= max_capacity_) {
        if (error) *error = "mempool full";
        return false;
    }
    std::size_t from_count = 0;
    for (const auto& p : txs_) {
        if (p.from == tx.from) ++from_count;
    }
    if (from_count >= max_per_sender_) {
        if (error) *error = "too many pending txs from sender";
        return false;
    }
    if (tx.expiry_height != 0 && current_height > tx.expiry_height) {
        if (error) *error = "transaction expired";
        return false;
    }

    State trial = state;
    // Holds/releases that would run at this height before new txs (same as block start).
    trial.begin_block(current_height == 0 ? 1 : current_height + 1);
    for (const auto& p : txs_) {
        auto ar = trial.apply(p, current_height == 0 ? 1 : current_height + 1);
        if (!ar.ok) continue;  // stale pool entry; select will skip
    }
    const std::uint64_t apply_h = current_height == 0 ? 1 : current_height + 1;
    auto ar = trial.apply(tx, apply_h);
    if (!ar.ok) {
        if (error) *error = ar.error.empty() ? "transaction not applicable" : ar.error;
        return false;
    }

    txs_.push_back(tx);
    seen_.insert(id);
    return true;
}

std::vector<Transaction> Mempool::select(const State& state, std::uint64_t current_height,
                                         std::size_t limit) const {
    std::vector<Transaction> out;
    State trial = state;
    const std::uint64_t apply_h = current_height == 0 ? 1 : current_height + 1;
    trial.begin_block(apply_h);
    for (const auto& tx : txs_) {
        if (out.size() >= limit) break;
        if (tx.expiry_height != 0 && apply_h > tx.expiry_height) continue;
        auto ar = trial.apply(tx, apply_h);
        if (!ar.ok) continue;
        out.push_back(tx);
    }
    return out;
}

void Mempool::remove_included(const std::vector<Transaction>& included) {
    for (const auto& tx : included) seen_.erase(tx.txid());
    std::vector<Transaction> keep;
    keep.reserve(txs_.size());
    for (const auto& t : txs_) {
        if (seen_.count(t.txid())) keep.push_back(t);
    }
    txs_.swap(keep);
}

void Mempool::drop_expired(std::uint64_t current_height) {
    std::vector<Transaction> keep;
    for (const auto& t : txs_) {
        if (t.expiry_height != 0 && current_height > t.expiry_height) {
            seen_.erase(t.txid());
            continue;
        }
        keep.push_back(t);
    }
    txs_.swap(keep);
}

std::size_t Mempool::prune_stale(const State& state, std::uint64_t current_height) {
    State trial = state;
    const std::uint64_t apply_h = current_height == 0 ? 1 : current_height + 1;
    trial.begin_block(apply_h);
    std::vector<Transaction> keep;
    std::size_t removed = 0;
    for (const auto& tx : txs_) {
        const bool expired = tx.expiry_height != 0 && apply_h > tx.expiry_height;
        if (expired || !trial.apply(tx, apply_h).ok) { seen_.erase(tx.txid()); ++removed; continue; }
        keep.push_back(tx);
    }
    txs_.swap(keep);
    return removed;
}

void Mempool::clear() {
    txs_.clear();
    seen_.clear();
}

}  // namespace taz
