#pragma once
#include "taz/block.hpp"
#include "taz/tx.hpp"
#include <optional>
#include <string>

namespace taz {

// Canonical JSON (fixed key order, no spaces). Round-trip must recompute same txid/hash.
std::string tx_to_json(const Transaction& tx);
std::optional<Transaction> tx_from_json(const std::string& json);

std::string block_to_json(const Block& block);
std::optional<Block> block_from_json(const std::string& json);

// Helpers exposed for tests
std::string hash_to_hex(const Hash32& h);
bool hash_from_hex(const std::string& hex, Hash32& out);

}  // namespace taz
