#ifndef TAZ_BLOCK_HPP
#define TAZ_BLOCK_HPP

#include "taz/types.hpp"
#include "taz/tx.hpp"

namespace taz {

struct Block {
    uint64_t height{0};
    Hash32 prev_hash{};
    int64_t timestamp{0};
    std::vector<Transaction> transactions;

    Hash32 hash() const;
};

}

#endif
