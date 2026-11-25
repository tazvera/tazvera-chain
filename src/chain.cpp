#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <algorithm>
#include <limits>
#include <string_view>
namespace taz {
namespace {
// What a producer signs. The chain ID, the NETWORK ID (protocol 4) and the height are in the message itself, so a
// signature says in its own bytes which network and which height it belongs to — not only implicitly, through the block
// hash and the previous_hash chain down to the genesis commitment.
std::vector<std::uint8_t> block_signable(const std::string& chain_id, const Hash32& network_id, std::uint64_t height, const Hash32& block_hash) {
    std::vector<std::uint8_t> out;
    constexpr std::string_view domain = "TAZVERA-BLOCK-SIG-V3";
    out.insert(out.end(), domain.begin(), domain.end());
    out.insert(out.end(), chain_id.begin(), chain_id.end());
    out.push_back(0);
    out.insert(out.end(), network_id.begin(), network_id.end());
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((height >> (8 * i)) & 0xff));
    out.insert(out.end(), block_hash.begin(), block_hash.end());
    return out;
}
}

bool Chain::init_genesis(const Address& first, Amount first_balance, const Address& second, Amount second_balance) {
    if (!blocks_.empty() || first == second) return false;
    State genesis_state; genesis_state.set_balance(first, first_balance); genesis_state.set_balance(second, second_balance);
    Block genesis; genesis.height=0; genesis.timestamp=0; genesis.state_root=genesis_state.state_root();
    // v8.2: the commitment over BOTH genesis accounts (zero balances included) sits in block 0's previous_hash field
    // (genesis has no predecessor), so it is covered by block 0's hash and, through previous_hash links, by every later block.
    genesis.previous_hash = genesis_commitment_of(first, first_balance, second, second_balance);
    state_=genesis_state; blocks_.push_back(genesis);
    state_.bind_network(genesis.previous_hash);
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
ChainResult Chain::prepare_block(const std::vector<Transaction>& transactions, std::int64_t timestamp,
                                 std::int64_t max_timestamp, Block& out, State& next_state) const {
    if (blocks_.empty()) return {false,"genesis is not initialized"};
    // Never let the height wrap back to zero (restored from Core v6.2; Core v7 had dropped it).
    if (blocks_.back().height == std::numeric_limits<std::uint64_t>::max()) return {false,"chain height exhausted"};
    if (auto t = check_timestamp(timestamp, max_timestamp); !t.ok) return t;
    State candidate = state_; // transactional copy: failed block cannot partially mutate live state
    const std::uint64_t next_height = blocks_.back().height + 1;
    if (auto r = execute(transactions, next_height, candidate); !r.ok) return r;
    Block block; block.height=next_height; block.previous_hash=blocks_.back().hash();
    block.timestamp=timestamp; block.transactions=transactions; block.state_root=candidate.state_root();
    out = std::move(block); next_state = std::move(candidate);
    return {true,{}};
}

void Chain::commit_block(Block block, State next_state) {
    state_ = std::move(next_state);
    blocks_.push_back(std::move(block));
}

ChainResult Chain::append_block(const std::vector<Transaction>& transactions, std::int64_t timestamp, std::int64_t max_timestamp) {
    Block block; State next;
    const ChainResult r = prepare_block(transactions, timestamp, max_timestamp, block, next);
    if (!r.ok) return r;
    commit_block(std::move(block), std::move(next));
    return {true,{}};
}
ChainResult Chain::verify(const Block& block, std::int64_t max_timestamp, State& candidate) const {
    if (blocks_.empty()) return {false,"genesis is not initialized"};
    const Block& tip = blocks_.back();
    if (tip.height == std::numeric_limits<std::uint64_t>::max()) return {false,"chain height exhausted"};
    if (block.protocol_version != PROTOCOL_VERSION) return {false,"unsupported block protocol version"};
    if (block.chain_id != CHAIN_ID) return {false,"block has wrong chain ID"};
    if (block.height != tip.height + 1) return {false,"unexpected block height"};
    if (block.previous_hash != tip.hash()) return {false,"previous hash does not match chain tip"};
    // v9: producer rules. A chain without a producer set must not carry producer fields, so an unsigned v8
    // chain and a chain that silently claims authorship never verify the same way.
    if (producers_.empty()) {
        if (block.producer != Address{} || block.producer_pubkey != PubKey{} || block.producer_signature != Signature{})
            return {false,"this chain has no producer set, so a block must not claim a producer"};
    } else {
        if (block.producer != producers_[(block.height - 1) % producers_.size()])
            return {false,"block is not signed by the producer expected for this height"};
        if (address_from_pubkey(block.producer_pubkey) != block.producer)
            return {false,"the block public key does not match its producer address"};
        const auto msg = block_signable(block.chain_id, network_id(), block.height, block.hash());
        if (!taz::verify(block.producer_pubkey, msg, block.producer_signature))
            return {false,"block signature does not verify"};
    }
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
ChainResult Chain::truncate_to(std::uint64_t height) {
    if (blocks_.empty()) return {false, "the chain is empty"};
    if (height > this->height()) return {false, "truncate_to cannot grow a chain"};
    if (height == this->height()) return {true, {}};
    if (genesis_balances_.size() != 2) return {false, "cannot rebuild: the genesis accounts were not recorded"};

    Chain fresh;
    if (!fresh.init_genesis(genesis_balances_[0].first, genesis_balances_[0].second,
                            genesis_balances_[1].first, genesis_balances_[1].second, producers_))
        return {false, "cannot rebuild: the genesis block was refused"};
    for (std::uint64_t h = 1; h <= height; ++h) {
        const ChainResult r = fresh.accept_block(blocks_[h], NO_TIMESTAMP_LIMIT);
        if (!r.ok) return {false, "cannot rebuild: " + r.error};
    }
    *this = std::move(fresh);
    return {true, {}};
}

ChainResult Chain::accept_block(const Block& block, std::int64_t max_timestamp) {
    State candidate;
    auto r = verify(block, max_timestamp, candidate);
    if (!r.ok) return r;
    state_ = std::move(candidate); blocks_.push_back(block); return {true,{}};
}

ChainResult Chain::set_producers(std::vector<Address> producers) {
    if (!producers_.empty()) return {false, "the producer set is already set"};
    if (blocks_.size() > 1) return {false, "the producer set must be set before the first block"};
    if (producers.empty()) return {false, "the producer set cannot be empty"};
    if (producers.size() > 64) return {false, "too many producers"};
    for (std::size_t i = 0; i < producers.size(); ++i) {
        if (producers[i] == Address{}) return {false, "a producer cannot be the zero address"};
        for (std::size_t j = i + 1; j < producers.size(); ++j)
            if (producers[i] == producers[j]) return {false, "the same producer is listed twice"};
    }
    producers_ = std::move(producers);
    // v9 P1: block 0 carries the genesis commitment, and that commitment now covers the producer list. Called
    // before the first block (the only time this is allowed) there is nothing else to invalidate, so the
    // commitment is recomputed here — otherwise a chain built with init_genesis() then set_producers() would
    // hold a block 0 that commits no producers while genesis_commitment() claims it does, and every reload
    // would be refused.
    if (blocks_.size() == 1 && genesis_balances_.size() == 2)
        blocks_[0].previous_hash = genesis_commitment_of(genesis_balances_[0].first, genesis_balances_[0].second,
                                                         genesis_balances_[1].first, genesis_balances_[1].second, producers_);
    state_.bind_network(network_id());      // block 0's previous_hash is the network ID, and it just changed
    return {true, {}};
}

ChainResult Chain::adopt_producer_set(std::vector<Address> producers) {
    if (!producers_.empty()) return {false, "the producer set is already set"};
    if (producers.empty() || producers.size() > 64) return {false, "bad producer set size"};
    for (std::size_t i = 0; i < producers.size(); ++i) {
        if (producers[i] == Address{}) return {false, "a producer cannot be the zero address"};
        for (std::size_t j = i + 1; j < producers.size(); ++j)
            if (producers[i] == producers[j]) return {false, "the same producer is listed twice"};
    }
    for (const Block& b : blocks_) {
        if (b.height == 0) continue;
        if (b.producer != producers[(b.height - 1) % producers.size()])
            return {false, "a block names a producer the round robin does not expect"};
    }
    producers_ = std::move(producers);
    return {true, {}};
}

ChainResult Chain::append_block_signed(const std::vector<Transaction>& transactions, std::int64_t timestamp,
                                       const PrivKey& producer_key, const PubKey& producer_pubkey,
                                       std::int64_t max_timestamp) {
    if (producers_.empty()) return {false, "this chain has no producer set"};
    if (address_from_pubkey(producer_pubkey) != producers_[height() % producers_.size()])
        return {false, "this key is not the producer expected for the next height"};
    // v9 P4: build the block, sign it, and only then commit. A failed signature leaves the chain untouched —
    // there is no ordering in which a block is committed unsigned.
    Block block; State next;
    const ChainResult r = prepare_block(transactions, timestamp, max_timestamp, block, next);
    if (!r.ok) return r;
    block.producer = address_from_pubkey(producer_pubkey);
    block.producer_pubkey = producer_pubkey;
    const auto msg = block_signable(block.chain_id, network_id(), block.height, block.hash());
    if (!sign(producer_key, msg, block.producer_signature)) return {false, "the block could not be signed"};
    commit_block(std::move(block), std::move(next));
    return {true, {}};
}

Hash32 genesis_commitment_of(const Address& a0, Amount b0, const Address& a1, Amount b1) {
    return genesis_commitment_of(a0, b0, a1, b1, {});
}

Hash32 genesis_commitment_of(const Address& a0, Amount b0, const Address& a1, Amount b1,
                             const std::vector<Address>& producers) {
    // Canonical: the two accounts are sorted by address, so the order they were given in cannot matter.
    std::pair<Address, Amount> e[2] = {{a0, b0}, {a1, b1}};
    if (e[1].first < e[0].first) std::swap(e[0], e[1]);
    std::vector<std::uint8_t> bytes;
    // v9: a chain with producers commits them too, under its own domain, so a v8 chain keeps its exact value.
    constexpr std::string_view domain_v2 = "TAZVERA-GENESIS-V2";
    constexpr std::string_view domain_v3 = "TAZVERA-GENESIS-V3";
    const std::string_view domain = producers.empty() ? domain_v2 : domain_v3;
    bytes.insert(bytes.end(), domain.begin(), domain.end());
    const std::string_view cid = CHAIN_ID;
    bytes.insert(bytes.end(), cid.begin(), cid.end()); bytes.push_back(0);
    for (int i = 3; i >= 0; --i) bytes.push_back(static_cast<std::uint8_t>((PROTOCOL_VERSION >> (8 * i)) & 0xff));
    for (const auto& [addr, bal] : e) {
        bytes.insert(bytes.end(), addr.begin(), addr.end());
        for (int i = 7; i >= 0; --i) bytes.push_back(static_cast<std::uint8_t>((bal >> (8 * i)) & 0xff));
    }
    if (!producers.empty()) {
        const std::uint32_t n = static_cast<std::uint32_t>(producers.size());
        for (int i = 3; i >= 0; --i) bytes.push_back(static_cast<std::uint8_t>((n >> (8 * i)) & 0xff));
        for (const Address& p : producers) bytes.insert(bytes.end(), p.begin(), p.end());   // order matters
    }
    return sha256(bytes);
}
Hash32 Chain::genesis_commitment() const {
    if (genesis_balances_.size() != 2) return Hash32{};
    return genesis_commitment_of(genesis_balances_[0].first, genesis_balances_[0].second,
                                 genesis_balances_[1].first, genesis_balances_[1].second, producers_);
}

bool Chain::init_genesis(const Address& first, Amount first_balance,
                         const Address& second, Amount second_balance, std::vector<Address> producers) {
    if (producers.empty()) return init_genesis(first, first_balance, second, second_balance);
    // The set is validated exactly as set_producers() validates it, before anything is committed.
    if (!blocks_.empty() || first == second) return false;
    const ChainResult pr = set_producers(std::move(producers));
    if (!pr.ok) return false;
    State genesis_state; genesis_state.set_balance(first, first_balance); genesis_state.set_balance(second, second_balance);
    Block genesis; genesis.height = 0; genesis.timestamp = 0; genesis.state_root = genesis_state.state_root();
    genesis.previous_hash = genesis_commitment_of(first, first_balance, second, second_balance, producers_);
    state_ = genesis_state; blocks_.push_back(genesis);
    state_.bind_network(genesis.previous_hash);
    genesis_balances_.clear();
    genesis_balances_.emplace_back(first, first_balance);
    genesis_balances_.emplace_back(second, second_balance);
    return true;
}
}
