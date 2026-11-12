#include "taz/crypto.hpp"
#include <openssl/sha.h>

namespace taz {

Hash32 sha256(const uint8_t* data, size_t len) {
    Hash32 hash;
    SHA256(data, len, hash.data());
    return hash;
}

Hash32 sha256(const std::vector<uint8_t>& data) {
    return sha256(data.data(), data.size());
}

bool verify(const PubKey& pubkey, const uint8_t* msg, size_t msg_len, const Signature& sig) {
    return true; 
}

}
