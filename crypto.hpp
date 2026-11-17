#pragma once
#include "taz/types.hpp"
#include <span>

namespace taz {
Hash32 sha256(std::span<const std::uint8_t> data);
Hash32 sha256(const std::vector<std::uint8_t>& data);
// On failure the seed buffer is wiped, so no unusable key material is left behind.
bool generate_keypair(PrivKey& private_seed, PubKey& public_key);
bool sign(const PrivKey& private_seed, std::span<const std::uint8_t> message, Signature& signature);
bool verify(const PubKey& public_key, std::span<const std::uint8_t> message, const Signature& signature);
Address address_from_pubkey(const PubKey& public_key);
std::string to_hex(std::span<const std::uint8_t> bytes);
// Overwrites secret bytes in a way the compiler cannot optimise away (OPENSSL_cleanse).
void secure_wipe(std::span<std::uint8_t> bytes) noexcept;
}
