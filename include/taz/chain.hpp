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
// F7: slot skipping. A chain may declare a slot length (seconds) in its genesis; 0 = off = the round robin of v9,
// byte for byte. Allowed range when on: 4 .. 86400 (the lower bound keeps the future-drift allowance, a quarter slot,
// at one second or more).
inline constexpr std::uint32_t MIN_SLOT_SECONDS = 4;
inline constexpr std::uint32_t MAX_SLOT_SECONDS = 86400;
// With slots, the slot length goes into the genesis commitment under its own domain, so a node that does not know
// about slots computes another commitment and refuses the chain instead of misreading it.
Hash32 genesis_commitment_of(const Address&, Amount, const Address&, Amount, const std::vector<Address>& producers,
                             std::uint32_t slot_seconds);
class Chain {
public:
    bool init_genesis(const Address& first, Amount first_balance,
                      const Address& second, Amount second_balance);
    // v9: the same, with the producer list committed into the genesis commitment. An empty list behaves
    // exactly like the call above, so nothing written before v9 changes.
    bool init_genesis(const Address& first, Amount first_balance,
                      const Address& second, Amount second_balance, std::vector<Address> producers);

    // F7: the same, with a slot length (see above). Refused: slot_seconds outside the range, or no producers.
    bool init_genesis(const Address& first, Amount first_balance,
                      const Address& second, Amount second_balance, std::vector<Address> producers,
                      std::uint32_t slot_seconds);
    std::uint32_t slot_seconds() const noexcept { return slot_seconds_; }
    // F7: the producer who may make the NEXT block if it carries this timestamp (the zero address if the chain has
    // no producers). Round robin by height; each full slot_seconds that passed since the parent block moves the turn
    // on by one producer.
    Address expected_producer(std::int64_t timestamp) const;
    // How far past "now" a live block may be stamped: 120 s without slots, a quarter slot (at most 120 s) with them.
    // A producer can claim a later slot only by stamping a later time, so this bound is what keeps it from skipping early.
    std::int64_t max_future_seconds() const;

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
    std::uint32_t slot_seconds_{0};
    State state_;
};
}
