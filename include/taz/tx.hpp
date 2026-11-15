#pragma once
#include "taz/types.hpp"
#include <optional>

namespace taz {
enum class TxKind : std::uint8_t {
    Transfer = 0,        // move funds (may be held for a while, see SafetyConfig)
    SetSafety = 1,       // set the account's own spending rules
    CancelHeld = 2,      // take back a transfer that is still being held
    SetGuardians = 3,    // name the trusted people who may help recover this account
    RecoveryVote = 4,    // a guardian votes to move someone's account to a new address
    RecoveryCancel = 5,  // the owner stops a recovery that is voting or waiting
};
struct Transaction {
    std::uint32_t protocol_version{PROTOCOL_VERSION};
    std::string chain_id{CHAIN_ID};
    Address from{};
    Address to{};            // Transfer: receiver. RecoveryVote: the account that is to be recovered
    Amount amount{0};        // Transfer only
    Amount fee{FEE_TRANSFER_V1};
    std::uint64_t nonce{0};
    std::uint64_t expiry_height{0}; // zero means no expiry in this prototype
    PubKey pubkey{};
    Signature signature{};
    TxKind kind{TxKind::Transfer};
    Amount daily_limit{0};          // SetSafety: max amount sent per window, 0 = no limit
    Amount hold_threshold{0};       // SetSafety: transfers >= this are held, 0 = never hold
    std::uint64_t hold_blocks{0};   // SetSafety: how long a held transfer waits
    Hash32 ref{};                   // CancelHeld: txid of the held transfer
    Address new_owner{};            // RecoveryVote: the address the funds should move to
    std::vector<Address> guardians; // SetGuardians: strictly ascending, at most MAX_GUARDIANS
    std::uint64_t guardian_threshold{0}; // SetGuardians: how many of them must agree
    std::vector<std::uint8_t> signable_bytes() const;
    Hash32 txid() const;
};
std::optional<std::string> validate_tx_shape(const Transaction& tx);
}
