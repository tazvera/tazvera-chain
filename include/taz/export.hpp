#pragma once
#include "taz/chain.hpp"

namespace taz {
// Serialises the whole chain for the offline explorer. Every number is a decimal STRING
// (uint64 does not fit in a JS number) and every byte array is lowercase hex.
// A chain that only contains plain transfers is written as format "v1"; one that uses the
// safety rules is written as "v2" (older explorers refuse v2 instead of mis-verifying it).
std::string to_json(const Chain& chain);

enum class EntryKind {
    Sent,        // transfer that arrived at once
    Received,    // transfer received at once
    HeldOut,     // transfer sent but still waiting (funds are inside the hold)
    ReleasedIn,  // a held transfer that has now arrived
    Cancelled,   // the owner took back a held transfer
    Refunded,    // a hold could not be delivered and went back to the sender
    SafetyChanged,
};
struct StatementEntry {
    std::uint64_t height{0};
    Hash32 txid{};
    Address counterparty{};
    EntryKind kind{EntryKind::Sent};
    Amount amount{0};
    Amount fee{0};
};
struct WalletStatement {
    Amount balance{0};               // spendable now
    std::uint64_t nonce{0};          // transactions this address has sent
    Amount total_received{0};        // Received + ReleasedIn
    Amount total_sent{0};            // Sent + HeldOut (gross)
    Amount total_refunded{0};        // Cancelled + Refunded
    Amount total_fees{0};
    Amount held_outgoing{0};         // still waiting, sent by this address
    Amount held_incoming{0};         // still waiting, on its way to this address
    std::vector<StatementEntry> entries; // in chain order
};
// Computed only from the genesis balances and the blocks, by replaying them, never from a separate
// history table, so it cannot drift from the chain. Tests compare it with Chain::state().
WalletStatement wallet_statement(const Chain& chain, const Address& address);
}
