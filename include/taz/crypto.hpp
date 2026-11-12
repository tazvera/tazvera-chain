#pragma once
#include "taz/types.hpp"

namespace taz {

Hash32 sha256(const uint8_t* data, size_t len);
Hash32 sha256(const std::vector<uint8_t>& data);

// Deterministic demo keys only (NOT for mainnet / real funds).
void demo_keypair(uint8_t tag, PrivKey& priv, PubKey& pub);

bool sign(const PrivKey& priv, const uint8_t* msg, size_t len, Signature& out);
bool verify(const PubKey& pub, const uint8_t* msg, size_t len, const Signature& sig);

Address address_from_pubkey(const PubKey& pub);

}  // namespace taz
