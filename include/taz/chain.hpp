#pragma once
#include "taz/block.hpp"
#include "taz/state.hpp"
#include <limits>
namespace taz {
struct ChainResult { bool ok{false}; std::string error; };

inline constexpr std::int64_t NO_TIMESTAMP_LIMIT = std::numeric_limits<std::int64_t>::max();

class Chain {
public:
    bool init_genesis(const Address& first, Amount first_balance,
                      const Address& second, Amount second_balance);

    // Build and commit a block from local transactions.
    // Timestamps must be strictly increasing and <= max_timestamp. The caller supplies the
    // upper bound (e.g. wall clock + allowed drift) so this class stays deterministic.
    ChainResult append_block(const std::vector<Transaction>& transactions, std::int64_t timestamp,
                             std::int64_t max_timestamp = NO_TIMESTAMP_LIMIT);

    // Fully check a block produced elsewhere WITHOUT changing the chain: version, chain ID,
    // height, previous hash, timestamp, block size, every transaction, and the claimed state root.
    ChainResult validate_block(const Block& block,
                               std::int64_t max_timestamp = NO_TIMESTAMP_LIMIT) const;
    // validate_block + commit. On any failure the chain is unchanged.
    ChainResult accept_block(const Block& block,
                             std::int64_t max_timestamp = NO_TIMESTAMP_LIMIT);

    std::uint64_t height() const noexcept { return blocks_.empty() ? 0 : blocks_.back().height; }
    const State& state() const noexcept { return state_; }
    const std::vector<Block>& blocks() const noexcept { return blocks_; }
private:
    ChainResult check_timestamp(std::int64_t timestamp, std::int64_t max_timestamp) const;
    ChainResult execute(const std::vector<Transaction>& txs, std::uint64_t height, State& candidate) const;
    ChainResult verify(const Block& block, std::int64_t max_timestamp, State& candidate) const;
    std::vector<Block> blocks_;
    State state_;
};
}
