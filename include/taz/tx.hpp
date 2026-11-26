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
    RecoveryCancel = 5,  // stop a recovery: the owner (to = zero) or, if one is set, the account's veto key (to = the account)
    RegisterAsset = 6,   // RWA registry, part 1: record that an asset exists and who owns it
    TransferAsset = 7,   // RWA registry, part 2: the owner gives an asset to another address (ref = asset_id, to = new owner, owner = current owner)
};
// RegisterAsset: what kind of asset is being registered.
enum class AssetKind : std::uint8_t { Land = 1, Vehicle = 2, Invoice = 3, Other = 4 };
struct Transaction {
    std::uint32_t protocol_version{PROTOCOL_VERSION};
    std::string chain_id{CHAIN_ID};
    Address from{};
    Address to{};            // Transfer: receiver. RecoveryVote: the account to recover. RecoveryCancel: zero (own account) or the account the veto key protects
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
    Hash32 ref{};                   // CancelHeld: txid of the held transfer; TransferAsset: the asset_id
    Address new_owner{};            // RecoveryVote: the address the funds should move to
    std::vector<Address> guardians; // SetGuardians: strictly ascending, at most MAX_GUARDIANS
    std::uint64_t guardian_threshold{0}; // SetGuardians: how many of them must agree
    Address veto{};                 // SetGuardians: optional separate (offline) key that alone may cancel a recovery; zero = none
    Address owner{};                // RegisterAsset: the owner being registered; TransferAsset: the current owner. Must be the signer (from)
    std::uint8_t asset_kind{0};     // RegisterAsset: an AssetKind value, 1..4
    Hash32 doc_hash{};              // RegisterAsset: SHA-256 of the document that proves the asset (the document itself stays off the chain)
    std::vector<std::uint8_t> signable_bytes() const;
    Hash32 txid() const;
};
std::optional<std::string> validate_tx_shape(const Transaction& tx);
// RegisterAsset: the identifier of the asset that this owner registers with this nonce on this chain:
//   SHA-256("TAZVERA-ASSET-V1" || chain_id || owner || u64be(nonce))   (all 32 bytes of the digest)
// chain_id is hashed as its raw bytes, with no length prefix (it is a fixed constant of the chain).
Hash32 asset_id(std::string_view chain_id, const Address& owner, std::uint64_t nonce);
}
