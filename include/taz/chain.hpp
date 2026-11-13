#pragma once
#include "taz/block.hpp"
#include "taz/state.hpp"
namespace taz {
struct ChainResult { bool ok{false}; std::string error; };
class Chain {
public:
    bool init_genesis(const Address& first, Amount first_balance,
                      const Address& second, Amount second_balance);
    ChainResult append_block(const std::vector<Transaction>& transactions, std::int64_t timestamp);
    std::uint64_t height() const noexcept { return blocks_.empty() ? 0 : blocks_.back().height; }
    const State& state() const noexcept { return state_; }
    const std::vector<Block>& blocks() const noexcept { return blocks_; }
private:
    std::vector<Block> blocks_;
    State state_;
};
}
