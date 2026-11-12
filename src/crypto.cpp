#include "taz/crypto.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <cstring>
#include <stdexcept>

namespace taz {

Hash32 sha256(const uint8_t* data, size_t len) {
  Hash32 out{};
  SHA256(data, len, out.data());
  return out;
}

Hash32 sha256(const std::vector<uint8_t>& data) {
  return sha256(data.data(), data.size());
}

Address address_from_pubkey(const PubKey& pub) {
  auto h = sha256(pub.data(), pub.size());
  Address a{};
  std::memcpy(a.data(), h.data(), 20);
  return a;
}

void demo_keypair(uint8_t tag, PrivKey& priv, PubKey& pub) {
  // Deterministic, disposable keys for local prototype ONLY.
  std::array<uint8_t, 32> seed{};
  seed.fill(tag);
  seed[0] = tag;
  seed[31] = static_cast<uint8_t>(0xA5 ^ tag);

  EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
  if (!pkey) throw std::runtime_error("demo_keypair: EVP_PKEY_new_raw_private_key failed");

  size_t priv_len = 32;
  if (EVP_PKEY_get_raw_private_key(pkey, priv.data(), &priv_len) != 1 || priv_len != 32) {
    EVP_PKEY_free(pkey);
    throw std::runtime_error("demo_keypair: get private failed");
  }
  size_t pub_len = 32;
  if (EVP_PKEY_get_raw_public_key(pkey, pub.data(), &pub_len) != 1 || pub_len != 32) {
    EVP_PKEY_free(pkey);
    throw std::runtime_error("demo_keypair: get public failed");
  }
  EVP_PKEY_free(pkey);
}

bool sign(const PrivKey& priv, const uint8_t* msg, size_t len, Signature& out) {
  EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, priv.data(), priv.size());
  if (!pkey) return false;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) {
    EVP_PKEY_free(pkey);
    return false;
  }
  bool ok = false;
  size_t siglen = out.size();
  if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
      EVP_DigestSign(ctx, out.data(), &siglen, msg, len) == 1 && siglen == out.size()) {
    ok = true;
  }
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  return ok;
}

bool verify(const PubKey& pub, const uint8_t* msg, size_t len, const Signature& sig) {
  EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), pub.size());
  if (!pkey) return false;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) {
    EVP_PKEY_free(pkey);
    return false;
  }
  bool ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
            EVP_DigestVerify(ctx, sig.data(), sig.size(), msg, len) == 1;
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  return ok;
}

}  // namespace taz
