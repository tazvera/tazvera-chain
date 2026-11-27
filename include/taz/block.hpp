#pragma once
#include "taz/tx.hpp"
namespace taz {
struct Block {
    std::uint32_t protocol_version{PROTOCOL_VERSION};
    std::string chain_id{CHAIN_ID};
    std::uint64_t height{0};
    Hash32 previous_hash{};
    std::int64_t timestamp{0};
    std::vector<Transaction> transactions;
    Hash32 state_root{};
    // v9: who made this block. Deliberately NOT part of hash(): adding it must not change a single existing
    // block hash, so a v8 chain keeps the exact hashes and state roots it had. The signature covers hash().
    Address producer{};
    PubKey producer_pubkey{};
    Signature producer_signature{};
    Hash32 hash() const;
};
}
