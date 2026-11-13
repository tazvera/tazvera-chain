#include "taz/chain.hpp"
namespace taz {
bool Chain::init_genesis(const Address& first, Amount first_balance, const Address& second, Amount second_balance) {
    if (!blocks_.empty() || first == second) return false;
    State genesis_state; genesis_state.set_balance(first, first_balance); genesis_state.set_balance(second, second_balance);
    Block genesis; genesis.height=0; genesis.timestamp=0; genesis.state_root=genesis_state.state_root();
    state_=genesis_state; blocks_.push_back(genesis); return true;
}
ChainResult Chain::check_timestamp(std::int64_t timestamp, std::int64_t max_timestamp) const {
    if (timestamp <= blocks_.back().timestamp) return {false,"block timestamp must be strictly greater than the previous block"};
    if (timestamp > max_timestamp) return {false,"block timestamp is too far in the future"};
    return {true,{}};
}
ChainResult Chain::execute(const std::vector<Transaction>& txs, std::uint64_t height, State& candidate) const {
    if (txs.size() > MAX_BLOCK_TXS) return {false,"too many transactions in block"};
    for (std::size_t i = 0; i < txs.size(); ++i) {
        const auto applied = candidate.apply(txs[i], height);
        if (!applied.ok) return {false,"tx " + std::to_string(i) + ": " + applied.error};
    }
    return {true,{}};
}
ChainResult Chain::append_block(const std::vector<Transaction>& transactions, std::int64_t timestamp, std::int64_t max_timestamp) {
    if (blocks_.empty()) return {false,"genesis is not initialized"};
    if (auto t = check_timestamp(timestamp, max_timestamp); !t.ok) return t;
    State candidate = state_; // transactional copy: failed block cannot partially mutate live state
    const std::uint64_t next_height = blocks_.back().height + 1;
    if (auto r = execute(transactions, next_height, candidate); !r.ok) return r;
    Block block; block.height=next_height; block.previous_hash=blocks_.back().hash();
    block.timestamp=timestamp; block.transactions=transactions; block.state_root=candidate.state_root();
    state_=std::move(candidate); blocks_.push_back(std::move(block)); return {true,{}};
}
ChainResult Chain::verify(const Block& block, std::int64_t max_timestamp, State& candidate) const {
    if (blocks_.empty()) return {false,"genesis is not initialized"};
    const Block& tip = blocks_.back();
    if (block.protocol_version != PROTOCOL_VERSION) return {false,"unsupported block protocol version"};
    if (block.chain_id != CHAIN_ID) return {false,"block has wrong chain ID"};
    if (block.height != tip.height + 1) return {false,"unexpected block height"};
    if (block.previous_hash != tip.hash()) return {false,"previous hash does not match chain tip"};
    if (auto t = check_timestamp(block.timestamp, max_timestamp); !t.ok) return t;
    candidate = state_;
    if (auto r = execute(block.transactions, block.height, candidate); !r.ok) return r;
    if (block.state_root != candidate.state_root()) return {false,"state root mismatch"};
    return {true,{}};
}
ChainResult Chain::validate_block(const Block& block, std::int64_t max_timestamp) const {
    State candidate;
    return verify(block, max_timestamp, candidate);
}
ChainResult Chain::accept_block(const Block& block, std::int64_t max_timestamp) {
    State candidate;
    auto r = verify(block, max_timestamp, candidate);
    if (!r.ok) return r;
    state_ = std::move(candidate); blocks_.push_back(block); return {true,{}};
}
}
