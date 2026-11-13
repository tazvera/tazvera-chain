#pragma once
#include "taz/types.hpp"
#include <optional>

namespace taz {
struct Transaction {
    std::uint32_t protocol_version{PROTOCOL_VERSION};
    std::string chain_id{CHAIN_ID};
    Address from{};
    Address to{};
    Amount amount{0};
    Amount fee{FEE_TRANSFER_V1};
    std::uint64_t nonce{0};
    std::uint64_t expiry_height{0}; // zero means no expiry in this prototype
    PubKey pubkey{};
    Signature signature{};
    std::vector<std::uint8_t> signable_bytes() const;
    Hash32 txid() const;
};
std::optional<std::string> validate_tx_shape(const Transaction& tx);
}
