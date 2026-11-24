#pragma once
#include "taz/block.hpp"
#include "taz/crypto.hpp"
#include "taz/state.hpp"
#include <limits>
#include <utility>
namespace taz {
struct ChainResult { bool ok{false}; std::string error; };

inline constexpr std::int64_t NO_TIMESTAMP_LIMIT = std::numeric_limits<std::int64_t>::max();

Hash32 genesis_commitment_of(const Address&, Amount, const Address&, Amount);
// v9: when a chain declares producers, the list is committed as well — in the order given, because that order
// IS the round-robin schedule. An empty list keeps the v8 commitment (domain V2) byte for byte.
Hash32 genesis_commitment_of(const Address&, Amount, const Address&, Amount, const std::vector<Address>& producers);
class Chain {
public:
    bool init_genesis(const Address& first, Amount first_balance,
                      const Address& second, Amount second_balance);
    // v9: the same, with the producer list committed into the genesis commitment. An empty list behaves
    // exactly like the call above, so nothing written before v9 changes.
    bool init_genesis(const Address& first, Amount first_balance,
                      const Address& second, Amount second_balance, std::vector<Address> producers);

    // Build and commit a block from local transactions.
    // Timestamps must be strictly increasing and <= max_timestamp. The caller supplies the
    // upper bound (e.g. wall clock + allowed drift) so this class stays deterministic.
    ChainResult append_block(const std::vector<Transaction>& transactions, std::int64_t timestamp,
                             std::int64_t max_timestamp = NO_TIMESTAMP_LIMIT);

    // v9: the producer set. Empty means "this chain does not use producer signatures", and then every block
    // must leave the producer fields zero — which is how a v8 chain keeps verifying exactly as before.
    // Round robin: the first block after genesis (height 1) is made by producers[0], and block h by
    // producers[(h - 1) % producers.size()].
    const std::vector<Address>& producers() const noexcept { return producers_; }
    // Only before the first non-genesis block; rejects an empty list, the zero address and duplicates.
    ChainResult set_producers(std::vector<Address> producers);
    // Loader path: adopt a set derived from blocks that are already in the chain, after checking that every
    // block names the producer the round robin would expect.
    ChainResult adopt_producer_set(std::vector<Address> producers);
    // Append a block and sign it as the producer expected for that height.
    ChainResult append_block_signed(const std::vector<Transaction>& transactions, std::int64_t timestamp,
                                    const PrivKey& producer_key, const PubKey& producer_pubkey,
                                    std::int64_t max_timestamp = NO_TIMESTAMP_LIMIT);

    // Fully check a block produced elsewhere WITHOUT changing the chain: version, chain ID,
    // height, previous hash, timestamp, block size, every transaction, and the claimed state root.
    ChainResult validate_block(const Block& block,
                               std::int64_t max_timestamp = NO_TIMESTAMP_LIMIT) const;
    // v9.0: drop every block above `height` and rebuild the state from genesis. This is what makes a fork
    // choice real: without a way back, "the other chain wins" is only a sentence. Rebuilding from genesis is
    // deliberately the slow, obviously-correct way — a snapshot would be faster and a new thing to get wrong.
    ChainResult truncate_to(std::uint64_t height);

    // validate_block + commit. On any failure the chain is unchanged.
    ChainResult accept_block(const Block& block,
                             std::int64_t max_timestamp = NO_TIMESTAMP_LIMIT);

    // Protocol 4: the identity of this network. It is the genesis commitment and it is the previous_hash of block 0, so it
    // is covered by every block hash; transactions and producer signatures sign it.
    Hash32 network_id() const noexcept { return blocks_.empty() ? Hash32{} : blocks_[0].previous_hash; }
    std::uint64_t height() const noexcept { return blocks_.empty() ? 0 : blocks_.back().height; }
    const State& state() const noexcept { return state_; }
    const std::vector<Block>& blocks() const noexcept { return blocks_; }
    // Non-zero genesis balances, in the order they were given (needed to re-verify the chain offline).
    const std::vector<std::pair<Address, Amount>>& genesis_balances() const noexcept { return genesis_balances_; }
    // Canonical commitment over ordered genesis (address||balance). Verify out-of-band for integrity.
    Hash32 genesis_commitment() const;
private:
    // v9 P4: build a block without touching the chain, so a caller can sign it before anything is committed.
    // append_block() is prepare + commit; append_block_signed() is prepare + sign + commit, which leaves no
    // path where a block is committed and then fails to be signed.
    ChainResult prepare_block(const std::vector<Transaction>& transactions, std::int64_t timestamp,
                              std::int64_t max_timestamp, Block& out, State& next_state) const;
    void commit_block(Block block, State next_state);
    ChainResult check_timestamp(std::int64_t timestamp, std::int64_t max_timestamp) const;
    ChainResult execute(const std::vector<Transaction>& txs, std::uint64_t height, State& candidate) const;
    ChainResult verify(const Block& block, std::int64_t max_timestamp, State& candidate) const;
    std::vector<Block> blocks_;
    std::vector<std::pair<Address, Amount>> genesis_balances_;
    std::vector<Address> producers_;
    State state_;
};
}
