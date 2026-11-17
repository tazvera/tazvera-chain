#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <algorithm>
#include <limits>
#include <string_view>
namespace taz {
bool Chain::init_genesis(const Address& first, Amount first_balance, const Address& second, Amount second_balance) {
    if (!blocks_.empty() || first == second) return false;
    State genesis_state; genesis_state.set_balance(first, first_balance); genesis_state.set_balance(second, second_balance);
    Block genesis; genesis.height=0; genesis.timestamp=0; genesis.state_root=genesis_state.state_root();
    // v8.2: the commitment over BOTH genesis accounts (zero balances included) sits in block 0's previous_hash field
    // (genesis has no predecessor), so it is covered by block 0's hash and, through previous_hash links, by every later block.
    genesis.previous_hash = genesis_commitment_of(first, first_balance, second, second_balance);
    state_=genesis_state; blocks_.push_back(genesis);
    genesis_balances_.clear();
    // v8: always record both genesis accounts (including zero) so load_chain_json can rebuild.
    genesis_balances_.emplace_back(first, first_balance);
    genesis_balances_.emplace_back(second, second_balance);
    return true;
}
ChainResult Chain::check_timestamp(std::int64_t timestamp, std::int64_t max_timestamp) const {
    if (timestamp <= blocks_.back().timestamp) return {false,"block timestamp must be strictly greater than the previous block"};
    if (timestamp > max_timestamp) return {false,"block timestamp is too far in the future"};
    return {true,{}};
}
ChainResult Chain::execute(const std::vector<Transaction>& txs, std::uint64_t height, State& candidate) const {
    if (txs.size() > MAX_BLOCK_TXS) return {false,"too many transactions in block"};
    candidate.begin_block(height); // release due holds and apply due safety changes before any transaction
    for (std::size_t i = 0; i < txs.size(); ++i) {
        const auto applied = candidate.apply(txs[i], height);
        if (!applied.ok) return {false,"tx " + std::to_string(i) + ": " + applied.error};
    }
    return {true,{}};
}
ChainResult Chain::append_block(const std::vector<Transaction>& transactions, std::int64_t timestamp, std::int64_t max_timestamp) {
    if (blocks_.empty()) return {false,"genesis is not initialized"};
    // Never let the height wrap back to zero (restored from Core v6.2; Core v7 had dropped it).
    if (blocks_.back().height == std::numeric_limits<std::uint64_t>::max()) return {false,"chain height exhausted"};
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
    if (tip.height == std::numeric_limits<std::uint64_t>::max()) return {false,"chain height exhausted"};
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

Hash32 genesis_commitment_of(const Address& a0, Amount b0, const Address& a1, Amount b1) {
    // Canonical: the two accounts are sorted by address, so the order they were given in cannot matter.
    std::pair<Address, Amount> e[2] = {{a0, b0}, {a1, b1}};
    if (e[1].first < e[0].first) std::swap(e[0], e[1]);
    std::vector<std::uint8_t> bytes;
    constexpr std::string_view domain = "TAZVERA-GENESIS-V2";
    bytes.insert(bytes.end(), domain.begin(), domain.end());
    const std::string_view cid = CHAIN_ID;
    bytes.insert(bytes.end(), cid.begin(), cid.end()); bytes.push_back(0);
    for (int i = 3; i >= 0; --i) bytes.push_back(static_cast<std::uint8_t>((PROTOCOL_VERSION >> (8 * i)) & 0xff));
    for (const auto& [addr, bal] : e) {
        bytes.insert(bytes.end(), addr.begin(), addr.end());
        for (int i = 7; i >= 0; --i) bytes.push_back(static_cast<std::uint8_t>((bal >> (8 * i)) & 0xff));
    }
    return sha256(bytes);
}
Hash32 Chain::genesis_commitment() const {
    if (genesis_balances_.size() != 2) return Hash32{};
    return genesis_commitment_of(genesis_balances_[0].first, genesis_balances_[0].second, genesis_balances_[1].first, genesis_balances_[1].second);
}
}
