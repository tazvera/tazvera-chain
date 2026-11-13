#include "taz/crypto.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace taz {
namespace {
using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
}
void secure_wipe(std::span<std::uint8_t> bytes) noexcept {
    if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
}
Hash32 sha256(std::span<const std::uint8_t> data) {
    Hash32 out{};
    unsigned int length = 0;
    if (EVP_Digest(data.data(), data.size(), out.data(), &length, EVP_sha256(), nullptr) != 1 || length != out.size())
        throw std::runtime_error("SHA-256 failed");
    return out;
}
Hash32 sha256(const std::vector<std::uint8_t>& data) { return sha256(std::span<const std::uint8_t>(data)); }
bool generate_keypair(PrivKey& private_seed, PubKey& public_key) {
    if (RAND_bytes(private_seed.data(), static_cast<int>(private_seed.size())) != 1) {
        secure_wipe(private_seed);
        return false;
    }
    PkeyPtr key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, private_seed.data(), private_seed.size()), EVP_PKEY_free);
    if (!key) { secure_wipe(private_seed); return false; }
    std::size_t length = public_key.size();
    if (EVP_PKEY_get_raw_public_key(key.get(), public_key.data(), &length) != 1 || length != public_key.size()) {
        secure_wipe(private_seed);
        return false;
    }
    return true;
}
bool sign(const PrivKey& private_seed, std::span<const std::uint8_t> message, Signature& signature) {
    PkeyPtr key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, private_seed.data(), private_seed.size()), EVP_PKEY_free);
    MdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!key || !ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) return false;
    std::size_t length = signature.size();
    return EVP_DigestSign(ctx.get(), signature.data(), &length, message.data(), message.size()) == 1 && length == signature.size();
}
bool verify(const PubKey& public_key, std::span<const std::uint8_t> message, const Signature& signature) {
    PkeyPtr key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size()), EVP_PKEY_free);
    MdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!key || !ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) return false;
    return EVP_DigestVerify(ctx.get(), signature.data(), signature.size(), message.data(), message.size()) == 1;
}
Address address_from_pubkey(const PubKey& public_key) {
    const auto digest = sha256(std::span<const std::uint8_t>(public_key));
    Address address{};
    std::copy_n(digest.begin(), address.size(), address.begin());
    return address;
}
std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out; out.reserve(bytes.size() * 2);
    for (auto b : bytes) { out.push_back(digits[b >> 4]); out.push_back(digits[b & 0x0f]); }
    return out;
}
}
