#include "taz/chain.hpp"
namespace taz {
bool Chain::init_genesis(const Address& first, Amount first_balance, const Address& second, Amount second_balance) {
    if (!blocks_.empty() || first == second) return false;
    State genesis_state; genesis_state.set_balance(first, first_balance); genesis_state.set_balance(second, second_balance);
    Block genesis; genesis.height=0; genesis.timestamp=0; genesis.state_root=genesis_state.state_root();
    state_=genesis_state; blocks_.push_back(genesis); return true;
}
ChainResult Chain::append_block(const std::vector<Transaction>& transactions, std::int64_t timestamp) {
    if (blocks_.empty()) return {false,"genesis is not initialized"};
    if (timestamp < blocks_.back().timestamp) return {false,"block timestamp moved backwards"};
    State candidate = state_; // transactional copy: failed block cannot partially mutate live state
    const std::uint64_t next_height = blocks_.back().height + 1;
    for (const auto& tx : transactions) {
        const auto applied = candidate.apply(tx, next_height);
        if (!applied.ok) return {false,applied.error};
    }
    Block block; block.height=next_height; block.previous_hash=blocks_.back().hash();
    block.timestamp=timestamp; block.transactions=transactions; block.state_root=candidate.state_root();
    state_=std::move(candidate); blocks_.push_back(std::move(block)); return {true,{}};
}
}
