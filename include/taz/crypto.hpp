#pragma once
#include "taz/types.hpp"
#include <optional>
#include <span>

namespace taz {
Hash32 sha256(std::span<const std::uint8_t> data);
Hash32 sha256(const std::vector<std::uint8_t>& data);
bool generate_keypair(PrivKey& private_seed, PubKey& public_key);
bool sign(const PrivKey& private_seed, std::span<const std::uint8_t> message, Signature& signature);
bool verify(const PubKey& public_key, std::span<const std::uint8_t> message, const Signature& signature);
Address address_from_pubkey(const PubKey& public_key);
std::string to_hex(std::span<const std::uint8_t> bytes);
}
