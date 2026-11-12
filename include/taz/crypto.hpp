#ifndef TAZ_CRYPTO_HPP
#define TAZ_CRYPTO_HPP

#include "taz/types.hpp"
#include <vector>

namespace taz {
Hash32 sha256(const uint8_t* data, size_t len);
Hash32 sha256(const std::vector<uint8_t>& data);
bool verify(const PubKey& pubkey, const uint8_t* msg, size_t msg_len, const Signature& sig);
}

#endif
