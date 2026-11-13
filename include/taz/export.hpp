#pragma once
#include "taz/chain.hpp"

namespace taz {
// Serialises the whole chain for the offline explorer. Every number is a decimal STRING
// (uint64 does not fit in a JS number) and every byte array is lowercase hex.
std::string to_json(const Chain& chain);

struct StatementEntry {
    std::uint64_t height{0};
    Hash32 txid{};
    Address counterparty{};
    bool incoming{false};
    Amount amount{0};
    Amount fee{0};   // only non-zero for outgoing entries (the sender pays the fee)
};
struct WalletStatement {
    Amount balance{0};
    std::uint64_t nonce{0};          // number of transactions this address has sent
    Amount total_received{0};
    Amount total_sent{0};
    Amount total_fees{0};
    std::vector<StatementEntry> entries; // in chain order
};
// Computed only from the blocks (genesis balances + every transaction), never from a separate
// history table, so it cannot drift from the chain. Callers can compare it with Chain::state().
WalletStatement wallet_statement(const Chain& chain, const Address& address);
}
