#ifndef TAZ_MEMPOOL_HPP
#define TAZ_MEMPOOL_HPP

#include "taz/tx.hpp"
#include <deque>
#include <vector>

namespace taz {

class Mempool {
public:
    bool add(const Transaction& tx, uint64_t current_height);
    std::vector<Transaction> get_pending(size_t limit) const;
    void remove_included(const std::vector<Transaction>& included_txs);
    size_t size() const;

private:
    std::deque<Transaction> pool_;
    size_t max_capacity_{10000};
};

}

#endif
